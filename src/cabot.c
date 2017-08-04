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
#include "hash.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

#define NSECS	(1000000000)
#define MCAST_ADDR	"ff05::134"
#define QUOTE_PORT	7978
#define TRADE_PORT	7979
#define ORDER_PORT	7977

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))

/* global limit order book */
static clob_t *clob;
static int quot_chan = STDOUT_FILENO;
static int exec_chan = STDOUT_FILENO;
static int info_chan = STDERR_FILENO;

#define NINSTR		(ninstr ?: 1U)
static size_t ninstr;
static char *instr;
static size_t *instz;
static hx_t *insth;


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
		accts = realloc(accts, zuser * NINSTR * sizeof(*accts));
	}
	socks[nuser] = s;
	accts[nuser * NINSTR] = (unxs_exa_t){0.dd, 0.dd};
	for (size_t i = 1U; i < ninstr; i++) {
		accts[nuser * ninstr + i] = (unxs_exa_t){0.dd, 0.dd};
	}
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
add_acct(uid_t u, size_t i, unxs_exa_t a)
{
	const size_t slot = (u - 1) * NINSTR + i;

	if (UNLIKELY(u <= 0)) {
		return (unxs_exa_t){0.dd, 0.dd};
	}
	accts[slot].base += a.base;
	accts[slot].term += a.term;
	return accts[slot];
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

#define INS(j)								\
	.ins = instr + instz[(j)], .inz = instz[(j) + 1U] - instz[(j) + 0U]
			
static void
prnt_lvl2(int s, size_t ins)
{
	char buf[4096U];
	size_t len = 0U;

	/* market orders first */
	qx_t mb = plqu_sum(clob[ins].mkt[CLOB_SIDE_BID]);
	qx_t ma = plqu_sum(clob[ins].mkt[CLOB_SIDE_ASK]);

	const size_t thisz = instz[ins + 1U] - instz[ins + 0U];
	len += (memcpy(buf, instr + instz[ins], thisz), thisz);
	buf[len++] = '\t';

	len += (memcpy(buf + len, "MKT\t", 4U), 4U);
	len += (memcpy(buf + len, "MKT\t", 4U), 4U);
	len += qxtostr(buf + len, sizeof(buf) - len, mb);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, ma);
	buf[len++] = '\n';

	/* now for limits */
	btree_iter_t bi = {clob[ins].lmt[CLOB_SIDE_BID]};
	btree_iter_t ai = {clob[ins].lmt[CLOB_SIDE_ASK]};

	while (len < sizeof(buf)) {
		bool bp, ap;

		bp = btree_iter_next(&bi);
		ap = btree_iter_next(&ai);

		if (UNLIKELY(!bp && !ap)) {
			break;
		} else if (UNLIKELY(len + 256U > sizeof(buf))) {
			/* intermediate flush */
			write(s, buf, len);
			len = 0U;
		}

		buf[len++] = '\t';
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
prnt_acct(int s, size_t ins)
{
	char buf[4096U];
	size_t len = 0U;

	const size_t thisz = instz[ins + 1U] - instz[ins + 0U];
	len += (memcpy(buf, instr + instz[ins], thisz), thisz);
	buf[len++] = '\t';
	for (size_t i = 0U; i < nuser; i++) {
		qx_t op;

		if (UNLIKELY(len >= countof(buf) - 64U)) {
			/* write interim */
			write(s, buf, len);
			len = 0U;
		}

		len += qxtostr(buf + len, sizeof(buf) - len,
			       accts[i * NINSTR + ins].base);
		buf[len++] = '/';
		len += qxtostr(buf + len, sizeof(buf) - len,
			       accts[i * NINSTR + ins].term);
		
		buf[len++] = '(';
		op = -accts[i * NINSTR + ins].term /
			accts[i * NINSTR + ins].base;
		op = quantizeqx(op, accts[i * NINSTR + ins].term);
		len += qxtostr(buf + len, sizeof(buf) - len, op);
		buf[len++] = ')';
		buf[len++] = ' ';
	}
	buf[len++] = '\n';
	write(s, buf, len);
	return;
}

#include <assert.h>
static void
chck_acct(void)
{
/* account invariant is that sum of base must be 0, and sum of terms must be 0 */
	for (size_t j = 0U; j < NINSTR; j++) {
		unxs_exa_t s = {0.dd, 0.dd};

		for (size_t i = 0U; i < nuser; i++) {
			s.base += accts[i * NINSTR + j].base;
			s.term += accts[i * NINSTR + j].term;
		}
		assert(s.base == 0.dd);
		assert(s.term == 0.dd);
	}
	return;
}


static void
omsg_add_ord(int fd, size_t i, clob_ord_t o, const uid_t u)
{
	clob_oid_t oi;

	switch (o.typ) {
	case CLOB_TYPE_MKT:
	case CLOB_TYPE_LMT:
		break;
	default:
		/* it's just rubbish */
		return;
	}
	/* stick uid into orderbook */
	o.user = u;
	/* continuous trading */
	oi = clob_add(clob[i], o);
	/* let him know about the residual order */
	if (oi.qid) {
		SEND_OMSG(fd, OMSG_OID, INS(i), .oid = oi);
	}
	return;
}

static void
omsg_del_oid(int fd, size_t i, clob_oid_t o)
{
	int rc = clob_del(clob[i], o);

	SEND_OMSG(fd, .typ = !(rc < 0) ? OMSG_KIL : OMSG_NOK, INS(i), .oid = o);
	return;
}

static void
diss_exe(unxs_t exe, size_t ins)
{

	for (size_t i = 0U; i < exe->n; i++) {
		/* let the traders know before anyone else */
		const clob_oid_t o = exe->o[MODE_SC * i];
		const clob_side_t s = (clob_side_t)exe->s[i];
		unxs_exa_t a = add_acct(o.user, ins, unxs_exa(exe->x[i], s));
		char buf[256U];
		size_t len = 0U;

		len += send_omsg(buf + len, sizeof(buf) - len,
				 (omsg_t){OMSG_FIL, INS(ins),
						 .fid = o,
						 .exe = exe->x[i]});
		len += send_omsg(buf + len, sizeof(buf) - len,
				 (omsg_t){OMSG_ACC, INS(ins), .exa = a});
		if (LIKELY(len > 0)) {
			send(user_sock(o.user), buf, len, 0);
			/* append user */
			buf[len - 1U] = '\t';
			len += snprintf(buf + len, sizeof(buf) - len,
					"%u", (uid_t)o.user);
			buf[len++] = '\n';
			write(exec_chan, buf, len);
		}
	}
	for (size_t i = 0U; i < exe->n; i++) {
		SEND_QMSG(quot_chan, QMSG_TRA, INS(ins),
			  .quo = {NCLOB_SIDES, exe->x[i].prc, exe->x[i].qty});
	}
	unxs_clr(exe);
	return;
}

static void
diss_auc(mmod_auc_t a, size_t ins)
{
	SEND_QMSG(quot_chan, QMSG_AUC, INS(ins), .auc = a);
	return;
}

static void
diss_imb(mmod_auc_t a, size_t ins)
{
	SEND_QMSG(quot_chan, QMSG_IMB, INS(ins), .auc = a);
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

static size_t
hins(const char *ins, size_t inz)
{
	hx_t h;
	size_t j;

	if (inz == 0U) {
		return 0U;
	}
	/* otherwise hash the guy */
	h = hash(ins, inz);
	/* and try and find him */
	for (j = 0U; j < ninstr; j++) {
		if (insth[j] == h) {
			return j;
		}
	}
	return -1ULL;
}

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
		size_t ins = hins(m.ins, m.inz);

		if (UNLIKELY(ins > ninstr)) {
			continue;
		}
		switch (m.typ) {
		case OMSG_ORD:
			omsg_add_ord(u->w.fd, ins, m.ord, u->uid);
			break;
		case OMSG_CAN:
			omsg_del_oid(u->w.fd, ins, m.oid);
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
hbeat_cb(EV_P_ ev_timer *UNUSED(w), int UNUSED(revents))
{
/* perform auction on even ticks */
	static unsigned int n;

	if (n++ % 4U) {
		for (size_t i = 0U; i < NINSTR; i++) {
			mmod_auc_t auc = mmod_auction(clob[i]);
			diss_imb(auc, i);
		}
		return;
	}
	/* otherwise really do it */
	for (size_t i = 0U; i < NINSTR; i++) {
		mmod_auc_t auc = mmod_auction(clob[i]);
		unxs_auction(clob[i], auc.prc, auc.qty);
		diss_auc(auc, i);
	}
	for (size_t i = 0U; i < NINSTR; i++) {
		diss_exe(clob[i].exe, i);
	}
	for (size_t i = 0U; i < NINSTR; i++) {
		/* not interested in quotes */
		quos_clr(clob[i].quo);
	}

	if (UNLIKELY(info_chan < 0)) {
		return;
	}
	/* otherwise print the book */
	for (size_t i = 0U; i < NINSTR; i++) {
		prnt_lvl2(info_chan, i);
		prnt_acct(info_chan, i);
	}

	/* check accounts */
	chck_acct();
	return;
}

static void
sig_cb(EV_P_ ev_signal *UNUSED(w), int UNUSED(revents))
{
	ev_unloop(EV_A_ EVUNLOOP_ALL);
	return;
}


#include "cabot.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	ev_io beef[1U];
	ev_timer hbeat[1U];
	ev_signal trm[1U];
	ev_signal itr[1U];
	/* use the default event loop unless you have special needs */
	struct ev_loop *loop;
	double freq = 1.0;
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->freq_arg) {
		if ((freq = strtod(argi->freq_arg, NULL)) <= 0) {
			errno = 0, serror("\
Error: frequency argument must be positive");
			rc = 1;
			goto out;
		}
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

	if ((ninstr = argi->nargs)) {
		size_t zinstr = 64U;
		size_t insoff = 0U;

		instr = malloc(zinstr * sizeof(*instr));
		instz = malloc((ninstr + 1U) * sizeof(*instz));
		insth = malloc(ninstr * sizeof(*insth));

		for (size_t i = 0U; i < argi->nargs; i++) {
			const size_t z = strlen(argi->args[i]);
			if (UNLIKELY(insoff + z >= zinstr)) {
				zinstr *= 2U;
				instr = realloc(instr, zinstr * sizeof(*instr));
			}
			memcpy(instr + insoff, argi->args[i], z);
			instz[i] = insoff;
			insth[i] = hash(argi->args[i], z);
			insoff += z;
		}
		instz[argi->nargs] = insoff;
	} else {
		static char dummy[] = "";
		static size_t dummz[] = {0U, 0U};
		instr = dummy;
		instz = dummz;
	}

	/* no more parameters */
	yuck_free(argi);

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

	/* make exec channel multicast */
	if (UNLIKELY((exec_chan = mc6_socket()) < 0)) {
		serror("\
Error: cannot open socket for quote messages");
	} else if (mc6_set_pub(exec_chan, MCAST_ADDR, TRADE_PORT, NULL) < 0) {
		serror("\
Error: cannot activate publishing mode on socket %d", exec_chan);
		close(exec_chan);
		exec_chan = STDOUT_FILENO;
	}

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	/* init the tcp socket for trading */
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
	ev_timer_init(hbeat, hbeat_cb, freq / 2., freq / 2.);
	ev_timer_start(EV_A_ hbeat);

	/* get going then */
	clob = malloc(NINSTR * sizeof(*clob));
	for (size_t i = 0U; i < NINSTR; i++) {
		clob[i] = make_clob();
		clob[i].exe = make_unxs(MODE_SC);
		clob[i].quo = make_quos();
	}

	ev_signal_init(trm, sig_cb, SIGTERM);
	ev_signal_start(EV_A_ trm);
	ev_signal_init(itr, sig_cb, SIGINT);
	ev_signal_start(EV_A_ itr);

	/* now wait for events to arrive */
	ev_loop(EV_A_ 0);

	/* begin the freeing */
	with (int s = beef->fd) {
		ev_io_stop(EV_A_ beef);
		setsock_linger(s, 1);
		close(s);
	}

	for (size_t i = 0U; i < NINSTR; i++) {
		free_quos(clob[i].quo);
		free_unxs(clob[i].exe);
		free_clob(clob[i]);
	}
	free(clob);

	if (ninstr) {
		free(instr);
		free(instz);
		free(insth);
	}

nop:
	/* destroy the default evloop */
	ev_default_destroy();

out:
	return rc;
}
