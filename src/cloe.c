#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#elif defined HAVE_DFP_STDLIB_H
# include <dfp/stdlib.h>
#elif defined HAVE_DECIMAL_H
# include <decimal.h>
#endif	/* DFP754_H || HAVE_DFP_STDLIB_H || HAVE_DECIMAL_H */
#include "dfp754_d64.h"
#include "clob/clob.h"
#include "clob/unxs.h"
#include "clob/quos.h"
#include "clob/mmod-auction.h"
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

#define TYPE_AUC	((clob_type_t)0x10U)

static FILE *traout, *quoout;


static clob_ord_t
push_beef(const char *ln, size_t lz)
{
/* simple order protocol
 * BUY/LONG \t Q [\t P]
 * SELL/SHORT \t Q [\t P] */
	clob_ord_t o;
	char *on;

	if (UNLIKELY(!lz)) {
		goto bork;
	}
	switch (ln[0U]) {
	case 'F'/*INISH AUCTION*/:
		return (clob_ord_t){TYPE_AUC};
	case 'B'/*UY*/:
	case 'L'/*ONG*/:
	case 'b'/*uy*/:
	case 'l'/*ong*/:
		o.sid = SIDE_BID;
		break;
	case 'S'/*ELL|HORT*/:
	case 's'/*ell|hort*/:
		o.sid = SIDE_ASK;
		break;
	default:
		goto bork;
	}
	with (const char *x = strchr(ln, '\t')) {
		if (UNLIKELY(x == NULL)) {
			goto bork;
		}
		/* otherwise */
		lz -= x + 1U - ln;
		ln = x + 1U;
	}
	/* read quantity */
	o.qty.hid = 0.dd;
	o.qty.dis = strtoqx(ln, &on);
	if (LIKELY(*on > ' ')) {
		/* nope */
		goto bork;
	} else if (*on++ == '\t') {
		o.lmt = strtopx(on, &on);
		if (*on > ' ') {
			goto bork;
		}
		o.typ = TYPE_LMT;
	} else {
		o.typ = TYPE_MKT;
	}
	return o;
bork:
	return (clob_ord_t){(clob_type_t)-1};
}

static void
send_beef(unxs_exe_t x)
{
	char buf[256U];
	size_t len = (memcpy(buf, "TRA\t", 4U), 4U);

	len += qxtostr(buf + len, sizeof(buf) - len, x.qty);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, x.prc);
	buf[len++] = '\n';
	fwrite(buf, 1, len, stdout);
	return;
}

static void
send_cake(quos_msg_t m)
{
	char buf[256U];
	size_t len = 0U;

	buf[len++] = (char)('A' + m.sid);
	buf[len++] = '2';
	buf[len++] = '\t';
	len += pxtostr(buf + len, sizeof(buf) - len, m.prc);
	buf[len++] = '\t';
	len += qxtostr(buf + len, sizeof(buf) - len, m.new);
	buf[len++] = '\n';
	fwrite(buf, 1, len, quoout);
	return;
}


#include "cloe.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	int rc = 0;
	clob_t c;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	/* get going then */
	c = make_clob();
	switch (argi->cmd) {
	default:
		c.exe = make_unxs(MODE_BI);
		/* open the quote channel */
		if ((quoout = fdopen(3, "w+")) != NULL) {
			c.quo = make_quos();
		}
		break;
	case CLOE_CMD_AUCTION:
		c.exe = make_unxs(MODE_SC);
		break;
	}

	/* open the trade channel */
	traout = stdout;

	/* read orders from stdin */
	switch (argi->cmd) {
		char *line;
		size_t llen;
		ssize_t nrd;

	default:
		/* default mode is continuous trading */
		line = NULL, llen = 0U;
		while ((nrd = getline(&line, &llen, stdin)) > 0) {
			clob_ord_t o = push_beef(line, nrd);

			if (UNLIKELY(o.typ > TYPE_STP)) {
				fputs("Error: unreadable line\n", stderr);
				continue;
			}
			/* otherwise hand him to continuous trading */
			unxs_order(c, o, NANPX);
			/* print trades at the very least */
			for (size_t i = 0U; i < c.exe->n; i++) {
				send_beef(c.exe->x[i]);
			}
			unxs_clr(c.exe);
			if (quoout != NULL) {
				quos_t q = c.quo;
				for (size_t i = 0U; i < q->n; i++) {
					send_cake(q->m[i]);
				}
				quos_clr(q);
			}
		}
		free(line);
		break;

	case CLOE_CMD_AUCTION:
		/* auction mode, read until we see FINISH AUCTION */
		line = NULL, llen = 0U, nrd = 1;
		while (nrd > 0) {
			mmod_auc_t a;

			while ((nrd = getline(&line, &llen, stdin)) > 0) {
				clob_ord_t o = push_beef(line, nrd);

				if (UNLIKELY(o.typ > TYPE_AUC)) {
					fputs("Error: unreadable line\n", stderr);
					continue;
				} else if (o.typ == TYPE_AUC) {
					/* auction him */
					break;
				}
				/* otherwise track him in the book */
				clob_add(c, o);
				/* calculate theoretical auction price */
				a = mmod_auction(c);
			}
			/* auction him */
			unxs_auction(c, a.prc, a.qty);
			/* print trades at the very least */
			for (size_t i = 0U; i < c.exe->n; i++) {
				send_beef(c.exe->x[i]);
			}
			unxs_clr(c.exe);
		}
		free(line);
		break;
	}

	/* close quotes channel */
	if (quoout != NULL) {
		fclose(quoout);
		free_quos(c.quo);
	}
	if (traout != NULL) {
		fclose(traout);
	}
	free_unxs(c.exe);
	free_clob(c);

out:
	yuck_free(argi);
	return rc;
}
