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
static qx_t basq = 100.dd;

static unxs_exa_t acc = {0.dd, 0.dd};
static px_t last = NANPX;

static unsigned int contrarianp;

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
		last = m.quo.prc;
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
hbeat_cb(bot_t b)
{
	static size_t k1, k2;
	static px_t p1[16U], p2[64U];
	static unsigned int spos;

	p1[k1] = last;
	p2[k2] = last;
	k1 = (k1 + 1U) % countof(p1);
	k2 = (k2 + 1U) % countof(p2);

	with (px_t s1 = 0.dd, s2 = 0.dd) {
		for (size_t i = 0U; i < countof(p1); i++) {
			s1 += p1[i];
		}
		for (size_t i = 0U; i < countof(p2); i++) {
			s2 += p2[i];
		}
		with (unsigned int s = signbitd64((s1 * 4.dd - s2) / 64.dd)) {
			if (spos ^ s) {
				omsg_t m = {OMSG_ORD, INS};
				clob_side_t x = (s ^ contrarianp)
					? CLOB_SIDE_SHORT : CLOB_SIDE_LONG;
				qx_t q = fabsqx(acc.base);

				q = min(maxq - q, q + basq);
				m.ord = (clob_ord_t){CLOB_TYPE_MKT, x, {q, 0.dd}};
				add_omsg(b, m);
				bot_send(b);
				spos = s;
			}
		}
	}
	return;
}


#include "techbot.yucc"

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

	contrarianp = argi->contrarian_flag;

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
