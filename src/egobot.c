#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <string.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "dfp754_d64.h"
#include <mkl_vsl.h>
#include <math.h>
#include "bot.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

#define INFQX		((_Decimal64)__builtin_inf())

static VSLStreamStatePtr rstr;
static double alpha = 1.;
static double sigma = 1.;
static double mktv = 0.;
static px_t sprd = 0.02dd;
static qx_t maxq = INFQX;
static qty_t Q = {500.dd, 500.dd};

static clob_oid_t coid[NSIDES];
static quos_msg_t cquo[NSIDES];
static unxs_exa_t acc = {0.dd, 0.dd};

static const char *cont;
static size_t conz;
#define INS		.ins = cont, .inz = conz


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

	if (LIKELY(!isnan(mktv))) {
		truv += rnorm(alpha * (mktv - truv));
	}
	return truv;
}

#if 0
static inline qty_t
minq(qty_t q, qx_t sum)
{
/* prefer less display */
	if (q.hid + q.dis > sum) {
		/* sum cannot be NAN anymore */
		if (q.hid > sum) {
			return (qty_t){0.dd, sum};
		}
		return (qty_t){sum - q.hid, q.hid};
	}
	return q;
}
#else
static inline qty_t
minq(qty_t q, qx_t sum)
{
/* prefer less hidden */
	if (q.dis + q.hid > sum) {
		/* sum cannot be NAN anymore */
		if (q.dis > sum) {
			return (qty_t){sum, 0.dd};
		}
		return (qty_t){q.dis, sum - q.dis};
	}
	return q;
}
#endif


static void
qchan_cb(bot_t UNUSED(b), qmsg_t m)
{
	switch (m.typ) {
	case QMSG_TOP:
		if (m.inz < conz || memcmp(m.ins, cont, conz)) {
			/* not our quote */
			break;
		}
		cquo[m.quo.sid] = m.quo;
		/* calc mktv as well */
		mktv = (cquo[SIDE_ASK].prc + cquo[SIDE_BID].prc) / 2.dd;
		break;
	case QMSG_AUC:
		if (!isnanpx(m.auc.prc)) {
			mktv = m.auc.prc;
		}
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
	case OMSG_OID:
		coid[m.oid.sid] = m.oid;
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
/* generate a random trade */
	px_t v;
	qty_t q;

	/* cancel old guys */
	add_omsg(b, (omsg_t){OMSG_CAN, INS, .oid = coid[SIDE_BID]});
	add_omsg(b, (omsg_t){OMSG_CAN, INS, .oid = coid[SIDE_ASK]});

	v = quantizepx(truv(), sprd);
	q = minq(Q, maxq - acc.base);
	if (qty(q) > 0.dd) {
		/* split q up in displayed and hidden */
		omsg_t m = {
			OMSG_ORD, INS,
			.ord = (clob_ord_t){TYPE_LMT, SIDE_LONG,
					    .qty = q,
					    .lmt = v - sprd / 2.dd}
		};
		add_omsg(b, m);
	}
	q = minq(Q, maxq + acc.base);
	if (qty(q) > 0.dd) {
		omsg_t m = {
			OMSG_ORD, INS,
			.ord = (clob_ord_t){TYPE_LMT, SIDE_SHORT,
					    .qty = q,
					    .lmt = v + sprd / 2.dd}
		};
		add_omsg(b, m);
	}
	bot_send(b);
	return;
}


#include "egobot.yucc"

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

	if (argi->spread_arg) {
		sprd = strtopx(argi->spread_arg, NULL);
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
		char *on;
		Q.dis = strtoqx(argi->qty_arg, &on);
		if (*on++ == '+') {
			Q.hid = strtoqx(on, NULL);
		} else {
			Q.hid = 0.dd;
		}
	}

	if (argi->sigma_arg) {
		if ((sigma = strtod(argi->sigma_arg, NULL)) <= 0.) {
			fputs("\
Error: argument to sigma must not be non-positive\n", stderr);
			rc = 1;
			goto out;
		}
	}

	if (argi->alpha_arg) {
		alpha = strtod(argi->alpha_arg, NULL);
	}

	init_rng(0ULL);

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	b->timer_cb = hbeat_cb;
	bot_set_timer(b, 0., freq);

	b->ochan_cb = ochan_cb;
	b->qchan_cb = qchan_cb;

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
