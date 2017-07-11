#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <time.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "dfp754_d64.h"
#include "pcg_basic.h"
#include "bot.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

static qx_t maxq;
static qx_t basq = 100.dd;
static size_t N = 5U;

static unxs_exa_t acc = {0.dd, 0.dd};

static const char *cont;
static size_t conz;
#define INS		.ins = cont, .inz = conz


static int
init_rng(uint64_t seed)
{
	uint64_t stid = 0ULL;

	if (!seed) {
		seed = time(NULL);
		stid = (intptr_t)&seed;
	}
	pcg32_srandom(seed, stid);
	return 0;
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
hbeat_cb(bot_t b)
{
/* generate a random trade */
	qx_t q = 0.dd;
	omsg_t m = {OMSG_ORD, INS};
	clob_side_t s;

	for (size_t i = 0U; i < N; i += 32U) {
		unsigned int x = pcg32_random();
		for (size_t j = i, l = min(32U, N); j < l; j++, x >>= 1U) {
			q += x & 0b1U ? basq : -basq;
		}
	}
	if (q > 0.dd && !(acc.base + q > maxq)) {
		s = SIDE_LONG;
	} else if (q < 0.dd && !(-acc.base - q > maxq)) {
		s = SIDE_SHORT;
		q = -q;
	} else {
		/* not today then */
		return;
	}
	m.ord = (clob_ord_t){TYPE_MKT, s, .qty = {q, 0.dd}};
	add_omsg(b, m);
	bot_send(b);
	return;
}


#include "monkey.yucc"

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

	if (argi->summands_arg) {
		if (!(N = strtoul(argi->summands_arg, NULL, 0))) {
			fputs("\
Error: argument to summands must be positive.\n", stderr);
			rc = 1;
			goto out;
		}
	}

	if (argi->max_arg) {
		maxq = strtoqx(argi->max_arg, NULL);
	} else {
		maxq = NANQX;
	}

	if (argi->qty_arg) {
		if ((basq = strtoqx(argi->qty_arg, NULL)) <= 0.dd) {
			fputs("\
Error: argument to qty must be positive.\n", stderr);
			rc = 1;
			goto out;
		}
	}

	init_rng(0ULL);

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

	b->ochan_cb = ochan_cb;
	b->timer_cb = hbeat_cb;
	bot_set_timer(b, freq, freq);

	/* go go go */
	rc = run_bots(b) < 0;

kil:
	kill_bot(b);

out:
	yuck_free(argi);
	return rc;
}
