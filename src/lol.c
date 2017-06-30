#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <string.h>
#include <stdio.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "dfp754_d64.h"
#include "lol.h"
#include "nifty.h"

#define strtopx		strtod64
#define pxtostr		d64tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr



/**
 * lobot language
 * - there's qmsg for trades and quotes
 * - there's omsg for orders and post-trade conversation
 *
 * QMSGs (wire):
 * <-A1, B1  top-level ask, bid
 * <-A2, B2  any-level ask, bid
 * <-TRA     trade
 *
 * OMSGs (wire):
 * ->BUY \t Q[+H] [\t P]  buy Q quantities at P or market, H is hidden
 * ->SEL \t Q[+H] [\t P]  sell Q quantities at P or market, H is hidden
 * <-FIL \t Q \t P        Q quantities got filled at P
 * <-ACC \t B \t T        account balance is B base and T terms
 * <-OID \t X             order was accepted with id X
 * ->CAN \t X             cancel order X
 * <-KIL \t X             response to cancel request, killed X
 * <-NOK \t X             response to cancel request, X not killed */

static ssize_t
send_oid(char *restrict buf, size_t bsz, clob_oid_t oid)
{
	static const char *sids[] = {"ASK ", "BID "};
	static const char *typs[] = {
		[TYPE_LMT] = "LMT ",
		[TYPE_MKT] = "MKT ",
	};
	size_t len = 0U;

	len += (memcpy(buf + len, typs[oid.typ], 4U), 4U);
	len += (memcpy(buf + len, sids[oid.sid], 4U), 4U);
	len += pxtostr(buf + len, sizeof(buf) - len, oid.prc);
	buf[len++] = ' ';
	len += snprintf(buf + len, sizeof(buf) - len, "%zu", oid.qid);
	buf[len++] = '\n';
	(void)bsz;
	return len;
}

static clob_oid_t
_recv_oid(const char *msg, size_t UNUSED(msz))
{
	clob_oid_t r;

	switch (msg[0U]) {
	case 'L':
		r.typ = TYPE_LMT;
		break;
	case 'M':
		r.typ = TYPE_MKT;
		break;
	default:
		goto nil;
	}
	r.sid = (clob_side_t)(msg[8U] - 'A');
	with (char *on) {
		r.prc = strtopx(msg + 12U, &on);
		if (*on++ != ' ') {
			goto nil;
		}
		/* and lastly read the qid */
		r.qid = strtoull(on, &on, 0);
	}
	return r;
nil:
	return (clob_oid_t){.qid = 0U};
}

static unxs_exa_t
_recv_acc(const char *msg, size_t UNUSED(msz))
{
	unxs_exa_t r;
	char *on;

	r.base = strtoqx(msg, &on);
	if (UNLIKELY(*on++ != '\t')) {
		goto nil;
	}
	r.term = strtoqx(msg, &on);
	return r;
nil:
	return (unxs_exa_t){};
}

static lol_quo_t
_recv_tra(const char *msg, size_t UNUSED(msz))
{
	lol_quo_t r = {NSIDES};
	char *on;

	r.p = strtopx(msg, &on);
	if (*on++ != '\t') {
		goto nil;
	}
	r.q = strtoqx(on, &on);
	return r;
nil:
	return (lol_quo_t){NSIDES, NANPX, NANQX};
}

static lol_quo_t
_recv_quo(const char *msg, size_t UNUSED(msz))
{
	lol_quo_t r;
	char *on;

	r.s = (clob_side_t)(*msg - 'A');
	r.p = strtopx(msg + 3U, &on);
	if (*on++ != '\t') {
		goto nil;
	}
	r.q = strtoqx(on, &on);
	return r;
nil:
	r.p = NANPX;
	r.q = NANQX;
	return r;
}


ssize_t
send_omsg(char *restrict buf, size_t bsz, omsg_t msg)
{
	switch (msg.typ) {
	case OMSG_BUY:
		memcpy(buf, "BUY\t", 4U);
		return 4U + 0U;
	case OMSG_SEL:
		memcpy(buf, "SEL\t", 4U);
		return 4U + 0U;
	case OMSG_CAN:
		memcpy(buf, "CAN\t", 4U);
		return 4U + send_oid(buf + 4U, bsz - 4U, msg.oid);

	default:
		/* can't send them other guys */
		break;
	}
	return -1;
}

omsg_t
recv_omsg(const char *msg, size_t msz)
{
	if (0) {
		;
	} else if (!memcmp(msg, "OID\t", 4U)) {
		return (omsg_t){OMSG_OID, .oid = _recv_oid(msg + 4U, msz - 4U)};
	} else if (!memcmp(msg, "ACC\t", 4U)) {
		return (omsg_t){OMSG_ACC, .acc = _recv_acc(msg + 4U, msz - 4U)};
	} else if (!memcmp(msg, "FIL\t", 4U)) {
		return (omsg_t){OMSG_FIL, .oid = _recv_oid(msg + 4U, msz - 4U)};
	} else if (!memcmp(msg, "KIL\t", 4U)) {
		return (omsg_t){OMSG_KIL, .oid = _recv_oid(msg + 4U, msz - 4U)};
	} else if (!memcmp(msg, "NOK\t", 4U)) {
		return (omsg_t){OMSG_NOK, .oid = _recv_oid(msg + 4U, msz - 4U)};
	}
	return (omsg_t){OMSG_UNK};
}

qmsg_t
recv_qmsg(const char *msg, size_t msz)
{
	switch (msg[0U]) {
	case 'A':
	case 'B':
		if (UNLIKELY(msg[2U] != '\t')) {
			break;
		}
		switch (msg[1U]) {
		case '1':
			return (qmsg_t){QMSG_TOP, .quo = _recv_quo(msg, msz)};
		case '2':
			return (qmsg_t){QMSG_LVL, .quo = _recv_quo(msg, msz)};
		default:
			break;
		}
		break;
	case 'T':
		if (memcmp(msg, "TRA\t", 4U)) {
			break;
		}
		return (qmsg_t){QMSG_TRA, .quo = _recv_tra(msg + 4U, msz - 4U)};
	default:
		break;
	}
	return (qmsg_t){QMSG_UNK};
}

/* lol.c ends here */
