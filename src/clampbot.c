#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <string.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include <clob/clob_val.h>
#include "dfp754_d64.h"
#include "bot.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

static quos_msg_t cquo[2U][NSIDES];
static qx_t Q = 400.dd;

static px_t dif;
static px_t quo;

static const char *cont[2U];
static size_t conz[2U];
#define INS0		.ins = cont[0U], .inz = conz[0U]
#define INS1		.ins = cont[1U], .inz = conz[1U]
#include <stdio.h>

static void
qchan_cb(bot_t b, qmsg_t m)
{
	omsg_t m0 = {INS0}, m1 = {INS1};

	switch (m.typ) {
	case QMSG_TOP:
		if (0) {
			;
		} else if (!memcmp(m.ins, cont[0U], conz[0U])) {
			cquo[0U][m.quo.sid] = m.quo;
			break;
		} else if (!memcmp(m.ins, cont[1U], conz[1U])) {
			cquo[1U][m.quo.sid] = m.quo;
			break;
		}
		/* fallthrough */
	default:
		return;
	}

	/* check opportunities */
	if (0) {
		;
	} else if (cquo[0U][SIDE_ASK].prc - cquo[1U][SIDE_BID].prc < dif ||
		   cquo[0U][SIDE_ASK].prc / cquo[1U][SIDE_BID].prc < quo) {
		qx_t q = min(Q, min(cquo[0U][SIDE_ASK].new, cquo[1U][SIDE_BID].new));
		if (q <= 0.dd) {
			/* not enough size */
			return;
		}
		/* go long C0/C1-spread */
		m0.typ = OMSG_BUY;
		m0.ord = (clob_ord_t){TYPE_MKT, .qty = {q, 0.dd}};
		m1.typ = OMSG_SEL;
		m1.ord = (clob_ord_t){TYPE_MKT, .qty = {q, 0.dd}};
	} else if (cquo[0U][SIDE_BID].prc - cquo[1U][SIDE_ASK].prc > dif ||
		   cquo[0U][SIDE_BID].prc / cquo[1U][SIDE_ASK].prc > quo) {
		qx_t q = min(Q, min(cquo[0U][SIDE_BID].new, cquo[1U][SIDE_ASK].new));
		if (q <= 0.dd) {
			/* not enough size */
			return;
		}
		/* go short C0/C1-spread */
		m0.typ = OMSG_SEL;
		m0.ord = (clob_ord_t){TYPE_MKT, .qty = {q, 0.dd}};
		m1.typ = OMSG_BUY;
		m1.ord = (clob_ord_t){TYPE_MKT, .qty = {q, 0.dd}};
	} else {
		/* nope, no opportunities today */
		return;
	}
	char buf[256];
	buf[send_omsg(buf, sizeof(buf), m0)] = '\0';
	fputs(buf, stdout);
	buf[send_omsg(buf, sizeof(buf), m1)] = '\0';
	fputs(buf, stdout);

	add_omsg(b, m0);
	add_omsg(b, m1);
	bot_send(b);
	return;
}


#include "clampbot.yucc"

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

	if (argi->nargs != 2U) {
		fputs("\
Error: need exactly 2 contracts\n", stderr);
		rc = 1;
		goto out;
	}
	cont[0U] = argi->args[0U];
	cont[1U] = argi->args[1U];
	conz[0U] = strlen(cont[0U]);
	conz[1U] = strlen(cont[1U]);

	if (argi->dif_arg) {
		dif = strtopx(argi->dif_arg, NULL);
	} else {
		dif = NANPX;
	}

	if (argi->quo_arg) {
		quo = strtopx(argi->quo_arg, NULL);
	} else {
		quo = NANPX;
	}

	if (isnanpx(dif) && isnanpx(quo)) {
		fputs("\
Error: need a difference or quotient spread.\n", stderr);
		rc = 1;
		goto out;
	}

	if (argi->quantity_arg) {
		Q = strtoqx(argi->quantity_arg, NULL);
	}

	if (argi->host_arg) {
		host = argi->host_arg;
	}

	cquo[0U][SIDE_BID].prc = NANPX;
	cquo[0U][SIDE_ASK].prc = NANPX;
	cquo[1U][SIDE_BID].prc = NANPX;
	cquo[1U][SIDE_ASK].prc = NANPX;

	/* initialise the bot */
	if (UNLIKELY((b = make_bot(host)) == NULL)) {
		goto out;
	}

	b->qchan_cb = qchan_cb;

	/* go go go */
	rc = run_bots(b) < 0;

	kill_bot(b);

out:
	yuck_free(argi);
	return rc;
}
