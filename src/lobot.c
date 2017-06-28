#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#elif defined HAVE_DFP_STDLIB_H
# include <dfp/stdlib.h>
#elif defined HAVE_DECIMAL_H
# include <decimal.h>
#endif	/* DFP754_H || HAVE_DFP_STDLIB_H || HAVE_DECIMAL_H */
#include <ev.h>
#include <errno.h>
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

#define NSECS	(1000000000)
#define MCAST_ADDR	"ff05::134"
#define QUOTE_PORT	7978
#define TRADE_PORT	7979
#define DEBUG_PORT	7977

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))


static __attribute__((format(printf, 1, 2))) void
serror(const char *fmt, ...)
{
	va_list vap;
	va_start(vap, fmt);
	vfprintf(stderr, fmt, vap);
	va_end(vap);
	if (errno) {
		fputc(':', stderr);
		fputc(' ', stderr);
		fputs(strerror(errno), stderr);
	}
	fputc('\n', stderr);
	return;
}


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
	//fwrite(buf, 1, len, stdout);
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
	//fwrite(buf, 1, len, quoout);
	return;
}


static void
beef_cb(EV_P_ ev_io *w, int UNUSED(re))
{
	return;
}


/* socket goodies */
#include "sock.c"


#include "lobot.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	ev_io beef[1U];
	/* use the default event loop unless you have special needs */
	struct ev_loop *loop;
	int rc = 0;
	clob_t c;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->daemonise_flag && daemon(0, 0) < 0) {
		serror("\
Error: cannot run in daemon mode");
		rc = 1;
		goto out;
	}

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	/* init the multicast socket */
	with (int s = listener(TRADE_PORT)) {
		if (UNLIKELY(s < 0)) {
			serror("\
Error: cannot open socket");
			rc = 1;
			goto nop;
		}
		/* hook into our event loop */
		ev_io_init(beef, beef_cb, s, EV_READ);
		ev_io_start(EV_A_ beef);
	}

	/* get going then */
	c = make_clob();
	c.exe = make_unxs(MODE_BI);
	c.quo = make_quos();

	/* now wait for events to arrive */
	ev_loop(EV_A_ 0);

	/* begin the freeing */
	with (int s = beef->fd) {
		ev_io_stop(EV_A_ beef);
		setsock_linger(s, 1);
		close(s);
	}

	free_quos(c.quo);
	free_unxs(c.exe);
	free_clob(c);

nop:
	/* destroy the default evloop */
	ev_default_destroy();

out:
	yuck_free(argi);
	return rc;
}
