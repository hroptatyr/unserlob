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

	/* message buffer, qchan buffer, ochan buffer offsets */
	size_t mbof;
	size_t qbof;
	size_t obof;

	char buf[];
};

#define BOTSIZ		(8192U)
#define BUFOFF		(offsetof(struct _bot_s, buf))
#define QBOF0		(0U)
#define QBOFZ		(BOTSIZ / 4U - BUFOFF)
#define OBOF0		(BOTSIZ / 4U - BUFOFF)
#define OBOFZ		(BOTSIZ - OBOF0)
#define MBOF0		(BOTSIZ / 2U - BUFOFF)
#define MBOFZ		(BOTSIZ - MBOF0)


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
	struct _bot_s *r = w->data;
	size_t npr = OBOF0;
	ssize_t nrd;

	nrd = recv(w->fd, r->buf + r->obof, OBOFZ - r->obof, 0);
	if (UNLIKELY(nrd <= 0)) {
		return;
	} else if (r->parent.ochan_cb == NULL) {
		goto reset;
	}
	/* up the bof */
	r->obof += nrd;
	/* now snarf the line */
	for (const char *x;
	     (x = memchr(r->buf + npr, '\n', r->obof - npr));
	     npr = x + 1U - r->buf) {
		omsg_t m = recv_omsg(r->buf + npr, x + 1U - (r->buf + npr));

		r->parent.ochan_cb((void*)r, m);
	}
	/* move left-overs */
	if (r->obof > npr) {
		memmove(r->buf + OBOF0, r->buf + npr, r->obof - npr);
		r->obof -= npr;
	} else {
	reset:
		r->obof = 0U;
	}
	r->obof += OBOF0;
	return;
}

static void
_qch_cb(EV_P_ ev_io *w, int UNUSED(re))
{
/* something went on in the quote channel */
	struct _bot_s *r = w->data;
	size_t npr = QBOF0;
	ssize_t nrd;

	nrd = recv(w->fd, r->buf + r->qbof, QBOFZ - r->qbof, 0);
	if (UNLIKELY(nrd <= 0)) {
		return;
	} else if (r->parent.qchan_cb == NULL) {
		goto reset;
	}
	/* up the bof */
	r->qbof += nrd;
	/* now snarf the line */
	for (const char *x;
	     (x = memchr(r->buf + npr, '\n', r->qbof - npr));
	     npr = x + 1U - r->buf) {
		qmsg_t m = recv_qmsg(r->buf + npr, x + 1U - (r->buf + npr));

		r->parent.qchan_cb((void*)r, m);
	}
	/* move left-overs */
	if (r->qbof > npr) {
		memmove(r->buf + QBOF0, r->buf + npr, r->qbof - npr);
		r->qbof -= npr;
	} else {
	reset:
		r->qbof = 0U;
	}
	r->qbof += QBOF0;
	return;
}


static struct ev_loop *loop;
static struct ipv6_mreq mreq;

bot_t
make_bot(const char *host)
{
	struct _bot_s *r;
	int s;
 

	/* initialise the main loop */
	loop = ev_default_loop(EVFLAG_AUTO);

	if (UNLIKELY((s = connector(host, TRADE_PORT)) < 0)) {
		serror("\
Error: cannot open socket for execution messages");
		goto nil;
	}

	/* get ourself a shiny new block of memory */
	r = malloc(BOTSIZ);
	memset(r, 0, offsetof(struct _bot_s, buf));
	r->mbof = MBOF0;
	r->qbof = QBOF0;
	r->obof = OBOF0;
	/* don't set up qch yet, see if they install a callback */
	r->qch->fd = -1;
	/* hook into our event loop */
	ev_io_init(r->och, _och_cb, s, EV_READ);
	ev_io_start(EV_A_ r->och);
	r->och->data = r;
	return (void*)r;

nil:
	return NULL;
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

int
bot_set_timer(bot_t b, double when, double intv)
{
	struct _bot_s *r = (void*)b;

	ev_timer_init(r->tim, _tim_cb, when, intv);
	r->tim->data = r;
	ev_timer_start(EV_A_ r->tim);
	return 0;
}

int
add_omsg(bot_t b, omsg_t msg)
{
	struct _bot_s *r = (void*)b;
	ssize_t nwr;

	/* message massage */
	switch (msg.typ) {
	case OMSG_BUY:
		msg.ord.sid = SIDE_LONG;
		break;
	case OMSG_SEL:
		msg.ord.sid = SIDE_SHORT;
		break;
	default:
		break;
	}

	/* try and serialise */
	nwr = send_omsg(r->buf + r->mbof, MBOFZ - r->mbof, msg);
	if (UNLIKELY(nwr <= 0)) {
		/* fuck */
		puts("FUCK");
		return -1;
	}
	printf("MBOF <- %zu\n", r->mbof + nwr);
	r->mbof += nwr;
	return 0;
}

int
bot_send(bot_t b)
{
	struct _bot_s *r = (void*)b;
	int rc;

	if (r->mbof <= MBOF0) {
		return 0;
	}
	rc = send(r->och->fd, r->buf + MBOF0, r->mbof - MBOF0, 0) > 0;
	r->mbof = MBOF0;
	return rc - 1;
}

/* lol.c ends here */
