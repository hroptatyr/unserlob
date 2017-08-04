#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <string.h>
#include <math.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include <clob/clob_val.h>
#include "dfp754_d64.h"
#include "bot.h"
#include "nifty.h"

/**
 * Post a pyramid of liquidity:
 *
 *        M
 *       B A
 *      BB AA
 *     BBB AAA
 *    BBBB AAAA
 *
 * M is the current best-belief mid-point and won't be posted.
 * The B's and A's are quoted around M.
 *
 * When trades occur, the pyramid is damaged in two ways:
 *
 *        M                M
 *         A
 *         AA    or      BB AA
 *     BBB AAA          BBB AAA
 *    BBBB AAAA        BBBB AAAA
 *
 * The left one  indicates that our spread was probably okat but the
 * mid-point wasn't, the right one indicates that the mid-point was
 * probably okay but the spread was too tight.
 *
 * The new mid-point is the volume-weighted average price of the fills,
 * the (consolidated) spread is linear, i.e. 0 at 0 quantities at M. */

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

#define INFQX		INFD64
#define fabsqx		fabsd64
#define quantizepx	quantized64
#define isnanpx		isnand64

struct vwap_s {
	qx_t vprc;
	qx_t vqty[NCLOB_SIDES];
};

static size_t nlvl = 1U;
static qty_t Q = {500.dd, 500.dd};
static qx_t maxq = INFQX;
static px_t maxs = 1.dd;
/* min tick */
static px_t mint = 0.01dd;
static px_t mins;
/* best-belief price */
static px_t bb = 0.dd;
/* best-(half-)spread per unit of quantity */
static px_t bs;
/* total posted quantity */
static qx_t totq;
static unxs_exa_t acc = {0.dd, 0.dd};

static clob_oid_t *coid;
static size_t noid;

static struct vwap_s vsum;
static size_t nauc;

static const char *cont;
static size_t conz;
#define INS		.ins = cont, .inz = conz


static inline qty_t
_maxq(qty_t q, qty_t sum, qx_t lim)
{
/* Q is the quantity we'd like to post
 * SUM is how much we posted so far
 * LIM is the maximum postable quantity, can be NAN */
	if (qty(qty_add(q, sum)) > lim) {
		if (LIKELY(qty(sum) >= lim)) {
			return qty0;
		}
		/* otherwise Q = LIM - SUM mimicking qty_exe() */
		qx_t x = lim - qty(sum);
		if (x > q.dis) {
			return (qty_t){q.dis, q.hid - x};
		}
		return (qty_t){x, 0.dd};
	}
	/* otherwise just post it */
	return q;
}


static void
ochan_cb(bot_t UNUSED(b), omsg_t m)
{
	switch (m.typ) {
	case OMSG_OID:
		coid[noid++] = m.oid;
		break;
	case OMSG_FIL:
		vsum.vprc += (qx_t)m.exe.prc * m.exe.qty;
		vsum.vqty[m.fid.sid] += m.exe.qty;
		break;
	case OMSG_ACC:
		acc = m.exa;
		break;
	default:
		break;
	}
	return;
}

static void
hbeat_cb(bot_t b)
{
	clob_ord_t o = {CLOB_TYPE_LMT, .qty = Q};

	/* cancel everything */
	for (size_t i = 0U; i < noid; i++) {
		add_omsg(b, (omsg_t){OMSG_CAN, INS, .oid = coid[i]});
		bot_send(b);
	}

	/* update mid point */
	with (px_t vwap = (px_t)(vsum.vprc / (vsum.vqty[0U] + vsum.vqty[1U]))) {
		if (!isnanpx(vwap)) {
			bb = vwap;
		}
	}
#if 0
	/* update spread,
	 * we determine the quantitiy taken off the book relative to
	 * the posted quantity.  50% taking shall mean the spread
	 * stays the same.  0% taking means the spread halves and
	 * 100% means the spread doubles, i.e. new spread = 2^(2 * clr - 1) */
	with (qx_t clrd = (vsum.vqty[0U] + vsum.vqty[1U]) / totq) {
		bs *= pow(2., 2. * (double)clrd - 1.);
	}
#elif 0
	/* update spread
	 * alternative with exponential back-off
	 * double the spread as soon as there's trades, halve it
	 * when there's not */
	if (vsum.vqty[0U] + vsum.vqty[1U] > 0.dd) {
		bs *= 4.dd;
	} else {
		bs = max(bs / 2.dd, mins);
	}
#else
	/* update spread
	 * when level n has been touched (but not cleared) use the
	 * spread of level n, when it has been cleared use n+1 */
	with (qx_t maxvqty = max(vsum.vqty[0U], vsum.vqty[1U])) {
		if (maxvqty == 0.dd) {
			bs = max(bs / 2.dd, mins);
		} else {
			qx_t lvl = ceild64((maxvqty + 1.dd) / qty(Q));

			bs *= lvl;
		}
	}
#endif

	/* reset oid counter, we'll emit new orders */
	noid = 0U;
	/* set up new liquidity pyramid around BB with spreads BS */
	const qx_t qinc = qty(Q);
	qx_t q = qinc;
	qty_t bq = qty0, aq = qty0;

	/* exponential prices, linearly increasing quantities */
	//for (size_t i = 0U; i < nlvl; i++, q += q, o.qty = qty_add(o.qty, Q))
	/* linear prices, constant quantities */
	for (size_t i = 0U; i < nlvl; i++, q += qinc) {
		qx_t sq = max(bs * q, mint / 2.dd);
		/* calculate renormalised spread */
		px_t rs = (px_t)(sq / (fabsqx(bb) + sq));

		if (UNLIKELY(rs > maxs)) {
			/* solve reno-spread equation for sq */
			sq = (qx_t)maxs * (fabsqx(bb) + sq);
		}

		o.sid = CLOB_SIDE_ASK;
		o.lmt = quantizepx((px_t)(bb + sq), mint);
		o.qty = _maxq(Q, aq, maxq + acc.base);
		add_omsg(b, (omsg_t){OMSG_ORD, INS, .ord = o});
		aq = qty_add(aq, o.qty);

		o.sid = CLOB_SIDE_BID;
		o.lmt = quantizepx((px_t)(bb - sq), mint);
		o.qty = _maxq(Q, bq, maxq - acc.base);
		add_omsg(b, (omsg_t){OMSG_ORD, INS, .ord = o});
		bq = qty_add(bq, o.qty);

		bot_send(b);
	}
	/* reset vsum counter */
	vsum = (struct vwap_s){0.dd, 0.dd, 0.dd};
	return;
}

static void
qchan_cb(bot_t b, qmsg_t m)
{
	static size_t cauc;

	switch (m.typ) {
	case QMSG_AUC:
		/* at least cabot sends out 2 AUCs per auction,
		 * once the preliminary price and once the final price
		 * we secretly use this knowledge here as there is
		 * currently no way to obtain it from the auctioning
		 * exchange and the user should not be trusted with
		 * having to specify critical information like this */
		if (m.inz < conz || memcmp(m.ins, cont, conz)) {
			/* not our auction */
			;
		} else if (!(cauc++ % nauc)) {
			hbeat_cb(b);
		}
		break;
	}
	return;
}


#include "pyrabot.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	const char *host = "localhost";
	double freq = 1.0;
	int rc = 0;
	bot_t b;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->nargs) {
		cont = *argi->args;
		conz = strlen(cont);
	}

	if (argi->host_arg) {
		host = argi->host_arg;
	}

	if (argi->freq_arg) {
		if ((freq = strtod(argi->freq_arg, NULL)) <= 0.0) {
			fputs("\
Error: argument to freq must be positive.\n", stderr);
			rc = 1;
			goto out;
		}
	}

	if (argi->qty_arg) {
		char *on;
		Q.dis = strtoqx(argi->qty_arg, &on);
		if (*on++ == '+') {
			Q.hid = strtoqx(on, NULL);
		} else {
			Q.hid = 0.dd;
		}
	}

	if (argi->max_arg) {
		maxq = strtoqx(argi->max_arg, NULL);
	}

	if (argi->max_spread_arg) {
		maxs = strtoqx(argi->max_spread_arg, NULL);
	}

	if (argi->auction_arg) {
		if (argi->auction_arg == YUCK_OPTARG_NONE) {
			nauc = 1U;
		} else if (!(nauc = strtoul(argi->auction_arg, NULL, 10))) {
			fputs("\
Error: argument to auction must be positive.\n", stderr);
			rc = 1;
			goto out;
		}
	}

	if (argi->levels_arg) {
		if (!(nlvl = strtoul(argi->levels_arg, NULL, 10))) {
			fputs("\
Error: argument to levels must be positive.\n", stderr);
			rc = 1;
			goto out;
		}
	}
	/* prep coids */
	coid = calloc(NCLOB_SIDES * nlvl, sizeof(*coid));
	totq = (qx_t)nlvl * qty(Q) * (qx_t)NCLOB_SIDES;
	mins = (qx_t)mint / qty(Q);
	bs = mint;
	(void)totq;

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	if (!nauc) {
		b->timer_cb = hbeat_cb;
		bot_set_timer(b, 0.0, freq);
	} else {
		b->qchan_cb = qchan_cb;
	}
	b->ochan_cb = ochan_cb;

	if (argi->daemonise_flag && daemon(0, 0) < 0) {
		fputs("\
Error: cannot run in daemon mode\n", stderr);
		rc = 1;
		goto kil;
	}

	/* go go go */
	rc = run_bots(b) < 0;

kil:
	kill_bot(b);

out:
	yuck_free(argi);
	return rc;
}
