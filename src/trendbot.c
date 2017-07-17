#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <string.h>
#include <time.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "dfp754_d64.h"
#include "bot.h"
#include "nifty.h"

/* collect trade volumes and volume-prices over a period of time
 * if the volume exceeds the threshold bet on the trend */

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

#undef INFQX
#define INFQX		((_Decimal64)__builtin_inf())
#undef NANQX
#define NANQX		((_Decimal64)__builtin_nan(""))

static qx_t maxq = INFQX;
static qx_t basq = 1000.dd;

static unxs_exa_t acc = {0.dd, 0.dd};
static qx_t vol = 0.dd;
static qx_t vpr = 0.dd;

static const char *cont;
static size_t conz;
#define INS		.ins = cont, .inz = conz


static void
qchan_cb(bot_t UNUSED(b), qmsg_t m)
{
	switch (m.typ) {
	case QMSG_TRA:
		if (m.inz < conz || memcmp(m.ins, cont, conz)) {
			/* not our quote */
			break;
		}
		vol += m.quo.new;
		vpr += m.quo.new * m.quo.prc;
		break;
	default:
		break;
	}
	return;
}

static void
ochan_cb(bot_t UNUSED(b), omsg_t m)
{
	switch (m.typ) {
	case OMSG_ACC:
		acc = m.exa;
		break;
	default:
		break;
	}
	return;
}

static void
trend_cb(bot_t b)
{
/* generate a random trade */
	static size_t nlong, nshort;
	static px_t old = NANQX;
	omsg_t m = {OMSG_ORD, INS};
	px_t new = (px_t)(vpr / vol);

	if (new > old && vol >= basq && nlong++ < 10U) {
		/* support the uptrend */
		qx_t q = min(maxq - acc.base, basq);
		m.ord = (clob_ord_t){TYPE_MKT, SIDE_LONG, {q, 0.dd}};
		add_omsg(b, m);
		nshort = 0U;
	} else if (new < old && vol >= basq && nshort++ < 10U) {
		/* support the downtrend */
		qx_t q = min(maxq + acc.base, basq);
		m.ord = (clob_ord_t){TYPE_MKT, SIDE_SHORT, {q, 0.dd}};
		add_omsg(b, m);
		nlong = 0U;
	}
	/* send any orders */
	bot_send(b);

	/* reset counters */
	vol = vpr = 0.dd;
	old = new;
	return;
}

static void
contr_cb(bot_t b)
{
/* generate a random trade */
	static size_t nlong, nshort;
	static px_t old = NANQX;
	omsg_t m = {OMSG_ORD, INS};
	px_t new = (px_t)(vpr / vol);

	if (new > old && vol >= basq && nlong++ < 10U) {
		/* bet against uptrend */
		qx_t q = min(maxq + acc.base, basq);
		m.ord = (clob_ord_t){TYPE_MKT, SIDE_SHORT, {q, 0.dd}};
		add_omsg(b, m);
		nshort = 0U;
	} else if (new < old && vol >= basq && nshort++ < 10U) {
		/* bet against downtrend */
		qx_t q = min(maxq - acc.base, basq);
		m.ord = (clob_ord_t){TYPE_MKT, SIDE_LONG, {q, 0.dd}};
		add_omsg(b, m);
		nlong = 0U;
	}
	/* send any orders */
	bot_send(b);

	/* reset counters */
	vol = vpr = 0.dd;
	old = new;
	return;
}


#include "trendbot.yucc"

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
		freq = strtod(argi->freq_arg, NULL);
	}

	if (argi->max_arg) {
		maxq = strtoqx(argi->max_arg, NULL);
	}

	if (argi->qty_arg) {
		if ((basq = strtoqx(argi->qty_arg, NULL)) <= 0.dd) {
			fputs("\
Error: argument to qty must be positive.\n", stderr);
			rc = 1;
			goto out;
		}
	}

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	if (argi->daemonise_flag && daemon(0, 0) < 0) {
		fputs("\
Error: cannot run in daemon mode\n", stderr);
		rc = 1;
		goto kil;
	}

	b->qchan_cb = qchan_cb;
	b->ochan_cb = ochan_cb;
	b->timer_cb = !argi->contrarian_flag ? trend_cb : contr_cb;
	bot_set_timer(b, freq, freq);

	/* go go go */
	rc = run_bots(b) < 0;

kil:
	kill_bot(b);

out:
	yuck_free(argi);
	return rc;
}
