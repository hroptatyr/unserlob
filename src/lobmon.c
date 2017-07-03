#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <time.h>
#include <ev.h>
#include "sock.h"
#include "nifty.h"

#define NSECS	(1000000000)
#define UDP_MULTICAST_TTL	64
#define MCAST_ADDR	"ff05::134"
#define MCAST_PORT	7978

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))

typedef long unsigned int tv_t;


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


static void
sigint_cb(EV_P_ ev_signal *UNUSED(w), int UNUSED(revents))
{
	ev_unloop(EV_A_ EVUNLOOP_ALL);
	return;
}

static void
beef_cb(EV_P_ ev_io *w, int UNUSED(revents))
{
	struct sockaddr_in6 sa;
	socklen_t sz = sizeof(sa);
	char buf[1536U];
	ssize_t nrd;
	tv_t metr;

	if (UNLIKELY((nrd = recvfrom(
			      w->fd, buf, sizeof(buf), 0,
			      (struct sockaddr*)&sa, &sz)) < 0)) {
		return;
	}
	/* set metronome before anything else */
	with (struct timespec tsp) {
		clock_gettime(CLOCK_REALTIME, &tsp);
		metr = tsp.tv_sec * NSECS + tsp.tv_nsec;
	}
	/* now snarf the line */
	buf[nrd] = '\0';
	fprintf(stdout, "%lu.%09lu\t%d\t", metr / NSECS, metr % NSECS, w->fd);
	if (inet_ntop(
		    AF_INET6, &sa.sin6_addr,
		    buf + nrd, sizeof(buf) - nrd) != NULL) {
		fputc('[', stdout);
		fputs(buf + nrd, stdout);
		fputc(']', stdout);
	}
	fprintf(stdout, ":%hu\t", ntohs(sa.sin6_port));
	fwrite(buf, sizeof(*buf), nrd, stdout);
	return;
}


#include "lobmon.yucc"

int
main(int argc, char *argv[])
{
/* grep -F 'EURUSD FAST Curncy' | cut -f1,5,6 */
	static yuck_t argi[1U];
	static struct ipv6_mreq r[3U];
	ev_signal sigint_watcher[1U];
	ev_io beef[3U];
	/* use the default event loop unless you have special needs */
	struct ev_loop *loop;
	int rc = 0;

	if (yuck_parse(argi, argc, argv) < 0) {
		rc = 1;
		goto out;
	}

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	/* initialise a sig C-c handler */
	ev_signal_init(sigint_watcher, sigint_cb, SIGINT);
	ev_signal_start(EV_A_ sigint_watcher);

	/* init the multicast socket */
	if (!argi->info_flag) {
		int s;
		if (UNLIKELY((s = mc6_socket()) < 0)) {
			serror("\
Error: cannot open socket");
			rc = 1;
			goto nop;
		} else if (mc6_join_group(
				   r + 0U, s,
				   MCAST_ADDR, MCAST_PORT, NULL) < 0) {
			serror("\
Error: cannot join multicast group on socket %d", s);
			rc = 1;
			close(s);
			goto nop;
		}
		/* hook into our event loop */
		ev_io_init(beef + 0U, beef_cb, s, EV_READ);
		ev_io_start(EV_A_ beef + 0U);
	}

	if (!argi->info_flag) {
		int s;
		if (UNLIKELY((s = mc6_socket()) < 0)) {
			serror("\
Error: cannot open socket");
			rc = 1;
			goto nop;
		} else if (mc6_join_group(
				   r + 1U, s,
				   MCAST_ADDR, MCAST_PORT + 1U, NULL) < 0) {
			serror("\
Error: cannot join multicast group on socket %d", s);
			rc = 1;
			close(s);
			goto nop;
		}
		/* hook into our event loop */
		ev_io_init(beef + 1U, beef_cb, s, EV_READ);
		ev_io_start(EV_A_ beef + 1U);
	}

	with (int s) {
		if (UNLIKELY((s = mc6_socket()) < 0)) {
			serror("\
Error: cannot open socket");
			rc = 1;
			goto nop;
		} else if (mc6_join_group(
				   r + 2U, s,
				   MCAST_ADDR, MCAST_PORT - 1U, NULL) < 0) {
			serror("\
Error: cannot join multicast group on socket %d", s);
			rc = 1;
			close(s);
			goto nop;
		}
		/* hook into our event loop */
		ev_io_init(beef + 2U, beef_cb, s, EV_READ);
		ev_io_start(EV_A_ beef + 2U);
	}

	/* now wait for events to arrive */
	ev_loop(EV_A_ 0);


	/* begin the freeing */
	with (int s = beef[0U].fd) {
		ev_io_stop(EV_A_ beef + 0U);
		mc6_leave_group(s, r + 0U);
		setsock_linger(s, 1);
		close(s);
	}
	with (int s = beef[1U].fd) {
		ev_io_stop(EV_A_ beef + 1U);
		mc6_leave_group(s, r + 1U);
		setsock_linger(s, 1);
		close(s);
	}
	with (int s = beef[2U].fd) {
		ev_io_stop(EV_A_ beef + 2U);
		mc6_leave_group(s, r + 2U);
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
