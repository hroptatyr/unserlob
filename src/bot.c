#if defined HAVE_CONFIG_H
# include "config.h"
#endif	/* HAVE_CONFIG_H */
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ev.h>
#include "sock.h"
#include "lol.h"
#include "bot.h"
#include "nifty.h"

#undef EV_P
#define EV_P  struct ev_loop *loop __attribute__((unused))

#define NSECS	(1000000000)
#define MCAST_ADDR	"ff05::134"
#define QUOTE_PORT	7978
#define TRADE_PORT	7979
#define DEBUG_PORT	7977

/* private version of bot_t */
struct _bot_s {
	struct bot_s parent;

	ev_io och[1U];
	ev_io qch[1U];
	ev_timer tim[1U];
};


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


/* callbacks */
static void
_tim_cb(EV_P_ ev_timer *UNUSED(w), int UNUSED(re))
{
	bot_t b = w->data;

	if (LIKELY(b->timer_cb != NULL)) {
		b->timer_cb(b);
	}
	return;
}

static void
_och_cb(EV_P_ ev_io *w, int UNUSED(re))
{
/* something went on in the exec channel */
	static char buf[4096U];
	static size_t bof;
	bot_t b = w->data;
	size_t npr = 0U;
	ssize_t nrd;

	nrd = recv(w->fd, buf + bof, sizeof(buf) - bof, 0);
	if (UNLIKELY(nrd <= 0)) {
		return;
	} else if (b->ochan_cb == NULL) {
		bof = 0U;
		return;
	}
	/* up the bof */
	bof += nrd;
	/* now snarf the line */
	for (const char *x;
	     (x = memchr(buf + npr, '\n', bof - npr));
	     npr = x + 1U - buf) {
		omsg_t m = recv_omsg(buf + npr, x + 1U - (buf + npr));

		b->ochan_cb(b, m);
	}
	/* move left-overs */
	if (bof - npr) {
		memmove(buf, buf + npr, bof - npr);
	}
	bof -= npr;
	return;
}

static void
_qch_cb(EV_P_ ev_io *w, int UNUSED(re))
{
/* something went on in the quote channel */
	static char buf[4096U];
	static size_t bof;
	bot_t b = w->data;
	size_t npr = 0U;
	ssize_t nrd;

	nrd = recv(w->fd, buf + bof, sizeof(buf) - bof, 0);
	if (UNLIKELY(nrd <= 0)) {
		return;
	} else if (b->qchan_cb == NULL) {
		bof = 0U;
		return;
	}
	/* up the bof */
	bof += nrd;
	/* now snarf the line */
	for (const char *x;
	     (x = memchr(buf + npr, '\n', bof - npr));
	     npr = x + 1U - buf) {
		qmsg_t m = recv_qmsg(buf + npr, x + 1U - (buf + npr));

		b->qchan_cb(b, m);
	}
	/* move left-overs */
	if (bof - npr) {
		memmove(buf, buf + npr, bof - npr);
	}
	bof -= npr;
	return;
}


static struct ev_loop *loop;
static struct ipv6_mreq mreq;

bot_t
make_bot(const char *host)
{
	struct _bot_s *r = calloc(1, sizeof(*r));
	int s;
 
	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	if (UNLIKELY((s = connector(host, TRADE_PORT)) < 0)) {
		serror("\
Error: cannot open socket for execution messages");
		goto nil;
	}
	/* don't set up qch yet, see if they install a callback */
	r->qch->fd = -1;
	/* hook into our event loop */
	ev_io_init(r->och, _och_cb, s, EV_READ);
	ev_io_start(EV_A_ r->och);
	return (void*)r;

nil:
	free(r);
	return NULL;
}

int
run_bots(bot_t b, ...)
{
	int s;

	if (b->qchan_cb == NULL) {
		goto run;
	}

	/* otherwise set up multicast listener */
	if (UNLIKELY((s = mc6_socket()) < 0)) {
		serror("\
Error: cannot open socket");
		return -1;
	} else if (mc6_join_group(&mreq, s, MCAST_ADDR, QUOTE_PORT, NULL) < 0) {
		serror("\
Error: cannot join multicast group on socket %d", s);
		close(s);
		return -1;
	}
	/* hook into our event loop */
	with (struct _bot_s *r = (void*)b) {
		r->qch->data = b;
		ev_io_init(r->qch, _qch_cb, s, EV_READ);
		ev_io_start(EV_A_ r->qch);
	}
run:
	ev_loop(EV_A_ 0);
	return 0;
}

void
kill_bot(bot_t b)
{
	struct _bot_s *r = (void*)b;

	with (int s = r->och->fd) {
		ev_io_stop(EV_A_ r->och);
		setsock_linger(s, 1);
		close(s);
	}
	if_with (int s = r->qch->fd, s >= 0) {
		ev_io_stop(EV_A_ r->qch);
		mc6_leave_group(s, &mreq);
		setsock_linger(s, 1);
		close(s);
	}
	if (r->parent.timer_cb) {
		ev_timer_stop(EV_A_ r->tim);
	}
	ev_default_destroy();
	free(b);
	return;
}

int
bot_set_timer(bot_t b, double when, double intv)
{
	struct _bot_s *r = (void*)b;

	ev_timer_init(r->tim, _tim_cb, when, intv);
	r->tim->data = b;
	return 0;
}

/* lol.c ends here */
