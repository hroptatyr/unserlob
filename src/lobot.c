#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#elif defined HAVE_DFP_STDLIB_H
# include <dfp/stdlib.h>
#elif defined HAVE_DECIMAL_H
# include <decimal.h>
#endif	/* DFP754_H || HAVE_DFP_STDLIB_H || HAVE_DECIMAL_H */
#include <ev.h>
#include <errno.h>
#include "dfp754_d64.h"
#include "clob/clob.h"
#include "clob/unxs.h"
#include "clob/quos.h"
#include "clob/mmod-auction.h"
/* we need book internals */
#include "clob/plqu.h"
#include "clob/btree.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

#define TYPE_AUC	((clob_type_t)0x10U)

#define NSECS	(1000000000)
#define MCAST_ADDR	"ff05::134"
#define QUOTE_PORT	7978
#define TRADE_PORT	7979
#define DEBUG_PORT	7977

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))

/* global limit order book */
static clob_t glob;
static int quot_chan = STDOUT_FILENO;
static int info_chan = STDERR_FILENO;


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

static qx_t
plqu_sum(plqu_t q)
{
/* sum up displayed quantities */
	qx_t sum = 0.dd;
	for (plqu_iter_t i = {.q = q}; plqu_iter_next(&i);) {
		sum += i.v.qty.dis;
	}
	return sum;
}


/* order/connection mapper */
static size_t nuser;
static size_t zuser;
static struct {
	int s;
} *user;

static uid_t
add_user(int s)
{
	if (UNLIKELY(nuser >= zuser)) {
		zuser = (zuser * 2U) ?: 64U;
		user = realloc(user, zuser * sizeof(*user));
	}
	user[nuser++].s = s;
	return nuser;
}

static int
user_sock(uid_t u)
{
	if (UNLIKELY(u <= 0)) {
		return -1;
	}
	return user[u - 1].s;
}

static int
kill_user(uid_t u)
{
	if (UNLIKELY(u <= 0)) {
		return -1;
	}
	user[u - 1].s = -1;
	return 0;
}


static clob_ord_t
push_beef(const char *ln, size_t lz)
{
/* simple order protocol
 * BUY/LONG \t Q [\t P]
 * SELL/SHORT \t Q [\t P] */
	clob_ord_t o;
	char *on;

	if (UNLIKELY(!lz)) {
		goto bork;
	}
	switch (ln[0U]) {
	case 'F'/*INISH AUCTION*/:
		return (clob_ord_t){TYPE_AUC};
	case 'B'/*UY*/:
	case 'L'/*ONG*/:
	case 'b'/*uy*/:
	case 'l'/*ong*/:
		o.sid = SIDE_BID;
		break;
	case 'S'/*ELL|HORT*/:
	case 's'/*ell|hort*/:
		o.sid = SIDE_ASK;
		break;
	default:
		goto bork;
	}
	with (const char *x = strchr(ln, '\t')) {
		if (UNLIKELY(x == NULL)) {
			goto bork;
		}
		/* otherwise */
		lz -= x + 1U - ln;
		ln = x + 1U;
	}
	/* read quantity */
	o.qty.hid = 0.dd;
	o.qty.dis = strtoqx(ln, &on);
	if (LIKELY(*on > ' ')) {
		/* nope */
		goto bork;
	} else if (*on++ == '\t') {
		o.lmt = strtopx(on, &on);
		if (*on > ' ') {
			goto bork;
		}
		o.typ = TYPE_LMT;
	} else {
		o.typ = TYPE_MKT;
	}
	return o;
bork:
	return (clob_ord_t){(clob_type_t)-1};
}

static void
send_fill(int s, unxs_exe_t x)
{
	char buf[256U];
	size_t len = (memcpy(buf, "FIL\t", 4U), 4U);

	len += qxtostr(buf + len, sizeof(buf) - len, x.qty);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, x.prc);
	buf[len++] = '\n';
	write(s, buf, len);
	return;
}

static void
send_beef(int s, unxs_exe_t x)
{
	char buf[256U];
	size_t len = (memcpy(buf, "TRA\t", 4U), 4U);

	len += qxtostr(buf + len, sizeof(buf) - len, x.qty);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, x.prc);
	buf[len++] = '\n';
	write(s, buf, len);
	return;
}

static void
send_cake(int s, quos_msg_t m)
{
	char buf[256U];
	size_t len = 0U;

	buf[len++] = (char)('A' + m.sid);
	buf[len++] = '2';
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, m.prc);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, m.new);
	buf[len++] = '\n';
	write(s, buf, len);
	return;
}

static void
send_lvl2(int s)
{
	char buf[4096U];
	size_t len = 0U;

	/* market orders first */
	qx_t mb = plqu_sum(glob.mkt[SIDE_BID]);
	qx_t ma = plqu_sum(glob.mkt[SIDE_ASK]);

	len += (memcpy(buf + len, "MKT\t", 4U), 4U);
	len += (memcpy(buf + len, "MKT\t", 4U), 4U);
	len += qxtostr(buf + len, sizeof(buf) - len, mb);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, ma);
	buf[len++] = '\n';

	/* now for limits */
	btree_iter_t bi = {glob.lmt[SIDE_BID]};
	btree_iter_t ai = {glob.lmt[SIDE_ASK]};

	while (len < sizeof(buf)) {
		bool bp = btree_iter_next(&bi);
		bool ap = btree_iter_next(&ai);

		if (UNLIKELY(!bp && !ap)) {
			break;
		}

		len = 0U;
		if (bp) {
			len += pxtostr(buf + len, sizeof(buf) - len, bi.k);
		}
		buf[len++] = '\t';
		if (ap) {
			len += pxtostr(buf + len, sizeof(buf) - len, ai.k);
		}
		buf[len++] = '\t';
		if (bp) {
			len += qxtostr(
				buf + len, sizeof(buf) - len, bi.v->sum.dis);
		}
		buf[len++] = '\t';
		if (ap) {
			len += qxtostr(
				buf + len, sizeof(buf) - len, ai.v->sum.dis);
		}
		buf[len++] = '\n';
	}
	write(s, buf, len);
	return;
}


/* socket goodies */
#include "sock.c"


static void
data_cb(EV_P_ ev_io *w, int UNUSED(re))
{
	char buf[4096];
	ssize_t nrd;

	if ((nrd = read(w->fd, buf, sizeof(buf))) <= 0) {
		/* they want closing */
		goto clo;
	}
	with (clob_ord_t o = push_beef(buf, nrd)) {
		clob_oid_t oi;

		switch (o.typ) {
		case TYPE_MKT:
		case TYPE_LMT:
			break;
		default:
			/* it's just rubbish */
			return;
		}
		/* let them know about this socket */
		o.user = (uintptr_t)w->data;
		/* continuous trading */
		oi = unxs_order(glob, o, NANPX);
		/* all fills concern him so tell him */
		with (unxs_t x = glob.exe) {
			for (size_t i = 0U; i < x->n; i++) {
				send_fill(w->fd, x->x[i]);
			}
		}
	}
	return;

clo:
	fsync(w->fd);
	ev_io_stop(EV_A_ w);
	shutdown(w->fd, SHUT_RDWR);
	close(w->fd);
	kill_user((uintptr_t)w->data);
	free(w);
	return;
}

static void
beef_cb(EV_P_ ev_io *w, int UNUSED(re))
{
/* we're tcp so we've got to accept() the bugger, don't forget :) */
	struct sockaddr_storage sa;
	socklen_t sa_size = sizeof(sa);
	volatile int ns;
	ev_io *aw;

	if ((ns = accept(w->fd, (struct sockaddr*)&sa, &sa_size)) < 0) {
		return;
	}

	aw = malloc(sizeof(*aw));
        ev_io_init(aw, data_cb, ns, EV_READ);
        ev_io_start(EV_A_ aw);
	aw->data = (void*)(uintptr_t)add_user(ns);
	return;
}

static void
prep_cb(EV_P_ ev_prepare *UNUSED(p), int UNUSED(re))
{
	with (unxs_t x = glob.exe) {
		for (size_t i = 0U; i < x->n; i++) {
			/* let the maker know before anyone else */
			int s = user_sock(x->o[MODE_BI * i + SIDE_MAKER].user);
			send_fill(s, x->x[i]);
			send_beef(STDOUT_FILENO, x->x[i]);
		}
		unxs_clr(x);
	}
	with (quos_t q = glob.quo) {
		for (size_t i = 0U; i < q->n; i++) {
			send_cake(STDOUT_FILENO, q->m[i]);
		}
		quos_clr(q);
	}
	return;
}

static void
hbeat_cb(EV_P_ ev_timer *UNUSED(w), int UNUSED(revents))
{
	if (UNLIKELY(info_chan < 0)) {
		return;
	}
	/* otherwise print the book */
	send_lvl2(info_chan);
	return;
}


#include "lobot.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	ev_io beef[1U];
	ev_prepare prep[1U];
	ev_timer hbeat[1U];
	/* use the default event loop unless you have special needs */
	struct ev_loop *loop;
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->daemonise_flag && daemon(0, 0) < 0) {
		serror("\
Error: cannot run in daemon mode");
		rc = 1;
		goto out;
	} else if (argi->daemonise_flag) {
		/* turn off info channel */
		info_chan = -1;
	}

	/* make quote channel multicast */
	if (UNLIKELY((quot_chan = mc6_socket()) < 0)) {
		serror("\
Error: cannot open socket for quote messages");
	} else if (mc6_set_pub(quot_chan, MCAST_ADDR, QUOTE_PORT, NULL) < 0) {
		serror("\
Error: cannot activate publishing mode on socket %d", quot_chan);
		close(quot_chan);
		quot_chan = STDOUT_FILENO;
	}

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	/* init the multicast socket */
	with (int s = listener(TRADE_PORT)) {
		if (UNLIKELY(s < 0)) {
			serror("\
Error: cannot open socket");
			rc = 1;
			goto nop;
		}
		/* hook into our event loop */
		ev_io_init(beef, beef_cb, s, EV_READ);
		ev_io_start(EV_A_ beef);
	}

	/* start the heartbeat timer */
	ev_timer_init(hbeat, hbeat_cb, 1.0, 1.0);
	ev_timer_start(EV_A_ hbeat);

	/* the preparator to publish quotes and trades */
	ev_prepare_init(prep, prep_cb);
	ev_prepare_start(EV_A_ prep);

	/* get going then */
	glob = make_clob();
	glob.exe = make_unxs(MODE_BI);
	glob.quo = make_quos();

	/* now wait for events to arrive */
	ev_loop(EV_A_ 0);

	/* begin the freeing */
	with (int s = beef->fd) {
		ev_io_stop(EV_A_ beef);
		setsock_linger(s, 1);
		close(s);
	}

	free_quos(glob.quo);
	free_unxs(glob.exe);
	free_clob(glob);

nop:
	/* destroy the default evloop */
	ev_default_destroy();

out:
	yuck_free(argi);
	return rc;
}
