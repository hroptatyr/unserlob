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

/* this one should mirror the anonymous struct in lol.h */
struct fil_s {
	clob_oid_t fid;
	unxs_exe_t exe;
	uid_t con;
};


/**
 * lobot language
 * - there's qmsg for trades and quotes
 * - there's omsg for pre-trade and post-trade conversation
 *
 * QMSGs (wire):
 * <-A1, B1  top-level ask, bid
 * <-A2, B2  any-level ask, bid
 * <-TRA     trade
 *
 * OMSGs (wire):
 * ->BUY \t INS \t Q[+H] [\t P]  buy Q quantities at P or market, H is hidden
 * ->SEL \t INS \t Q[+H] [\t P]  sell Q quantities at P or market, H is hidden
 * <-FIL \t INS \t Q \t P \t U [\t X]  Order X has Q quantities filled at P,
 *                                     contra firm is U
 * <-ACC \t INS \t B \t T        account balance is B base and T terms
 * <-OID \t INS \t X             order was accepted with id X
 * ->CAN \t INS \t X             cancel order X
 * <-KIL \t INS \t X             response to cancel request, killed X
 * <-NOK \t INS \t X             response to cancel request, X not killed */

static ssize_t
_send_oid(char *restrict buf, size_t bsz, clob_oid_t oid)
{
	static const char *sids[] = {"ASK ", "BID "};
	static const char *typs[] = {
		[TYPE_LMT] = "LMT ",
		[TYPE_MKT] = "MKT ",
	};
	size_t len = 0U;

	len += (memcpy(buf + len, typs[oid.typ], 4U), 4U);
	len += (memcpy(buf + len, sids[oid.sid], 4U), 4U);
	len += pxtostr(buf + len, bsz - len, oid.prc);
	buf[len++] = ' ';
	len += snprintf(buf + len, bsz - len, "%zu", oid.qid);
	buf[len++] = '\n';
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
	r.sid = (clob_side_t)(msg[4U] - 'A');
	with (char *on) {
		r.prc = strtopx(msg + 8U, &on);
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

static ssize_t
_send_ord(char *restrict buf, size_t bsz, clob_ord_t ord)
{
	size_t len = 0U;

	len += qxtostr(buf + len, bsz - len, ord.qty.dis);
	if (ord.qty.hid > 0.dd) {
		buf[len++] = '+';
		len += qxtostr(buf + len, bsz - len, ord.qty.hid);
	}
	if (ord.typ == TYPE_LMT) {
		buf[len++] = '\t';
		len += pxtostr(buf + len, bsz - len, ord.lmt);
	}
	buf[len++] = '\n';
	return len;
}

static clob_ord_t
_recv_ord(const char *msg, const char *ord, size_t UNUSED(msz))
{
	clob_ord_t r = {TYPE_MKT};
	char *on;

	switch (*msg) {
	case 'B':
		r.sid = SIDE_LONG;
		break;
	case 'S':
		r.sid = SIDE_SHORT;
		break;
	}

	/* read quantity */
	r.qty.dis = strtoqx(ord, &on);
	if (UNLIKELY(*on == '+')) {
		r.qty.hid = strtoqx(++on, &on);
	} else {
		r.qty.hid = 0.dd;
	}
	if (*on++ == '\t') {
		/* got limit prices as well */
		r.lmt = strtopx(on, &on);
		r.typ = TYPE_LMT;
	}
	return r;
}

static ssize_t
_send_exa(char *restrict buf, size_t bsz, unxs_exa_t a)
{
	size_t len = 0U;

	len += qxtostr(buf + len, bsz - len, a.base);
	buf[len++] = '\t';
	len += qxtostr(buf + len, bsz - len, a.term);
	buf[len++] = '\n';
	return len;
}

static unxs_exa_t
_recv_exa(const char *msg, size_t UNUSED(msz))
{
	unxs_exa_t r;
	char *on;

	r.base = strtoqx(msg, &on);
	if (UNLIKELY(*on++ != '\t')) {
		goto nil;
	}
	r.term = strtoqx(on, &on);
	return r;
nil:
	return (unxs_exa_t){};
}

static ssize_t
_send_exe(char *restrict buf, size_t bsz, unxs_exe_t x, uid_t u)
{
	size_t len = 0U;

	len += qxtostr(buf + len, bsz - len, x.qty);
	buf[len++] = '\t';
	len += pxtostr(buf + len, bsz - len, x.prc);
	buf[len++] = '\t';
	len += snprintf(buf + len, bsz - len, "%u", u);
	buf[len++] = '\n';
	return len;
}

static struct fil_s
_recv_fil(const char *msg, size_t msz)
{
	clob_oid_t o;
	unxs_exe_t r;
	uid_t u;
	char *on;

	r.qty = strtoqx(msg, &on);
	if (*on++ != '\t') {
		goto nil;
	}
	r.prc = strtopx(on, &on);
	u = strtoul(on, &on, 0);
	/* there's possibly an order id coming now */
	if (*on++ == '\t') {
		o = _recv_oid(on, msz - (on - msg));
	}
	return (struct fil_s){o, r, u};
nil:
	return (struct fil_s){.exe = (unxs_exe_t){0.dd, NANPX}};
}

static quos_msg_t
_recv_tra(const char *msg, size_t UNUSED(msz))
{
	quos_msg_t r = {NSIDES};
	char *on;

	r.prc = strtopx(msg, &on);
	if (*on++ != '\t') {
		goto nil;
	}
	r.new = strtoqx(on, &on);
	return r;
nil:
	return (quos_msg_t){NSIDES, NANPX, 0.dd};
}

static mmod_auc_t
_recv_auc(const char *msg, size_t UNUSED(msz))
{
	mmod_auc_t r;
	char *on;

	r.prc = strtopx(msg, &on);
	if (*on++ != '\t') {
		goto nil;
	}
	r.qty = strtoqx(on, &on);
	r.imb = 0.dd;
	return r;
nil:
	return (mmod_auc_t){NANPX, 0.dd, 0.dd};
}

static mmod_auc_t
_recv_imb(const char *msg, size_t UNUSED(msz))
{
	mmod_auc_t r;
	char *on;

	r.prc = strtopx(msg, &on);
	if (*on++ != '\t') {
		goto nil;
	}
	r.imb = strtoqx(on, &on);
	r.qty = 0.dd;
	return r;
nil:
	return (mmod_auc_t){NANPX, 0.dd, 0.dd};
}

static quos_msg_t
_recv_quo(const char *msg, const char *quo, size_t UNUSED(msz))
{
	quos_msg_t r;
	char *on;

	r.sid = (clob_side_t)(*msg - 'A');
	r.prc = strtopx(quo, &on);
	if (*on++ != '\t') {
		goto nil;
	}
	r.new = strtoqx(on, &on);
	return r;
nil:
	r.prc = NANPX;
	r.new = 0.dd;
	return r;
}


ssize_t
send_omsg(char *restrict buf, size_t bsz, omsg_t msg)
{
	/* must be the same order as omsg enum */
	static const char typ[] = "\
UNK\tACC\tFIL\tKIL\tNOK\tOID\tCAN\tORD\t";
	size_t len = 0U;

	len += (memcpy(buf, typ + 4U * msg.typ, 4U), 4U);
	len += (memcpy(buf + 4U, msg.ins, msg.inz), msg.inz);
	buf[len++] = '\t';

	switch (msg.typ) {
		static const char sid[] = "\
SEL\tBUY\t";

	case OMSG_ORD:
		memcpy(buf, sid + 4U * msg.ord.sid, 4U);
		return len + _send_ord(buf + len, bsz - len, msg.ord);
	case OMSG_CAN:
		return len + _send_oid(buf + len, bsz - len, msg.oid);

	case OMSG_ACC:
		return len + _send_exa(buf + len, bsz - len, msg.exa);
	case OMSG_FIL:
		len += _send_exe(buf + len, bsz - len, msg.exe, msg.con);
		/* possibly sending the order id as well */
		if (msg.fid.qid) {
			buf[len - 1] = '\t';
			len += _send_oid(buf + len, bsz - len, msg.fid);
		}
		return len;
	case OMSG_KIL:
		return len + _send_oid(buf + len, bsz - len, msg.oid);
	case OMSG_NOK:
		return len + _send_oid(buf + len, bsz - len, msg.oid);
	case OMSG_OID:
		return len + _send_oid(buf + len, bsz - len, msg.oid);

	default:
		/* can't send them other guys */
		break;
	}
	return -1;
}

omsg_t
recv_omsg(const char *msg, size_t msz)
{
	const char *ins, *eoi;

	eoi = memchr(ins = msg + 4U, '\t', msz - 4U);
	msz -= eoi - msg;

	if (UNLIKELY(eoi == NULL)) {
		;
	} else if (!memcmp(msg, "OID\t", 4U)) {
		return (omsg_t){OMSG_OID, .ins = ins, .inz = eoi - ins,
				.oid = _recv_oid(eoi + 1U, msz - 1U)};
	} else if (!memcmp(msg, "ACC\t", 4U)) {
		return (omsg_t){OMSG_ACC, .ins = ins, .inz = eoi - ins,
				.exa = _recv_exa(eoi + 1U, msz - 1U)};
	} else if (!memcmp(msg, "FIL\t", 4U)) {
		struct fil_s f = _recv_fil(eoi + 1U, msz - 1U);
		return (omsg_t){OMSG_FIL, .ins = ins, .inz = eoi - ins,
				.fid = f.fid, .exe = f.exe, .con = f.con};
	} else if (!memcmp(msg, "KIL\t", 4U)) {
		return (omsg_t){OMSG_KIL, .ins = ins, .inz = eoi - ins,
				.oid = _recv_oid(eoi + 1U, msz - 1U)};
	} else if (!memcmp(msg, "NOK\t", 4U)) {
		return (omsg_t){OMSG_NOK, .ins = ins, .inz = eoi - ins,
				.oid = _recv_oid(eoi + 1U, msz - 1U)};
	} else if (!memcmp(msg, "BUY\t", 4U)) {
		return (omsg_t){OMSG_ORD, .ins = ins, .inz = eoi - ins,
				.ord = _recv_ord(msg, eoi + 1U, msz - 1U)};
	} else if (!memcmp(msg, "SEL\t", 4U)) {
		return (omsg_t){OMSG_ORD, .ins = ins, .inz = eoi - ins,
				.ord = _recv_ord(msg, eoi + 1U, msz - 1U)};
	} else if (!memcmp(msg, "CAN\t", 4U)) {
		return (omsg_t){OMSG_CAN, .ins = ins, .inz = eoi - ins,
				.oid = _recv_oid(eoi + 1U, msz - 1U)};
	}
	/* i shouldn't receive an ORD message */
	return (omsg_t){OMSG_UNK};
}

ssize_t
send_qmsg(char *restrict buf, size_t bsz, qmsg_t msg)
{
	size_t len = 0U;

	switch (msg.typ) {
	case QMSG_TOP:
	case QMSG_LVL:
		switch (msg.quo.sid) {
		case SIDE_ASK:
		case SIDE_BID:
			buf[len++] = (char)(msg.quo.sid + 'A');
			buf[len++] = (char)(msg.typ ^ '0');
			buf[len++] = '\t';
			goto quo;
		}
		return 0;
	case QMSG_TRA:
		len = (memcpy(buf + len, "TRA\t", 4U), 4U);
	quo:
		len += (memcpy(buf + len, msg.ins, msg.inz), msg.inz);
		buf[len++] = '\t';
		len += pxtostr(buf + len, bsz - len, msg.quo.prc);
		buf[len++] = '\t';
		len += qxtostr(buf + len, bsz - len, msg.quo.new);
		buf[len++] = '\n';
		break;
	case QMSG_AUC:
		len = (memcpy(buf + len, "AUC\t", 4U), 4U);
		len += (memcpy(buf + len, msg.ins, msg.inz), msg.inz);
		buf[len++] = '\t';
		len += pxtostr(buf + len, bsz - len, msg.auc.prc);
		buf[len++] = '\t';
		len += qxtostr(buf + len, bsz - len, msg.auc.qty);
		buf[len++] = '\n';
		break;
	case QMSG_IMB:
		len = (memcpy(buf + len, "IMB\t", 4U), 4U);
		len += (memcpy(buf + len, msg.ins, msg.inz), msg.inz);
		buf[len++] = '\t';
		len += pxtostr(buf + len, bsz - len, msg.auc.prc);
		buf[len++] = '\t';
		len += qxtostr(buf + len, bsz - len, msg.auc.imb);
		buf[len++] = '\n';
		break;
	default:
		return 0;
	}
	return len;
}

qmsg_t
recv_qmsg(const char *msg, size_t msz)
{
	switch (msg[0U]) {
		const char *ins, *eoi;

	case 'A':
		if (!memcmp(msg, "AUC\t", 4U)) {
			goto auc;
		}
	case 'B':
		if (UNLIKELY(msg[2U] != '\t')) {
			break;
		}
		/* snarf instrument */
		ins = msg + 3U;
		eoi = memchr(msg + 3U, '\t', msz - 3U);
		msz -= eoi + 1U - msg;
		if (UNLIKELY(eoi == NULL)) {
			break;
		}
		switch (msg[1U]) {
		case '1':
			return (qmsg_t){QMSG_TOP, ins, eoi - ins,
					.quo = _recv_quo(msg, eoi + 1U, msz)};
		case '2':
			return (qmsg_t){QMSG_LVL, ins, eoi - ins,
					.quo = _recv_quo(msg, eoi + 1U, msz)};
		default:
			break;
		}
		break;
	case 'I':
		if (memcmp(msg, "IMB\t", 4U)) {
			break;
		}
		/* snarf instrument */
		ins = msg + 4U;
		eoi = memchr(msg + 4U, '\t', msz - 4U);
		msz -= eoi - msg;
		if (UNLIKELY(eoi == NULL)) {
			break;
		}
		return (qmsg_t){QMSG_IMB, ins, eoi - ins,
				.auc = _recv_imb(eoi + 1U, msz - 1U)};
		
	case 'T':
		if (memcmp(msg, "TRA\t", 4U)) {
			break;
		}
		/* snarf instrument */
		ins = msg + 4U;
		eoi = memchr(msg + 4U, '\t', msz - 4U);
		msz -= eoi - msg;
		if (UNLIKELY(eoi == NULL)) {
			break;
		}
		return (qmsg_t){QMSG_TRA, ins, eoi - ins,
				.quo = _recv_tra(eoi + 1U, msz - 1U)};
	auc:
		/* snarf instrument */
		ins = msg + 4U;
		eoi = memchr(msg + 4U, '\t', msz - 4U);
		msz -= eoi - msg;
		if (UNLIKELY(eoi == NULL)) {
			break;
		}
		return (qmsg_t){QMSG_AUC, ins, eoi - ins,
				.auc = _recv_auc(eoi + 1U, msz - 1U)};
	default:
		break;
	}
	return (qmsg_t){QMSG_UNK};
}

/* lol.c ends here */
