#if !defined INCLUDED_bot_h_
#define INCLUDED_bot_h_
#include "lol.h"

typedef struct bot_s *bot_t;

struct bot_s {
	void *user;
	void(*timer_cb)(bot_t);
	void(*ochan_cb)(bot_t, omsg_t);
	void(*qchan_cb)(bot_t, qmsg_t);
};


extern bot_t make_bot(const char *host);
extern void kill_bot(bot_t);

extern int run_bots(bot_t, ...);

extern int
bot_set_timer(bot_t, double when, double intv);

#endif	/* INCLUDED_bot_h_ */
