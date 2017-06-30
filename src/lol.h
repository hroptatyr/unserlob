#if !defined INCLUDED_lol_h_
#define INCLUDED_lol_h_
#include <unistd.h>
#include <clob/clob.h>
#include <clob/unxs.h>

typedef long unsigned int tv_t;

typedef struct {
	clob_side_t s;
	px_t p;
	qx_t q;
} lol_quo_t;

typedef struct {
	enum {
		OMSG_UNK,
		/* hence */
		OMSG_ACC,
		OMSG_FIL,
		OMSG_KIL,
		OMSG_NOK,
		OMSG_OID,
		/* forth */
		OMSG_BUY,
		OMSG_SEL,
		OMSG_CAN,
	} typ;
	union {
		clob_oid_t oid;
		unxs_exa_t acc;
	};
} omsg_t;

typedef struct {
	enum {
		QMSG_UNK,
		QMSG_TOP,
		QMSG_LVL,
		QMSG_TRA,
	} typ;
	union {
		lol_quo_t quo;
	};
} qmsg_t;


extern omsg_t recv_omsg(const char *msg, size_t msz);
extern ssize_t send_omsg(char *restrict buf, size_t bsz, omsg_t msg);

extern qmsg_t recv_qmsg(const char *msg, size_t msz);
extern ssize_t send_qmsg(char *restrict buf, size_t bsz, qmsg_t msg);

#endif	/* INCLUDED_lol_h_ */
