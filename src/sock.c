#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "sock.h"
#include "nifty.h"

#define UDP_MULTICAST_TTL	64


/* socket goodies */
int
setsock_nonblock(int sock)
{
	int opts;

	/* get former options */
	opts = fcntl(sock, F_GETFL);
	if (opts < 0) {
		return -1;
	}
	opts |= O_NONBLOCK;
	return fcntl(sock, F_SETFL, opts);
}

int
setsock_linger(int s, int ltime)
{
#if defined SO_LINGER
	struct linger lng[1] = {{.l_onoff = 1, .l_linger = ltime}};
	return setsockopt(s, SOL_SOCKET, SO_LINGER, lng, sizeof(*lng));
#else  /* !SO_LINGER */
	return 0;
#endif	/* SO_LINGER */
}

int
setsock_reuseaddr(int s)
{
#if defined SO_REUSEADDR
	int yes = 1;
	return setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#else  /* !SO_REUSEADDR */
	return 0;
#endif	/* SO_REUSEADDR */
}

int
setsock_reuseport(int s)
{
	(void)s;
	return 0;
}

int
mc6_socket(void)
{
	volatile int s;

	/* try v6 first */
	if ((s = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IP)) < 0) {
		return -1;
	}

#if defined IPV6_V6ONLY
	{
		int yes = 1;
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
	}
#endif	/* IPV6_V6ONLY */
	/* be less blocking */
	setsock_nonblock(s);
	return s;
}

int
mc6_join_group(
	struct ipv6_mreq *tgt, int s,
	const char *addr, short unsigned int port, const char *iface)
{
	struct sockaddr_in6 sa = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
		.sin6_port = htons(port),
		.sin6_scope_id = 0,
	};
	struct ipv6_mreq r = {
		.ipv6mr_interface = 0,
	};
	int opt;

	/* allow many many many subscribers on that port */
	setsock_reuseaddr(s);

#if defined IPV6_USE_MIN_MTU
	/* use minimal mtu */
	opt = 1;
	setsockopt(s, IPPROTO_IPV6, IPV6_USE_MIN_MTU, &opt, sizeof(opt));
#endif
#if defined IPV6_DONTFRAG
	/* rather drop a packet than to fragment it */
	opt = 1;
	setsockopt(s, IPPROTO_IPV6, IPV6_DONTFRAG, &opt, sizeof(opt));
#endif
#if defined IPV6_RECVPATHMTU
	/* obtain path mtu to send maximum non-fragmented packet */
	opt = 1;
	setsockopt(s, IPPROTO_IPV6, IPV6_RECVPATHMTU, &opt, sizeof(opt));
#endif
#if defined IPV6_MULTICAST_HOPS
	opt = UDP_MULTICAST_TTL;
	/* turn into a mcast sock and set a TTL */
	setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &opt, sizeof(opt));
#endif	/* IPV6_MULTICAST_HOPS */

	if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
		return -1;
	}

	/* set up the membership request */
	if (inet_pton(AF_INET6, addr, &r.ipv6mr_multiaddr) < 0) {
		return -1;
	}
	if (iface != NULL) {
		r.ipv6mr_interface = if_nametoindex(iface);
	}

	/* now truly join */
	*tgt = r;
	return setsockopt(s, IPPROTO_IPV6, IPV6_JOIN_GROUP, &r, sizeof(r));
}

void
mc6_leave_group(int s, struct ipv6_mreq *mreq)
{
	/* drop mcast6 group membership */
	setsockopt(s, IPPROTO_IPV6, IPV6_LEAVE_GROUP, mreq, sizeof(*mreq));
	return;
}

int
mc6_set_pub(int s, const char *addr, short unsigned int port, const char *iface)
{
	struct sockaddr_in6 sa = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
		.sin6_port = htons(port),
		.sin6_flowinfo = 0,
		.sin6_scope_id = 0,
	};

	/* we pick link-local here for simplicity */
	if (inet_pton(AF_INET6, addr, &sa.sin6_addr) < 0) {
		return -1;
	}
	/* scope id */
	if (iface != NULL) {
		sa.sin6_scope_id = if_nametoindex(iface);
	}
	/* and do the connect() so we can use send() instead of sendto() */
	return connect(s, (struct sockaddr*)&sa, sizeof(sa));
}


/* tcp shit */
int
listener(short unsigned int port)
{
#if defined IPPROTO_IPV6
	struct sockaddr_in6 __sa6 = {
		.sin6_family = AF_INET6,
		.sin6_addr = IN6ADDR_ANY_INIT,
		.sin6_port = htons(port),
	};
	volatile int s;

	if (UNLIKELY((s = socket(PF_INET6, SOCK_STREAM, 0)) < 0)) {
		return s;
	}

	setsock_reuseaddr(s);
	setsock_reuseport(s);

	/* we used to retry upon failure, but who cares */
	if (bind(s, (struct sockaddr*)&__sa6, sizeof(__sa6)) < 0 ||
	    listen(s, 2) < 0) {
		close(s);
		return -1;
	}
	return s;

#else  /* !IPPROTO_IPV6 */
	return -1;
#endif	/* IPPROTO_IPV6 */
}

/* sock.c ends here */
