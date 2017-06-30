#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "dfp754_d64.h"
#include <mkl_vsl.h>
#include "bot.h"
#include "nifty.h"

static VSLStreamStatePtr rstr;
static double alpha = 1.;
static double sigma = 1.;
static double mktv = 0.;
static px_t sprd = 0.02dd;

static clob_oid_t coid[NSIDES];
static lol_quo_t cquo[NSIDES];


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


static void
qchan_cb(bot_t UNUSED(b), qmsg_t m)
{
	switch (m.typ) {
	case QMSG_TOP:
		cquo[m.quo.s] = m.quo;
		/* calc mktv as well */
		mktv = (cquo[SIDE_ASK].p + cquo[SIDE_BID].p) / 2.dd;
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
	default:
		break;
	}
	return;
}

static void
hbeat_cb(bot_t b)
{
/* generate a random trade */
	px_t v = quantizepx(truv(), sprd);

	/* cancel old guys */
	add_omsg(b, (omsg_t){OMSG_CAN, .oid = coid[SIDE_BID]});
	add_omsg(b, (omsg_t){OMSG_CAN, .oid = coid[SIDE_ASK]});
	add_omsg(b, (omsg_t){OMSG_BUY,
				 .ord = (clob_ord_t){TYPE_LMT,
						     .qty = {500.dd, 500.dd},
						     .lmt = v - sprd / 2.dd}});
	add_omsg(b, (omsg_t){OMSG_SEL,
				 .ord = (clob_ord_t){TYPE_LMT,
						     .qty = {500.dd, 500.dd},
						     .lmt = v + sprd / 2.dd}});
	bot_send(b);
	return;
}


#include "egobot.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	const char *host = "localhost";
	int rc = 0;
	bot_t b;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->host_arg) {
		host = argi->host_arg;
	}

	init_rng(0ULL);

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	b->timer_cb = hbeat_cb;
	bot_set_timer(b, 1.0, 1.0);

	b->ochan_cb = ochan_cb;
	b->qchan_cb = qchan_cb;

	/* go go go */
	rc = run_bots(b) < 0;

	kill_bot(b);

out:
	yuck_free(argi);
	return rc;
}
