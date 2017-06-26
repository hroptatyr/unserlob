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
#include "nifty.h"

#define strtoqx		strtod64
#define strtopx		strtod64
#define qxtostr		d64tostr
#define pxtostr		d64tostr

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
send_cake(clob_t c, unxs_exe_t x)
{
	char buf[256U];
	size_t len = 0U;

	if (UNLIKELY(isnanpx(x.prc))) {
		/* don't print nans */
		return;
	}
	len = pxtostr(buf + len, sizeof(buf) - len, x.prc);
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

	/* open the quote channel */
	if ((quoout = fdopen(3, "w+")) == NULL) {
		/* don't worry about him */
		;
	}

	/* open the trade channel */
	traout = stdout;

	/* read orders from stdin */
	c = make_clob();
	{
		char *line = NULL;
		size_t llen = 0UL;

		for (ssize_t nrd; (nrd = getline(&line, &llen, stdin)) > 0;) {
			unxs_exbi_t x[256U];
			clob_ord_t o = push_beef(line, nrd);
			size_t nx;

			if (UNLIKELY(o.typ > TYPE_STP)) {
				fputs("Error: unreadable line\n", stderr);
				continue;
			}
			/* otherwise hand him to continuous trading */
			nx = unxs_order(x, countof(x), c, o, NANPX);
			/* print trades at the very least */
			for (size_t i = 1U; i < nx; i++) {
				send_beef(x[i].x);
			}
			if (quoout != NULL) {
				if (!isnanpx(x->x.prc)) {
					send_cake(c, x->x);
				}
				for (size_t i = 1U; i < nx; i++) {
					if (x[i - 1U].x.prc == x[i].x.prc) {
						continue;
					}
					send_cake(c, x[i].x);
				}
			}
		}
	}
	free_clob(c);

	/* close quotes channel */
	if (quoout != NULL) {
		fclose(quoout);
	}
	if (traout != NULL) {
		fclose(traout);
	}

out:
	yuck_free(argi);
	return rc;
}
