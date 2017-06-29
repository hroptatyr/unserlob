#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <ev.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include <mkl_vsl.h>
#include <clob/clob.h>
#include <clob/unxs.h>
#include "dfp754_d64.h"
#include "sock.h"
#include "nifty.h"

#define NSECS	(1000000000)
#define MCAST_ADDR	"ff05::134"
#define QUOTE_PORT	7978
#define TRADE_PORT	7979
#define DEBUG_PORT	7977

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))

typedef _Decimal64 px_t;
typedef _Decimal64 qx_t;
typedef long unsigned int tv_t;
#define strtopx		strtod64
#define pxtostr		d64tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr

typedef struct {
	clob_side_t s;
	px_t p;
	qx_t q;
} quo_t;

static int exec_chan = -1;
static VSLStreamStatePtr rstr;
static double alpha = 1.;
static double sigma = 1.;
static double mktv = 0.;
static double sprd = 0.02;

static clob_oid_t coid[NSIDES];
static quo_t cquo[NSIDES];


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

static int
init_rng(uint64_t seed)
{
	vslNewStream(&rstr, VSL_BRNG_MT19937, seed);
	return 0;
}

static double
rnorm(double mean)
{
	static double r[512U];
	static size_t i = countof(r);

	if (UNLIKELY(i >= countof(r))) {
		/* get some more */
		vdRngGaussian(
			VSL_RNG_METHOD_GAUSSIAN_ICDF, rstr,
			countof(r), r, 0.0, sigma);
		i = 0U;
	}
	return r[i++] + mean;
}

static double
truv(void)
{
	static double truv;
	double eps = rnorm(alpha * (mktv - truv));

	return truv += eps;
}


typedef struct {
	enum {
		BMSG_UNK,
		BMSG_LVL1,
		BMSG_LVL2,
	} type;
	union {
		quo_t quo;
	};
} bmsg_t;

static bmsg_t
_read_beef_quo(const char *msg, size_t UNUSED(msz))
{
	bmsg_t r = {BMSG_UNK};

	switch (msg[1U]) {
	case '1':
		r.type = BMSG_LVL1;
		break;
	case '2':
		r.type = BMSG_LVL2;
		break;
	default:
		goto nil;
	}

	r.quo.s = (clob_side_t)(*msg - 'A');
	with (char *on) {
		r.quo.p = strtopx(msg + 3U, &on);
		if (*on++ != '\t') {
			goto nil;
		}
		r.quo.q = strtoqx(on, &on);
	}
	return r;
nil:
	return (bmsg_t){BMSG_UNK};
}

static bmsg_t
read_beef(const char *msg, size_t msz)
{
	fwrite(msg, 1, msz, stdout);

	switch (*msg) {
	case 'A':
	case 'B':
		return _read_beef_quo(msg, msz);
	case 'T':
	default:
		break;
	}
	return (bmsg_t){BMSG_UNK};
}


typedef struct {
	enum {
		CMSG_UNK,
		CMSG_OID,
		CMSG_ACC,
	} type;
	union {
		clob_oid_t oid;
		unxs_exa_t acc;
	};
} cmsg_t;

static cmsg_t
_read_cake_oid(const char *msg, size_t UNUSED(msz))
{
	cmsg_t r = {CMSG_OID};

	switch (msg[4U]) {
	case 'L':
		r.oid.typ = TYPE_LMT;
		break;
	case 'M':
		r.oid.typ = TYPE_MKT;
		break;
	default:
		goto nil;
	}
	r.oid.sid = (clob_side_t)(msg[8U] - 'A');
	with (char *on) {
		r.oid.prc = strtopx(msg + 12U, &on);
		if (*on++ != ' ') {
			goto nil;
		}
		/* and read the qid */
		r.oid.qid = strtoull(on, &on, 0);
	}
	return r;
nil:
	return (cmsg_t){CMSG_UNK};
}

static cmsg_t
read_cake(const char *msg, size_t msz)
{
	fwrite(msg, 1, msz, stdout);

	if (!memcmp(msg, "OID\t", 4U)) {
		return _read_cake_oid(msg, msz);
	}
	return (cmsg_t){CMSG_UNK};
}

static size_t
make_cancel(char *restrict buf, size_t bsz, clob_oid_t oi)
{
	static const char *sids[] = {"ASK ", "BID "};
	static const char *typs[] = {
		[TYPE_LMT] = "LMT ",
		[TYPE_MKT] = "MKT ",
	};
	size_t len = 0U;

	len += (memcpy(buf + len, "CAN\t", 4U), 4U);
	len += (memcpy(buf + len, typs[oi.typ], 4U), 4U);
	len += (memcpy(buf + len, sids[oi.sid], 4U), 4U);
	len += pxtostr(buf + len, bsz - len, oi.prc);
	buf[len++] = ' ';
	len += snprintf(buf + len, bsz - len, "%zu", oi.qid);
	buf[len++] = '\n';
	return len;
}


static void
sigint_cb(EV_P_ ev_signal *UNUSED(w), int UNUSED(re))
{
	ev_unloop(EV_A_ EVUNLOOP_ALL);
	return;
}

static void
beef_cb(EV_P_ ev_io *w, int UNUSED(re))
{
/* something went on in the quote channel */
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
	/* now snarf the line */
	for (const char *x;
	     (x = memchr(buf + npr, '\n', bof - npr));
	     npr = x + 1U - buf) {
		bmsg_t m = read_beef(buf + npr, x + 1U - (buf + npr));

		switch (m.type) {
		case BMSG_LVL1:
			cquo[m.quo.s] = m.quo;
			/* calc mktv as well */
			mktv = (cquo[SIDE_ASK].p + cquo[SIDE_BID].p) / 2.dd;
			break;
		default:
			break;
		}
	}
	/* move left-overs */
	if (bof - npr) {
		memmove(buf, buf + npr, bof - npr);
	}
	bof -= npr;
	return;
}

static void
cake_cb(EV_P_ ev_io *w, int UNUSED(re))
{
/* something went on in the exec channel */
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
	/* now snarf the line */
	for (const char *x;
	     (x = memchr(buf + npr, '\n', bof - npr));
	     npr = x + 1U - buf) {
		cmsg_t m = read_cake(buf + npr, x + 1U - (buf + npr));

		switch (m.type) {
		case CMSG_OID:
			coid[m.oid.sid] = m.oid;
			break;
		default:
			break;
		}
	}
	/* move left-overs */
	if (bof - npr) {
		memmove(buf, buf + npr, bof - npr);
	}
	bof -= npr;
	return;
}

static void
hbeat_cb(EV_P_ ev_timer *UNUSED(w), int UNUSED(re))
{
/* generate a random trade */
	char buf[256U];
	int len = 0;
	double v = truv();
	unsigned int q = 5;

	/* cancel old guys */
	if (coid[SIDE_BID].qid) {
		len += make_cancel(buf + len, sizeof(buf) - len, coid[SIDE_BID]);
	}
	if (coid[SIDE_ASK].qid) {
		len += make_cancel(buf + len, sizeof(buf) - len, coid[SIDE_ASK]);
	}

	len += snprintf(buf + len, sizeof(buf) - len,
			"BUY\t%u00\t%.2f\n", q, v - sprd / 2);
	len += snprintf(buf + len, sizeof(buf) - len,
			"SELL\t%u00\t%.2f\n", q, v + sprd / 2);
	send(exec_chan, buf, len, 0);
	fwrite(buf, 1, len, stdout);

	printf("%f\n", v);
	return;
}


#include "egobot.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	static struct ipv6_mreq r;
	ev_signal sigint_watcher[1U];
	ev_signal sigterm_watcher[1U];
	ev_timer hbeat[1U];
	ev_io beef[1U];
	ev_io cake[1U];
	const char *host = "localhost";
	/* use the default event loop unless you have special needs */
	struct ev_loop *loop;
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->host_arg) {
		host = argi->host_arg;
	}

	init_rng(0ULL);

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	/* initialise a sig C-c handler */
	ev_signal_init(sigint_watcher, sigint_cb, SIGINT);
	ev_signal_start(EV_A_ sigint_watcher);
	ev_signal_init(sigterm_watcher, sigint_cb, SIGTERM);
	ev_signal_start(EV_A_ sigterm_watcher);

	/* open exec channel */
	if (UNLIKELY((exec_chan = connector(host, TRADE_PORT)) < 0)) {
		serror("\
Error: cannot open socket for execution messages");
		rc = 1;
		goto nop;
	}
	/* hook into our event loop */
	ev_io_init(cake, cake_cb, exec_chan, EV_READ);
	ev_io_start(EV_A_ cake);

	/* init the quote channel */
	with (int s) {
		if (UNLIKELY((s = mc6_socket()) < 0)) {
			serror("\
Error: cannot open socket");
			rc = 1;
			goto nop;
		} else if (mc6_join_group(
				   &r, s, MCAST_ADDR, QUOTE_PORT, NULL) < 0) {
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

	/* start the heartbeat timer */
	ev_timer_init(hbeat, hbeat_cb, 1.0, 1.0);
	ev_timer_start(EV_A_ hbeat);

	/* now wait for events to arrive */
	ev_loop(EV_A_ 0);

	/* begin the freeing */
	with (int s = beef->fd) {
		ev_io_stop(EV_A_ beef);
		mc6_leave_group(s, &r);
		setsock_linger(s, 1);
		close(s);
	}

nop:
	/* destroy the default evloop */
	ev_default_destroy();

out:
	yuck_free(argi);
	return rc;
}
