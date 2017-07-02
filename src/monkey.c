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
hbeat_cb(bot_t b)
{
/* generate a random trade */
	unsigned int x = pcg32_random();
	qx_t q = 0.dd;
	omsg_t m;

	for (size_t i = 0U; i < 5U; i++, x >>= 1U) {
		q += x & 0b1U ? 100.dd : -100.dd;
	}
	if (q > 0.dd) {
		m.typ = OMSG_BUY;
	} else if (q < 0.dd) {
		m.typ = OMSG_SEL;
		q = -q;
	} else {
		/* not today then */
		return;
	}
	m.ord = (clob_ord_t){TYPE_MKT, .qty = {q, 0.dd}};
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

	if (argi->host_arg) {
		host = argi->host_arg;
	}

	if (argi->freq_arg) {
		freq = strtod(argi->freq_arg, NULL);
	}

	init_rng(0ULL);

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	b->timer_cb = hbeat_cb;
	bot_set_timer(b, 0., freq);

	/* go go go */
	rc = run_bots(b) < 0;

	kill_bot(b);

out:
	yuck_free(argi);
	return rc;
}
