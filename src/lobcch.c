#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#elif defined HAVE_DFP_STDLIB_H
# include <dfp/stdlib.h>
#elif defined HAVE_DECIMAL_H
# include <decimal.h>
#endif	/* DFP754_h || DFP_STDLIB_H || DECIMAL_H */
#include <ev.h>
#include <books/books.h>
#include "dfp754_d64.h"
#include "sock.h"
#include "hash.h"
#include "nifty.h"

#define MCAST_ADDR	"ff05::134"
#define MCAST_PORT	7978

#define strtopx		strtod64
#define strtoqx		strtod64
#define pxtostr		d64tostr
#define qxtostr		d64tostr

#define quantizepx	quantized64

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))

typedef long unsigned int tv_t;

typedef struct {
	quo_t q;
	const char *ins;
	size_t inz;
} xquo_t;

typedef struct {
	px_t b;
	px_t a;
	qx_t B;
	qx_t A;
} lvl_t;

#define NOT_A_XQUO	((xquo_t){NOT_A_QUO})
#define NOT_A_XQUO_P(x)	(NOT_A_QUO_P((x).q))

#define HX_CATCHALL	((hx_t)-1ULL)

/* book keeping, literally */
static hx_t *conx;
static const char **cont;
static book_t *book;
static size_t nbook;
static size_t zbook;
static size_t nctch;
/* the actual quotes 3U * NBOOK * NPER wide */
static px_t *hist;
static qx_t *hisq;
static size_t nper = 61U;
static size_t iper;

/* no parens around X to add additive access */
#define HIST(x)	hist[3U * nper * x]
#define HISQ(x)	hisq[3U * nper * x]


static __attribute__((format(printf, 1, 2))) void
serror(const char *fmt, ...)
{
	va_list vap;
	va_start(vap, fmt);
	vfprintf(stderr, fmt, vap);
	va_end(vap);
	if (errno) {
		fputc(':', stderr);
		fputc(' ', stderr);
		fputs(strerror(errno), stderr);
	}
	fputc('\n', stderr);
	return;
}

static inline size_t
memncpy(char *restrict tgt, const char *src, size_t zrc)
{
	memcpy(tgt, src, zrc);
	return zrc;
}

static void
init_hist(size_t n)
{
	for (size_t i = 0U; i < 3U * nper; i += 3U) {
		HIST(n + i) = 0.dd;
		HISQ(n + i) = 0.dd;
	}
	return;
}


/* serialiser and deserialiser */
static xquo_t
recv_quo(const char *msg, size_t msz)
{
	switch (msg[0U]) {
		const char *ins, *eoi;
		char *on;
		quo_t q;

	case 'A':
		if (!memcmp(msg, "AUC\t", 4U)) {
			break;
		}
	case 'B':
		if (UNLIKELY(msg[2U] != '\t')) {
			break;
		}
		/* snarf instrument */
		ins = msg + 3U;
		eoi = memchr(msg + 3U, '\t', msz - 3U);
		msz -= eoi + 1U - msg;
		if (UNLIKELY(eoi == NULL)) {
			break;
		}
		q.s = (side_t)(msg[0U] ^ '@');
		q.f = (typeof(q.f))(msg[1U] ^ '0');
	pq:
		q.p = strtopx(eoi + 1U, &on);
		if (*on++ != '\t') {
			break;
		}
		q.q = strtoqx(on, &on);
		return (xquo_t){q, ins, eoi - ins};

	case 'T':
		/* should be a trade */
		if (UNLIKELY(msg[3U] != '\t')) {
			break;
		}
		ins = msg + 4U;
		eoi = memchr(msg + 4U, '\t', msz - 4U);
		if (UNLIKELY(eoi == NULL)) {
			break;
		}
		q.s = SIDE_DEL;
		goto pq;
		
	default:
		break;
	}
	return NOT_A_XQUO;
}

static px_t
snap_tra(px_t t, qx_t T)
{
	return quantizepx(t / T, t);
}

static lvl_t
snap_top(book_t bk)
{
	lvl_t r = {NANPX, NANPX, 0.dd, 0.dd};

	with (book_iter_t i = {.b = bk.BOOK(SIDE_BID)}) {
		if (!book_iter_next(&i)) {
			break;
		}
		r.b = i.p;
		r.B = i.q;
	}

	with (book_iter_t i = {.b = bk.BOOK(SIDE_ASK)}) {
		if (!book_iter_next(&i)) {
			break;
		}
		r.a = i.p;
		r.A = i.q;
	}
	return r;
}


static void
sigint_cb(EV_P_ ev_signal *UNUSED(w), int UNUSED(revents))
{
	fsync(STDOUT_FILENO);
	ev_unloop(EV_A_ EVUNLOOP_ALL);
	return;
}

static void
beef_cb(EV_P_ ev_io *w, int UNUSED(revents))
{
	static char buf[4096U];
	static size_t bof;
	size_t npr = 0U;
	ssize_t nrd;

	nrd = recv(w->fd, buf + bof, sizeof(buf) - bof, 0);
	if (UNLIKELY(nrd <= 0)) {
		return;
	}
	/* up the bof */
	bof += nrd;
	/* snarf the line */
	for (const char *x;
	     (x = memchr(buf + npr, '\n', bof - npr));
	     npr = x + 1U - buf) {
		xquo_t q = recv_quo(buf + npr, x + 1U - (buf + npr));
		size_t k;
		hx_t hx;

		if (NOT_A_XQUO_P(q)) {
			continue;
		}
		/* check if we've got him in our books */
		if (nbook || zbook) {
			hx = hash(q.ins, q.inz);
			for (k = 0U; k < nbook; k++) {
				if (conx[k] == hx) {
					goto snap;
				}
			}
		}
		if (nctch) {
			goto snap;
		} else if (!zbook) {
			/* ok, it's not for us */
			continue;
		} else if (UNLIKELY(nbook >= zbook)) {
			/* resize */
			zbook *= 2U;
			cont = realloc(cont, zbook * sizeof(*cont));
			conx = realloc(conx, zbook * sizeof(*conx));
			book = realloc(book, zbook * sizeof(*book));
			hist = realloc(hist, 3U * nper * zbook * sizeof(*hist));
			hisq = realloc(hisq, 3U * nper * zbook * sizeof(*hisq));
		}
		/* initialise the book */
		cont[nbook] = strndup(q.ins, q.inz);
		conx[nbook] = hx;
		book[nbook] = make_book();
		init_hist(nbook);
		nbook++;
	snap:
		switch (q.q.s) {
		case SIDE_ASK:
		case SIDE_BID:
			(void)book_add(book[k], q.q);
			break;
		case SIDE_DEL/*TRA*/:
			HIST(k + 3U * iper) += q.q.p * q.q.q;
			HISQ(k + 3U * iper) += q.q.q;
			break;
		default:
			break;
		}
	}
	/* move left-overs */
	if (bof > npr) {
		memmove(buf, buf + npr, bof - npr);
		bof -= npr;
	} else {
		bof = 0U;
	}
	return;
}

static void
snap_cb(EV_P_ ev_timer *UNUSED(w), int UNUSED(revents))
{
	for (size_t i = 0U; i < nbook; i++) {
		lvl_t l = snap_top(book[i]);
		px_t t = snap_tra(HIST(i + 3U * iper), HISQ(i + 3U * iper));

		HIST(i + 3U * iper + SIDE_UNK) = t;
		HIST(i + 3U * iper + SIDE_BID) = l.b;
		HIST(i + 3U * iper + SIDE_ASK) = l.a;
		HISQ(i + 3U * iper + SIDE_BID) = l.B;
		HISQ(i + 3U * iper + SIDE_ASK) = l.A;
	}
	iper = (iper + 1U) % nper;
	return;
}


#include "lobcch.yucc"

int
main(int argc, char *argv[])
{
/* grep -F 'EURUSD FAST Curncy' | cut -f1,5,6 */
	static yuck_t argi[1U];
	static struct ipv6_mreq r[3U];
	ev_signal sigint_watcher[1U];
	ev_io beef[1U];
	ev_timer snap[1U];
	/* use the default event loop unless you have special needs */
	struct ev_loop *loop;
	int rc = EXIT_SUCCESS;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if ((nbook = argi->nargs)) {
		size_t j = 0U;

		/* initialise hash array and books */
		cont = malloc(nbook * sizeof(*cont));
		conx = malloc(nbook * sizeof(*conx));
		book = malloc(nbook * sizeof(*book));
		hist = malloc(3U * nper * nbook * sizeof(*hist));
		hisq = malloc(3U * nper * nbook * sizeof(*hisq));

		for (size_t i = 0U; i < nbook; i++) {
			const char *this = argi->args[i];
			const size_t conz = strlen(this);

			if (UNLIKELY(conz == 0U ||
				     conz == 1U && *this == '*')) {
				/* catch-all hash */
				continue;
			}
			cont[j] = this;
			conx[j] = hash(this, conz);
			book[j] = make_book();
			init_hist(j);
			j++;
		}
		if (j < nbook) {
			/* oh, there's been catch-alls,
			 * shrink NBOOK and initialise last cell */
			nbook = j;
			cont[nbook] = nbook ? "ALL" : NULL;
			conx[nbook] = HX_CATCHALL;
			book[nbook] = make_book();
			init_hist(nbook);
			nctch = 1U;
		}
	} else {
		/* allocate some 8U books */
		zbook = 8U;
		cont = malloc(zbook * sizeof(*cont));
		conx = malloc(zbook * sizeof(*conx));
		book = malloc(zbook * sizeof(*book));
		hist = malloc(3U * nper * zbook * sizeof(*hist));
		hisq = malloc(3U * nper * zbook * sizeof(*hisq));
	}

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	/* initialise a sig C-c handler */
	ev_signal_init(sigint_watcher, sigint_cb, SIGINT);
	ev_signal_start(EV_A_ sigint_watcher);

	/* init the multicast socket */
	with (int s) {
		if (UNLIKELY((s = mc6_socket()) < 0)) {
			serror("\
Error: cannot open socket");
			rc = 1;
			goto nop;
		} else if (mc6_join_group(
				   r + 0U, s,
				   MCAST_ADDR, MCAST_PORT, NULL) < 0) {
			serror("\
Error: cannot join multicast group on socket %d", s);
			rc = 1;
			close(s);
			goto nop;
		}
		/* hook into our event loop */
		ev_io_init(beef, beef_cb, s, EV_READ);
		ev_io_start(EV_A_ beef);
	}

	/* intialise snapshot timer */
	ev_timer_init(snap, snap_cb, 1.0, 1.0);
	ev_timer_start(EV_A_ snap);

	/* now wait for events to arrive */
	ev_loop(EV_A_ 0);

	for (size_t i = 0U; i < nbook + nctch; i++) {
		puts(cont[i]);
		for (size_t j = iper + 1U; j < nper + iper; j++) {
			const size_t k = j % nper;
			char buf[256U];
			size_t len = 0U;

			len += pxtostr(
				buf + len, sizeof(buf) - len,
				HIST(i + 3U * k + SIDE_UNK));
			buf[len++] = ' ';
			len += pxtostr(
				buf + len, sizeof(buf) - len,
				HIST(i + 3U * k + SIDE_BID));
			buf[len++] = '/';
			len += pxtostr(
				buf + len, sizeof(buf) - len,
				HIST(i + 3U * k + SIDE_ASK));
			buf[len++] = '\n';
			fwrite(buf, 1, len, stdout);
		}
	}

	/* begin the freeing */
	with (int s = beef->fd) {
		ev_io_stop(EV_A_ beef);
		mc6_leave_group(s, r + 0U);
		setsock_linger(s, 1);
		close(s);
	}

	if (nbook + nctch) {
		for (size_t i = 0U; i < nbook + nctch; i++) {
			book[i] = free_book(book[i]);
		}
		if (zbook) {
			for (size_t i = 0U; i < nbook + nctch; i++) {
				free(deconst(cont[i]));
			}
		}
		free(cont);
		free(conx);
		free(book);
		free(hist);
		free(hisq);
	}

nop:
	/* destroy the default evloop */
	ev_default_destroy();

out:
	yuck_free(argi);
	return rc;
}
