#if !defined INCLUDED_sock_h_
#define INCLUDED_sock_h_
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#define UDP_MULTICAST_TTL	64


/* socket goodies */
extern int setsock_nonblock(int sock);

extern int setsock_linger(int s, int ltime);

extern int setsock_reuseaddr(int s);

extern int setsock_reuseport(int s);

extern int mc6_socket(void);

extern int
mc6_join_group(
	struct ipv6_mreq *tgt, int s,
	const char *addr, short unsigned int port, const char *iface);

extern void
mc6_leave_group(int s, struct ipv6_mreq *mreq);

extern int
mc6_set_pub(int s, const char *addr, short unsigned int port, const char *ifc);

/* tcp shit */
extern int listener(short unsigned int port);
extern int connector(const char *host, short unsigned int port);

#endif	/* INCLUDED_sock_h_ */
