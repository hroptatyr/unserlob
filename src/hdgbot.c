#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <ev.h>
#if defined HAVE_DFP754_H
# include <dfp754.h>
#endif	/* HAVE_DFP754_H */
#include "dfp754_d64.h"
#include "pcg_basic.h"
#include "sock.h"
#include "nifty.h"

#define NSECS	(1000000000)
#define MCAST_ADDR	"ff05::134"
#define QUOTE_PORT	7978
#define TRADE_PORT	7979
#define DEBUG_PORT	7977

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))

typedef _Decimal64 px_t;
typedef _Decimal64 qx_t;
typedef long unsigned int tv_t;
#define strtopx		strtod64
#define pxtostr		d64tostr
#define strtoqx		strtod64
#define qxtostr		d64tostr

static int exec_chan = -1;


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
sigint_cb(EV_P_ ev_signal *UNUSED(w), int UNUSED(re))
{
	ev_unloop(EV_A_ EVUNLOOP_ALL);
	return;
}

static void
beef_cb(EV_P_ ev_io *w, int UNUSED(re))
{
/* something went on in the quote channel */
	char buf[1536U];
	ssize_t nrd;

	if (UNLIKELY((nrd = recv(w->fd, buf, sizeof(buf), 0)) <= 0)) {
		return;
	}
	/* now snarf the line */
	fwrite(buf, 1, nrd, stdout);
	return;
}

static void
cake_cb(EV_P_ ev_io *w, int UNUSED(re))
{
/* something went on in the quote channel */
	char buf[1536U];
	ssize_t nrd;

	if (UNLIKELY((nrd = recv(w->fd, buf, sizeof(buf), 0)) <= 0)) {
		return;
	}
	/* now snarf the line */
	fwrite(buf, 1, nrd, stdout);
	return;
}

static void
hbeat_cb(EV_P_ ev_timer *UNUSED(w), int UNUSED(re))
{
	/* generate a random trade */
	char buf[32U];
	int len;
	unsigned int q = pcg32_boundedrand(5U) + 1U;

	switch (pcg32_boundedrand(3U)) {
	case 0U:
		len = snprintf(buf, sizeof(buf), "BUY\t%u00\n", q);
		break;
	case 1U:
		len = snprintf(buf, sizeof(buf), "SELL\t%u00\n", q);
		break;
	default:
		/* not today then */
		return;
	}
	send(exec_chan, buf, len, 0);
	fwrite(buf, 1, len, stdout);
	return;
}


#include "hdgbot.yucc"

int
main(int argc, char *argv[])
{
	static yuck_t argi[1U];
	static struct ipv6_mreq r;
	ev_signal sigint_watcher[1U];
	ev_signal sigterm_watcher[1U];
	ev_timer hbeat[1U];
	ev_io beef[1U];
	ev_io cake[1U];
	const char *host = "localhost";
	/* use the default event loop unless you have special needs */
	struct ev_loop *loop;
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	if (argi->host_arg) {
		host = argi->host_arg;
	}

	init_rng(0ULL);

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	/* initialise a sig C-c handler */
	ev_signal_init(sigint_watcher, sigint_cb, SIGINT);
	ev_signal_start(EV_A_ sigint_watcher);
	ev_signal_init(sigterm_watcher, sigint_cb, SIGTERM);
	ev_signal_start(EV_A_ sigterm_watcher);

	/* open exec channel */
	if (UNLIKELY((exec_chan = connector(host, TRADE_PORT)) < 0)) {
		serror("\
Error: cannot open socket for execution messages");
		goto nop;
	}
	/* hook into our event loop */
	ev_io_init(cake, cake_cb, exec_chan, EV_READ);
	ev_io_start(EV_A_ cake);

	/* init the quote channel */
	with (int s) {
		if (UNLIKELY((s = mc6_socket()) < 0)) {
			serror("\
Error: cannot open socket");
			rc = 1;
			goto nop;
		} else if (mc6_join_group(
				   &r, s, MCAST_ADDR, QUOTE_PORT, NULL) < 0) {
			serror("\
Error: cannot join multicast group on socket %d", s);
			rc = 1;
			close(s);
			goto nop;
		}
		/* hook into our event loop */
		ev_io_init(beef, beef_cb, s, EV_READ);
		ev_io_start(EV_A_ beef);
	}

	/* start the heartbeat timer */
	ev_timer_init(hbeat, hbeat_cb, 1.0, 1.0);
	ev_timer_start(EV_A_ hbeat);

	/* now wait for events to arrive */
	ev_loop(EV_A_ 0);

	/* begin the freeing */
	with (int s = beef->fd) {
		ev_io_stop(EV_A_ beef);
		mc6_leave_group(s, &r);
		setsock_linger(s, 1);
		close(s);
	}

nop:
	/* destroy the default evloop */
	ev_default_destroy();

out:
	yuck_free(argi);
	return rc;
}
