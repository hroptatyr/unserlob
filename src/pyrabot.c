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

struct vwap_s {
	qx_t vprc;
	qx_t vqty;
};

static size_t nlvl = 1U;
static qty_t Q = {500.dd, 500.dd};
/* best-belief price */
static px_t bb;
/* best-(half-)spread per unit of quantity */
static px_t bs = 0.0001dd;
/* min tick */
static px_t mint = 0.01dd;
/* total posted quantity */
static qx_t totq;

static clob_oid_t *coid;
static size_t noid;

static struct vwap_s vsum = {0.dd, 0.dd};

static const char *cont;
static size_t conz;
#define INS		.ins = cont, .inz = conz


static void
ochan_cb(bot_t UNUSED(b), omsg_t m)
{
	switch (m.typ) {
	case OMSG_OID:
		coid[noid++] = m.oid;
		break;
	case OMSG_FIL:
		vsum.vprc += (qx_t)m.exe.prc * m.exe.qty;
		vsum.vqty += m.exe.qty;
		break;
	case OMSG_ACC:
		break;
	default:
		break;
	}
	return;
}

static void
hbeat_cb(bot_t b)
{
	clob_ord_t o = {TYPE_LMT, .qty = Q};

	/* cancel everything */
	for (size_t i = 0U; i < noid; i++) {
		add_omsg(b, (omsg_t){OMSG_CAN, INS, .oid = coid[i]});
		bot_send(b);
	}

	/* update mid point */
	with (px_t vwap = (px_t)(vsum.vprc / vsum.vqty)) {
		if (!isnanpx(vwap)) {
			bb = vwap;
		}
	}
	/* update spread,
	 * we determine the quantitiy taken off the book relative to
	 * the posted quantity.  50% taking shall mean the spread
	 * stays the same.  0% taking means the spread halves and
	 * 100% means the spread doubles, i.e. new spread = 2^(2 * clr - 1) */
	with (qx_t clrd = vsum.vqty / totq) {
		bs *= pow(2., 2. * (double)clrd - 1.);
	}

	/* reset oid counter, we'll emit new orders */
	noid = 0U;
	/* set up new liquidity pyramid around BB with spreads BS */
	const qx_t qinc = qty(Q);
	qx_t q = qinc;

	/* exponential prices, linearly increasing quantities */
	//for (size_t i = 0U; i < nlvl; i++, q += q, o.qty = qty_add(o.qty, Q))
	/* linear prices, constant quantities */
	for (size_t i = 0U; i < nlvl; i++, q += qinc) {
		qx_t sq = max(bs * q, mint / 2.dd);

		o.sid = SIDE_ASK;
		o.lmt = quantizepx((px_t)(bb + sq), mint);
		add_omsg(b, (omsg_t){OMSG_ORD, INS, .ord = o});

		o.sid = SIDE_BID;
		o.lmt = quantizepx((px_t)(bb - sq), mint);
		add_omsg(b, (omsg_t){OMSG_ORD, INS, .ord = o});

		bot_send(b);
	}
	/* reset vsum counter */
	vsum = (struct vwap_s){0.dd, 0.dd};
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

	if (argi->qty_arg) {
		char *on;
		Q.dis = strtoqx(argi->qty_arg, &on);
		if (*on++ == '+') {
			Q.hid = strtoqx(on, NULL);
		} else {
			Q.hid = 0.dd;
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
	coid = calloc(NSIDES * nlvl, sizeof(*coid));
	totq = (qx_t)nlvl * qty(Q) * (qx_t)NSIDES;

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	b->timer_cb = hbeat_cb;
	bot_set_timer(b, 0.0, freq);

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
