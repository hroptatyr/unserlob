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
#include "sock.h"
#include "lol.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

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
		sum += i.v.qty.hid;
	}
	return sum;
}


/* order/connection mapper */
static size_t nuser;
static size_t zuser;
static int *socks;
static unxs_exa_t *accts;

static uid_t
add_user(int s)
{
	if (UNLIKELY(nuser >= zuser)) {
		zuser = (zuser * 2U) ?: 64U;
		socks = realloc(socks, zuser * sizeof(*socks));
		accts = realloc(accts, zuser * sizeof(*accts));
	}
	socks[nuser] = s;
	accts[nuser] = (unxs_exa_t){0.dd, 0.dd};
	return ++nuser;
}

static int
user_sock(uid_t u)
{
	if (UNLIKELY(u <= 0)) {
		return -1;
	}
	return socks[u - 1];
}

static unxs_exa_t
add_acct(uid_t u, unxs_exa_t a)
{
	if (UNLIKELY(u <= 0)) {
		return (unxs_exa_t){0.dd, 0.dd};
	}
	accts[u - 1].base += a.base;
	accts[u - 1].term += a.term;
	return accts[u - 1];
}

static int
kill_user(uid_t u)
{
	if (UNLIKELY(u <= 0)) {
		return -1;
	}
	/* reset socket but leave accounts untouched */
	socks[u - 1] = -1;
	return 0;
}


#define SEND_OMSG(fd, x...)						\
	do {								\
		char __buf__[256U];					\
		ssize_t __len__;					\
		__len__ = send_omsg(__buf__, sizeof(__buf__), (omsg_t){x}); \
		if (LIKELY(__len__ > 0)) {				\
			(void)send((fd), __buf__, __len__, 0);		\
		}							\
	} while (0)

#define SEND_QMSG(fd, x...)						\
	do {								\
		char __buf__[256U];					\
		ssize_t __len__;					\
		__len__ = send_qmsg(__buf__, sizeof(__buf__), (qmsg_t){x}); \
		if (LIKELY(__len__ > 0)) {				\
			(void)send((fd), __buf__, __len__, 0);		\
		}							\
	} while (0)
			
static void
prnt_lvl2(int s)
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
		bool bp, ap;

		do {
			bp = btree_iter_next(&bi);
		} while (bp && bi.v->sum.dis + bi.v->sum.hid <= 0.dd);
		do {
			ap = btree_iter_next(&ai);
		} while (ap && ai.v->sum.dis + ai.v->sum.hid <= 0.dd);

		if (UNLIKELY(!bp && !ap)) {
			break;
		}

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
		buf[len++] = '\t';
		if (bp) {
			len += qxtostr(
				buf + len, sizeof(buf) - len, bi.v->sum.hid);
		}
		buf[len++] = '\t';
		if (ap) {
			len += qxtostr(
				buf + len, sizeof(buf) - len, ai.v->sum.hid);
		}
		buf[len++] = '\n';
	}
	write(s, buf, len);
	return;
}

static void
prnt_acct(int s)
{
	char buf[4096U];
	size_t len = 0U;

	for (size_t i = 1U; i <= nuser; i++) {
		len += qxtostr(buf + len, sizeof(buf) - len, accts[i - 1].base);
		buf[len++] = '/';
		len += qxtostr(buf + len, sizeof(buf) - len, accts[i - 1].term);
		buf[len++] = ' ';
	}
	buf[len++] = '\n';
	write(s, buf, len);
	return;
}

static void
omsg_add_ord(int fd, clob_ord_t o, const uid_t u)
{
	clob_oid_t oi;

	switch (o.typ) {
	case TYPE_MKT:
	case TYPE_LMT:
		break;
	default:
		/* it's just rubbish */
		return;
	}
	/* stick uid into orderbook */
	o.user = u;
	/* continuous trading */
	oi = unxs_order(glob, o, NANPX);
	/* let him know about the residual order */
	if (oi.qid) {
		SEND_OMSG(fd, OMSG_OID, .oid = oi);
	}
	return;
}

static void
omsg_del_oid(int fd, clob_oid_t o)
{
	int rc = clob_del(glob, o);

	SEND_OMSG(fd, .typ = !(rc < 0) ? OMSG_KIL : OMSG_NOK, .oid = o);
	return;
}


#define ZCLO	(4096U)
#define UBSZ	(ZCLO - offsetof(struct uclo_s, buf))

struct uclo_s {
	ev_io w;
	uid_t uid;
	size_t bof;
	char buf[];
};

static void
data_cb(EV_P_ ev_io *w, int UNUSED(re))
{
	struct uclo_s *u = (void*)w;
	ssize_t nrd;
	size_t npr = 0U;

	nrd = read(u->w.fd, u->buf + u->bof, UBSZ - u->bof);
	if (UNLIKELY(nrd <= 0)) {
		/* they want closing */
		goto clo;
	}
	/* up the bof */
	u->bof += nrd;
	/* snarf linewise */
	for (const char *x;
	     (x = memchr(u->buf + npr, '\n', u->bof - npr));
	     npr = x + 1U - u->buf) {
		omsg_t m = recv_omsg(u->buf + npr, x + 1U - (u->buf + npr));

		switch (m.typ) {
		case OMSG_BUY:
		case OMSG_SEL:
			omsg_add_ord(u->w.fd, m.ord, u->uid);
			break;
		case OMSG_CAN:
			omsg_del_oid(u->w.fd, m.oid);
			break;
		default:
			break;
		}
	}
	/* move left-overs */
	if (u->bof - npr) {
		memmove(u->buf, u->buf + npr, u->bof - npr);
	}
	u->bof -= npr;
	return;

clo:
	fsync(w->fd);
	kill_user((uintptr_t)w->data);
	ev_io_stop(EV_A_ w);
	shutdown(w->fd, SHUT_RDWR);
	close(w->fd);
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
	struct uclo_s *aw;

	if ((ns = accept(w->fd, (struct sockaddr*)&sa, &sa_size)) < 0) {
		return;
	}

	aw = malloc(4096U);
        ev_io_init(&aw->w, data_cb, ns, EV_READ);
        ev_io_start(EV_A_ &aw->w);
	aw->uid = add_user(ns);
	aw->bof = 0;
	return;
}

static void
prep_cb(EV_P_ ev_prepare *UNUSED(p), int UNUSED(re))
{
	with (unxs_t x = glob.exe) {
		char buf[256U];
		ssize_t len;

		for (size_t i = 0U; i < x->n; i++) {
			/* let the maker know before anyone else
			 * well, the taker has already been informed */
			const uid_t u = x->o[MODE_BI * i + SIDE_MAKER].user;
			const uid_t cu = x->o[MODE_BI * i + SIDE_TAKER].user;
			const clob_side_t s = (clob_side_t)x->s[i];
			const clob_side_t cs = clob_contra_side(s);
			unxs_exa_t acc = add_acct(u, unxs_exa(x->x[i], s));
			unxs_exa_t cacc = add_acct(cu, unxs_exa(x->x[i], cs));

			len = 0U;
			len += send_omsg(buf + len, sizeof(buf) - len,
					(omsg_t){OMSG_FIL, .exe = x->x[i]});
			len += send_omsg(buf + len, sizeof(buf) - len,
					 (omsg_t){OMSG_ACC, .exa = acc});
			if (LIKELY(len > 0)) {
				send(user_sock(u), buf, len, 0);
			}

			len = 0U;
			len += send_omsg(buf + len, sizeof(buf) - len,
					(omsg_t){OMSG_FIL, .exe = x->x[i]});
			len += send_omsg(buf + len, sizeof(buf) - len,
					 (omsg_t){OMSG_ACC, .exa = cacc});
			if (LIKELY(len > 0)) {
				send(user_sock(cu), buf, len, 0);
			}
		}
		for (size_t i = 0U; i < x->n; i++) {
			SEND_QMSG(quot_chan, QMSG_TRA,
				  .quo = {NSIDES, x->x[i].prc, x->x[i].qty});
		}
		unxs_clr(x);
	}
	with (quos_t q = glob.quo) {
		for (size_t i = 0U; i < q->n; i++) {
			SEND_QMSG(quot_chan, QMSG_LVL, q->m[i]);
		}
		if (q->n) {
			btree_key_t k;
			btree_val_t *v;

			v = btree_top(glob.lmt[SIDE_ASK], &k);
			if (LIKELY(v != NULL)) {
				quos_msg_t t = {SIDE_ASK, k, v->sum.dis};
				SEND_QMSG(quot_chan, QMSG_TOP, t);
			}

			v = btree_top(glob.lmt[SIDE_BID], &k);
			if (LIKELY(v != NULL)) {
				quos_msg_t t = {SIDE_BID, k, v->sum.dis};
				SEND_QMSG(quot_chan, QMSG_TOP, t);
			}
		}
		/* clear them quotes */
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
	prnt_lvl2(info_chan);
	prnt_acct(info_chan);
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
