#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <string.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "dfp754_d64.h"
#include "bot.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

#define copysignpx	copysignd64
#define copysignqx	copysignd64

static qty_t Q = {1.dd, 0.dd};
static clob_side_t dir = NSIDES;
static unsigned int pingpongp;

static clob_oid_t coid[NSIDES];
static unxs_exa_t acc = {0.dd, 0.dd};
static qx_t pnl = 0.dd;
static px_t tabs;
static px_t trel;

static const char *cont;
static size_t conz;
#define INS		.ins = cont, .inz = conz

#if defined __INTEL_COMPILER
# pragma warning (disable: 188)
# pragma warning (disable: 589)
#endif	/* __INTEL_COMPILER */


static void
ochan_cb(bot_t b, omsg_t m)
{
	switch (m.typ) {
		px_t mean, labs;

	case OMSG_ACC:
		acc = m.exa;
		/* cancel any outstanding orders */
		add_omsg(b, (omsg_t){OMSG_CAN, INS, .oid = coid[SIDE_BID]});
		add_omsg(b, (omsg_t){OMSG_CAN, INS, .oid = coid[SIDE_ASK]});

		/* store pnl if we're flat */
		if (acc.base == 0.dd) {
			pnl = acc.term;
			goto send;
		}

		/* calc mean price otherwise */
		mean = (px_t)quantizeqx((pnl - acc.term) / acc.base, acc.term);
#if 0
/* contract size */
		labs = (px_t)quantizeqx((qx_t)tabs / acc.base, acc.term);
		if (UNLIKELY(labs == 0.dd)) {
			labs = scalbnd64(1.dd, quantexpd64(acc.term));
		}
#else
		labs = copysignd64(tabs, acc.base);
#endif
		/* put bracket order */
		const qx_t babs = fabsqx(acc.base);
		clob_ord_t tak = {
			TYPE_LMT, (acc.base < 0.dd),
			.qty = {Q.dis, babs + Q.hid - (!pingpongp ? Q.dis : 0.dd)},
			.lmt = mean + labs
		};
		clob_ord_t mor = {
			TYPE_LMT, (acc.base > 0.dd),
			.qty = {Q.dis, babs + Q.hid},
			.lmt = mean - labs
		};
	
		add_omsg(b, (omsg_t){OMSG_ORD, INS, .ord = tak});
		add_omsg(b, (omsg_t){OMSG_ORD, INS, .ord = mor});
	send:
		bot_send(b);
		break;
	case OMSG_OID:
		coid[m.oid.sid] = m.oid;
		break;
	default:
		break;
	}
	return;
}

static void
hbeat_cb(bot_t b)
{
	if (acc.base) {
		/* there should be a bracket out there already */
		return;
	}
	add_omsg(b, (omsg_t){OMSG_ORD, INS, .ord = {TYPE_MKT, dir, Q}});
	bot_send(b);
	return;
}


#include "roulbot.yucc"

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

	if (argi->nargs) {
		cont = *argi->args;
		conz = strlen(cont);
	}

	if (argi->host_arg) {
		host = argi->host_arg;
	}

	pingpongp = argi->ping_pong_flag;

	if (!argi->target_arg) {
		fputs("\
Error: need a target, how about --target=10%?\n", stderr);
		rc = 1;
		goto out;
	}
	/* parse target */
	with (char *on) {
		tabs = strtopx(argi->target_arg, &on);

		if (tabs < 0.dd) {
			dir = SIDE_SHORT;
		} else if (tabs > 0.dd) {
			dir = SIDE_LONG;
		} else {
			goto inv;
		}
		/* make us absolute */
		tabs = fabsd64(tabs);

		switch (*on) {
		case '\0':
			/* all is good */
			break;
		case '%':
			/* oooh relative they want */
			trel = tabs;
			tabs = 0.dd;
			fputs("\
Error: relative targets not implemented yet, can you code it?\n", stderr);
			rc = 1;
			goto out;
		default:
		inv:
			fputs("\
Error: invalid specification of target, should be [+/-]P or [+/-]P%\n", stderr);
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

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	b->ochan_cb = ochan_cb;
	b->timer_cb = hbeat_cb;
	bot_set_timer(b, 0.0, 5.0);

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
