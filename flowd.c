/*
 * Copyright (c) 2004 Damien Miller <djm@mindrot.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <syslog.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <poll.h>

#include "sys-queue.h"
#include "sys-tree.h"
#include "flowd.h"
#include "privsep.h"
#include "netflow.h"
#include "store.h"
#include "atomicio.h"

RCSID("$Id$");

/* Dump unknown packet types */
/* #define DEBUG_UNKNOWN */

/*
 * Structure to hold per-peer state. NetFlow v.9 / IPFIX will require that we 
 * hold state for each peer to retain templates. This peer state is stored in
 * a splay tree for quick access by sender address and in a deque so we can
 * do fast LRU deletions on overflow
 */
struct peer_state {
	SPLAY_ENTRY(peer_state) tp;
	TAILQ_ENTRY(peer_state) lp;
	struct xaddr from;
	u_int64_t npackets, nflows, ninvalid;
	struct timeval firstseen, lastvalid;
	u_int last_version;
};

/* Structures for top of peer state tree and head of list */
SPLAY_HEAD(peer_tree, peer_state);
TAILQ_HEAD(peer_list, peer_state);

/* Peer stateholding structure */
struct peers {
	struct peer_tree peer_tree;
	struct peer_list peer_list;
	u_int max_peers, num_peers, num_forced;
};

/* Flags set by signal handlers */

static sig_atomic_t exit_flag = 0;
static sig_atomic_t reconf_flag = 0;
static sig_atomic_t reopen_flag = 0;
static sig_atomic_t info_flag = 0;

/* Signal handlers */
static void
sighand_exit(int signo)
{
	exit_flag = signo;
	signal(signo, sighand_exit);
}

static void
sighand_reconf(int signo)
{
	reconf_flag = 1;
	reopen_flag = 1;
	signal(signo, sighand_reconf);
}

static void
sighand_reopen(int signo)
{
	reopen_flag = 1;
	signal(signo, sighand_reopen);
}

static void
sighand_info(int signo)
{
	info_flag = 1;
	signal(signo, sighand_info);
}

/* Peer state housekeeping functions */
static int
peer_compare(struct peer_state *a, struct peer_state *b)
{
	return (addr_cmp(&a->from, &b->from));
}

/* Generate functions for peer state tree */
SPLAY_PROTOTYPE(peer_tree, peer_state, tp, peer_compare);
SPLAY_GENERATE(peer_tree, peer_state, tp, peer_compare);

static void
delete_peer(struct peers *peers, struct peer_state *peer)
{
	TAILQ_REMOVE(&peers->peer_list, peer, lp);
	SPLAY_REMOVE(peer_tree, &peers->peer_tree, peer);
	free(peer);
	peers->num_peers--;
}

static struct peer_state *
new_peer(struct peers *peers, struct xaddr *addr)
{
	struct peer_state *peer;

	/* If we have overflowed our peer table, then kick out the LRU peer */
	peers->num_peers++;
	if (peers->num_peers > peers->max_peers) {
		peers->num_forced++;
		peer = TAILQ_LAST(&peers->peer_list, peer_list);
		logit(LOG_WARNING, "forced deletion of peer %s", 
		    addr_ntop_buf(&peer->from));
		/* XXX ratelimit errors */
		delete_peer(peers, peer);
	}

	if ((peer = calloc(1, sizeof(*peer))) == NULL)
		logerrx("%s: calloc failed", __func__);
	memcpy(&peer->from, addr, sizeof(peer->from));

	logit(LOG_DEBUG, "new peer %s", addr_ntop_buf(addr));

	TAILQ_INSERT_HEAD(&peers->peer_list, peer, lp);
	SPLAY_INSERT(peer_tree, &peers->peer_tree, peer);
	gettimeofday(&peer->firstseen, NULL);
	
	return (peer);
}

#ifdef notyet
static void
flush_peers(struct peers *peers)
{
	struct peer_state *peer;

	while ((peer = TAILQ_FIRST(&peers->peer_list)) != NULL)
		delete_peer(peers, peer);
}
#endif

static void
update_peer(struct peers *peers, struct peer_state *peer, u_int nflows, 
    u_int netflow_version)
{
	/* Push peer to front of LRU queue, if it isn't there already */
	if (peer != TAILQ_FIRST(&peers->peer_list)) {
		TAILQ_REMOVE(&peers->peer_list, peer, lp);
		TAILQ_INSERT_HEAD(&peers->peer_list, peer, lp);
	}
	gettimeofday(&peer->lastvalid, NULL);
	peer->nflows += nflows;
	peer->npackets++;
	peer->last_version = netflow_version;
	logit(LOG_DEBUG, "update peer %s", addr_ntop_buf(&peer->from));
}

static struct peer_state *
find_peer(struct peers *peers, struct xaddr *addr)
{
	struct peer_state tmp, *peer;

	bzero(&tmp, sizeof(tmp));
	memcpy(&tmp.from, addr, sizeof(tmp.from));

	peer = SPLAY_FIND(peer_tree, &peers->peer_tree, &tmp);
	logit(LOG_DEBUG, "%s: found %s", __func__,
	    peer == NULL ? "NONE" : addr_ntop_buf(addr));

	return (peer);
}

static void
dump_peers(struct peers *peers)
{
	struct peer_state *peer;
	u_int i;

	logit(LOG_INFO, "Peer state: %u of %u in used, %u forced deletions",
	    peers->num_peers, peers->max_peers, peers->num_forced);
	i = 0;
	SPLAY_FOREACH(peer, peer_tree, &peers->peer_tree) {
		logit(LOG_INFO,
		    "peer %u - %s: %llu packets %llu flows %llu invalid",
		    i, addr_ntop_buf(&peer->from), 
		    peer->npackets, peer->nflows,
		    peer->ninvalid);
		logit(LOG_INFO, "peer %u - %s: first seen %s.%03u",
		    i, addr_ntop_buf(&peer->from), 
		    iso_time(peer->firstseen.tv_sec, 0), 
		    (u_int)(peer->firstseen.tv_usec / 1000));
		logit(LOG_INFO, "peer %u - %s: last valid %s.%03u netflow v.%u",
		    i, addr_ntop_buf(&peer->from), 
		    iso_time(peer->lastvalid.tv_sec, 0), 
		    (u_int)(peer->lastvalid.tv_usec / 1000), 
		    peer->last_version);
		i++;
	}
}

/* Display commandline usage information */
static void
usage(void)
{
	fprintf(stderr, "Usage: %s [options]\n", PROGNAME);
	fprintf(stderr, "This is %s version %s. Valid commandline options:\n",
	    PROGNAME, PROGVER);
	fprintf(stderr, "  -d              Don't daemonise\n");
	fprintf(stderr, "  -h              Display this help\n");
	fprintf(stderr, "  -f path         Configuration file (default: %s)\n",
	    DEFAULT_CONFIG);
	fprintf(stderr, "\n");
}

/* Dump a packet */
#ifdef DEBUG_UNKNOWN
static void
dump_packet(const u_int8_t *p, int len)
{
	char buf[1024], tmp[3];
	int i;

	for (*buf = '\0', i = 0; i < len; i++) {
		snprintf(tmp, sizeof(tmp), "%02x%s", p[i], i % 2 ? " " : "");
		if (strlcat(buf, tmp, sizeof(buf) - 4) >= sizeof(buf) - 4) {
			strlcat(buf, "...", sizeof(buf));
			break;
		}
	}
	logit(LOG_INFO, "packet len %d: %s", len, buf);
}
#endif

static int
start_log(int monitor_fd)
{
	int fd;
	off_t pos;
	const char *e;

	if ((fd = client_open_log(monitor_fd)) == -1)
		logerrx("Logfile open failed, exiting");

	/* Only write out the header if we are at the start of the file */
	switch ((pos = lseek(fd, 0, SEEK_END))) {
	case 0:
		/* New file, continue below */
		break;
	case -1:
		logerr("%s: llseek error, exiting", __func__);
	default:
		/* Logfile exists, don't write new header */
		if (lseek(fd, 0, SEEK_SET) != 0)
			logerr("%s: llseek error, exiting", __func__);
		if (store_check_header(fd, &e) != 0)
			logerrx("%s: Exiting on %s", __func__, e);
		if (lseek(fd, 0, SEEK_END) <= 0)
			logerr("%s: llseek error, exiting", __func__);
		logit(LOG_DEBUG, "Continuing with existing logfile len %lld", 
		    (long long)pos);
		return (fd);
	}

	logit(LOG_DEBUG, "Writing new logfile header");

	if (store_put_header(fd, &e) != 0)
		logerrx("%s: Exiting on %s", __func__, e);

	return (fd);
}

static void 
process_flow(struct store_flow_complete *flow, struct flowd_config *conf,
    int log_fd)
{
	const char *e;

	/* Another sanity check */
	if (flow->src_addr.af != flow->dst_addr.af) {
		logit(LOG_WARNING, "%s: flow src(%d)/dst(%d) AF mismatch",
		    __func__, flow->src_addr.af, flow->dst_addr.af);
		return;
	}

	/* Prepare for writing */
	flow->hdr.fields = htonl(flow->hdr.fields);

	flow->recv_time.recv_secs = htonl(flow->recv_time.recv_secs);

	if (conf->opts & FLOWD_OPT_VERBOSE) {
		char fbuf[1024];

		store_format_flow(flow, fbuf, sizeof(fbuf), 0,
		    STORE_DISPLAY_BRIEF);
		logit(LOG_DEBUG, "%s: flow %s", __func__, fbuf);
	}

	if (filter_flow(flow, &conf->filter_list) == FF_ACTION_DISCARD)
		return;

	if (store_put_flow(log_fd, flow, conf->store_mask, &e) != 0)
		logerrx("%s: exiting on %s", __func__, e);

	/* XXX reopen log file on one failure, exit on multiple */
}

static void 
process_netflow_v1(u_int8_t *pkt, size_t len, struct xaddr *flow_source,
    struct flowd_config *conf, struct peer_state *peer, struct peers *peers, 
    int log_fd)
{
	struct NF1_HEADER *nf1_hdr = (struct NF1_HEADER *)pkt;
	struct NF1_FLOW *nf1_flow;
	struct store_flow_complete flow;
	size_t offset;
	u_int i, nflows;

	if (len < sizeof(*nf1_hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.1 packet %d bytes from %s",
		    len, addr_ntop_buf(flow_source));
		return;
	}
	nflows = ntohs(nf1_hdr->c.flows);
	if (nflows == 0 || nflows > NF1_MAXFLOWS) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Invalid number of flows (%u) in netflow "
		    "v.1 packet from %s", nflows, addr_ntop_buf(flow_source));
		return;
	}
	if (len != NF1_PACKET_SIZE(nflows)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Inconsistent Netflow v.1 packet from %s: "
		    "len %u expected %u", addr_ntop_buf(flow_source), len,
		    NF1_PACKET_SIZE(nflows));
		return;
	}

	logit(LOG_DEBUG, "Valid netflow v.1 packet %d flows", nflows);
	update_peer(peers, peer, nflows, 1);

	for (i = 0; i < nflows; i++) {
		offset = NF1_PACKET_SIZE(i);
		nf1_flow = (struct NF1_FLOW *)(pkt + offset);

		bzero(&flow, sizeof(flow));

		/* NB. These are converted to network byte order later */
		flow.hdr.fields = STORE_FIELD_ALL;
		/* flow.hdr.tag is set later */
		flow.hdr.fields &= ~STORE_FIELD_TAG;
		flow.hdr.fields &= ~STORE_FIELD_SRC_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_DST_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_GATEWAY_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_AS_INFO;
		flow.hdr.fields &= ~STORE_FIELD_FLOW_ENGINE_INFO;

		flow.recv_time.recv_secs = time(NULL);

		flow.pft.tcp_flags = nf1_flow->tcp_flags;
		flow.pft.protocol = nf1_flow->protocol;
		flow.pft.tos = nf1_flow->tos;

		memcpy(&flow.agent_addr, flow_source, sizeof(flow.agent_addr));
		
		flow.src_addr.v4.s_addr = nf1_flow->src_ip;
		flow.src_addr.af = AF_INET;
		flow.dst_addr.v4.s_addr = nf1_flow->dest_ip;
		flow.dst_addr.af = AF_INET;
		flow.gateway_addr.v4.s_addr = nf1_flow->nexthop_ip;
		flow.gateway_addr.af = AF_INET;

		flow.ports.src_port = nf1_flow->src_port;
		flow.ports.dst_port = nf1_flow->dest_port;

#define NTO64(a) (store_htonll(ntohl(a)))
		flow.octets.flow_octets = NTO64(nf1_flow->flow_octets);
		flow.packets.flow_packets = NTO64(nf1_flow->flow_packets);
#undef NTO64

		flow.ifndx.if_index_in = nf1_flow->if_index_in;
		flow.ifndx.if_index_out = nf1_flow->if_index_out;

		flow.ainfo.sys_uptime_ms = nf1_hdr->uptime_ms;
		flow.ainfo.time_sec = nf1_hdr->time_sec;
		flow.ainfo.time_nanosec = nf1_hdr->time_nanosec;
		flow.ainfo.netflow_version = nf1_hdr->c.version;

		flow.ftimes.flow_start = nf1_flow->flow_start;
		flow.ftimes.flow_finish = nf1_flow->flow_finish;

		process_flow(&flow, conf, log_fd);
	}
}

static void 
process_netflow_v5(u_int8_t *pkt, size_t len, struct xaddr *flow_source,
    struct flowd_config *conf, struct peer_state *peer, struct peers *peers, 
    int log_fd)
{
	struct NF5_HEADER *nf5_hdr = (struct NF5_HEADER *)pkt;
	struct NF5_FLOW *nf5_flow;
	struct store_flow_complete flow;
	size_t offset;
	u_int i, nflows;

	if (len < sizeof(*nf5_hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.5 packet %d bytes from %s",
		    len, addr_ntop_buf(flow_source));
		return;
	}
	nflows = ntohs(nf5_hdr->c.flows);
	if (nflows == 0 || nflows > NF5_MAXFLOWS) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Invalid number of flows (%u) in netflow "
		    "v.5 packet from %s", nflows, addr_ntop_buf(flow_source));
		return;
	}
	if (len != NF5_PACKET_SIZE(nflows)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Inconsistent Netflow v.5 packet from %s: "
		    "len %u expected %u", addr_ntop_buf(flow_source), len,
		    NF5_PACKET_SIZE(nflows));
		return;
	}

	logit(LOG_DEBUG, "Valid netflow v.5 packet %d flows", nflows);
	update_peer(peers, peer, nflows, 5);

	for (i = 0; i < nflows; i++) {
		offset = NF5_PACKET_SIZE(i);
		nf5_flow = (struct NF5_FLOW *)(pkt + offset);

		bzero(&flow, sizeof(flow));

		/* NB. These are converted to network byte order later */
		flow.hdr.fields = STORE_FIELD_ALL;
		/* flow.hdr.tag is set later */
		flow.hdr.fields &= ~STORE_FIELD_TAG;
		flow.hdr.fields &= ~STORE_FIELD_SRC_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_DST_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_GATEWAY_ADDR6;

		flow.recv_time.recv_secs = time(NULL);

		flow.pft.tcp_flags = nf5_flow->tcp_flags;
		flow.pft.protocol = nf5_flow->protocol;
		flow.pft.tos = nf5_flow->tos;

		memcpy(&flow.agent_addr, flow_source, sizeof(flow.agent_addr));
		
		flow.src_addr.v4.s_addr = nf5_flow->src_ip;
		flow.src_addr.af = AF_INET;
		flow.dst_addr.v4.s_addr = nf5_flow->dest_ip;
		flow.dst_addr.af = AF_INET;
		flow.gateway_addr.v4.s_addr = nf5_flow->nexthop_ip;
		flow.gateway_addr.af = AF_INET;

		flow.ports.src_port = nf5_flow->src_port;
		flow.ports.dst_port = nf5_flow->dest_port;

#define NTO64(a) (store_htonll(ntohl(a)))
		flow.octets.flow_octets = NTO64(nf5_flow->flow_octets);
		flow.packets.flow_packets = NTO64(nf5_flow->flow_packets);
#undef NTO64

		flow.ifndx.if_index_in = nf5_flow->if_index_in;
		flow.ifndx.if_index_out = nf5_flow->if_index_out;

		flow.ainfo.sys_uptime_ms = nf5_hdr->uptime_ms;
		flow.ainfo.time_sec = nf5_hdr->time_sec;
		flow.ainfo.time_nanosec = nf5_hdr->time_nanosec;
		flow.ainfo.netflow_version = nf5_hdr->c.version;

		flow.ftimes.flow_start = nf5_flow->flow_start;
		flow.ftimes.flow_finish = nf5_flow->flow_finish;

		flow.asinf.src_as = nf5_flow->src_as;
		flow.asinf.dst_as = nf5_flow->dest_as;
		flow.asinf.src_mask = nf5_flow->src_mask;
		flow.asinf.dst_mask = nf5_flow->dst_mask;

		flow.finf.engine_type = nf5_hdr->engine_type;
		flow.finf.engine_id = nf5_hdr->engine_id;
		flow.finf.flow_sequence = nf5_hdr->flow_sequence;

		process_flow(&flow, conf, log_fd);
	}
}

static void 
process_netflow_v7(u_int8_t *pkt, size_t len, struct xaddr *flow_source,
    struct flowd_config *conf, struct peer_state *peer, struct peers *peers, 
    int log_fd)
{
	struct NF7_HEADER *nf7_hdr = (struct NF7_HEADER *)pkt;
	struct NF7_FLOW *nf7_flow;
	struct store_flow_complete flow;
	size_t offset;
	u_int i, nflows;

	if (len < sizeof(*nf7_hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short netflow v.7 packet %d bytes from %s",
		    len, addr_ntop_buf(flow_source));
		return;
	}
	nflows = ntohs(nf7_hdr->c.flows);
	if (nflows == 0 || nflows > NF7_MAXFLOWS) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Invalid number of flows (%u) in netflow "
		    "v.7 packet from %s", nflows, addr_ntop_buf(flow_source));
		return;
	}
	if (len != NF7_PACKET_SIZE(nflows)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "Inconsistent Netflow v.7 packet from %s: "
		    "len %u expected %u", addr_ntop_buf(flow_source), len,
		    NF7_PACKET_SIZE(nflows));
		return;
	}

	logit(LOG_DEBUG, "Valid netflow v.7 packet %d flows", nflows);
	update_peer(peers, peer, nflows, 7);

	for (i = 0; i < nflows; i++) {
		offset = NF7_PACKET_SIZE(i);
		nf7_flow = (struct NF7_FLOW *)(pkt + offset);

		bzero(&flow, sizeof(flow));

		/* NB. These are converted to network byte order later */
		flow.hdr.fields = STORE_FIELD_ALL;
		/* flow.hdr.tag is set later */
		flow.hdr.fields &= ~STORE_FIELD_TAG;
		flow.hdr.fields &= ~STORE_FIELD_SRC_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_DST_ADDR6;
		flow.hdr.fields &= ~STORE_FIELD_GATEWAY_ADDR6;

		/*
		 * XXX: we can parse the (undocumented) flags1 and flags2
		 * fields of the packet to disable flow fields not set by
		 * the Cat5k (e.g. destination-only mls nde mode)
		 */

		flow.recv_time.recv_secs = time(NULL);

		flow.pft.tcp_flags = nf7_flow->tcp_flags;
		flow.pft.protocol = nf7_flow->protocol;
		flow.pft.tos = nf7_flow->tos;

		memcpy(&flow.agent_addr, flow_source, sizeof(flow.agent_addr));
		
		flow.src_addr.v4.s_addr = nf7_flow->src_ip;
		flow.src_addr.af = AF_INET;
		flow.dst_addr.v4.s_addr = nf7_flow->dest_ip;
		flow.dst_addr.af = AF_INET;
		flow.gateway_addr.v4.s_addr = nf7_flow->nexthop_ip;
		flow.gateway_addr.af = AF_INET;

		flow.ports.src_port = nf7_flow->src_port;
		flow.ports.dst_port = nf7_flow->dest_port;

#define NTO64(a) (store_htonll(ntohl(a)))
		flow.octets.flow_octets = NTO64(nf7_flow->flow_octets);
		flow.packets.flow_packets = NTO64(nf7_flow->flow_packets);
#undef NTO64

		flow.ifndx.if_index_in = nf7_flow->if_index_in;
		flow.ifndx.if_index_out = nf7_flow->if_index_out;

		flow.ainfo.sys_uptime_ms = nf7_hdr->uptime_ms;
		flow.ainfo.time_sec = nf7_hdr->time_sec;
		flow.ainfo.time_nanosec = nf7_hdr->time_nanosec;
		flow.ainfo.netflow_version = nf7_hdr->c.version;

		flow.ftimes.flow_start = nf7_flow->flow_start;
		flow.ftimes.flow_finish = nf7_flow->flow_finish;

		flow.asinf.src_as = nf7_flow->src_as;
		flow.asinf.dst_as = nf7_flow->dest_as;
		flow.asinf.src_mask = nf7_flow->src_mask;
		flow.asinf.dst_mask = nf7_flow->dst_mask;

		flow.finf.flow_sequence = nf7_hdr->flow_sequence;

		process_flow(&flow, conf, log_fd);
	}
}

static void
process_input(struct flowd_config *conf, struct peers *peers, 
    int net_fd, int log_fd)
{
	struct sockaddr_storage from;
	struct peer_state *peer;
	socklen_t fromlen;
	u_int8_t buf[2048];
	ssize_t len;
	struct NF_HEADER_COMMON *hdr;
	struct xaddr flow_source;

 retry:
	fromlen = sizeof(from);
	if ((len = recvfrom(net_fd, buf, sizeof(buf), 0, 
	    (struct sockaddr *)&from, &fromlen)) < 0) {
		if (errno == EINTR)
			goto retry;
		if (errno != EAGAIN) {
			logit(LOG_WARNING, "recvfrom(fd = %d)", net_fd);
		}
		/* XXX ratelimit errors */
		return;
	}
	if (addr_sa_to_xaddr((struct sockaddr *)&from, fromlen,
	    &flow_source) == -1) {
		logit(LOG_WARNING, "Invalid agent address");
		return;
	}

	if ((peer = find_peer(peers, &flow_source)) == NULL)
		peer = new_peer(peers, &flow_source);

	if ((size_t)len < sizeof(*hdr)) {
		peer->ninvalid++;
		logit(LOG_WARNING, "short packet %d bytes from %s", len,
		    addr_ntop_buf(&flow_source));
		return;
	}

	hdr = (struct NF_HEADER_COMMON *)buf;
	switch (ntohs(hdr->version)) {
	case 1:
		process_netflow_v1(buf, len, &flow_source, conf, peer, 
		    peers, log_fd);
		break;
	case 5:
		process_netflow_v5(buf, len, &flow_source, conf, peer, 
		    peers, log_fd);
		break;
	case 7:
		process_netflow_v7(buf, len, &flow_source, conf, peer, 
		    peers, log_fd);
		break;
	default:
		logit(LOG_INFO, "Unsupported netflow version %u from %s",
		    ntohs(hdr->version), addr_ntop_buf(&flow_source));
#ifdef DEBUG_UNKNOWN
		dump_packet(buf, len);
#endif
		return;
	}
}

static void
init_pfd(struct flowd_config *conf, struct pollfd **pfdp, int mfd, int *num_fds)
{
	struct pollfd *pfd = *pfdp;
	struct listen_addr *la;
	int i;

	logit(LOG_DEBUG, "%s: entering (num_fds = %d)", __func__, *num_fds);

	if (pfd != NULL)
		free(pfd);

	*num_fds = 1; /* fd to monitor */

	/* Count socks */
	TAILQ_FOREACH(la, &conf->listen_addrs, entry)
		(*num_fds)++;

	if ((pfd = calloc((*num_fds) + 1, sizeof(*pfd))) == NULL) {
		logerrx("%s: calloc failed (num %d)",
		    __func__, *num_fds + 1);
	}

	pfd[0].fd = mfd;
	pfd[0].events = POLLIN;

	i = 1;
	TAILQ_FOREACH(la, &conf->listen_addrs, entry) {
		pfd[i].fd = la->fd;
		pfd[i].events = POLLIN;
		i++;
	}

	*pfdp = pfd;

	logit(LOG_DEBUG, "%s: done (num_fds = %d)", __func__, *num_fds);
}

static void
flowd_mainloop(struct flowd_config *conf, struct peers *peers, int monitor_fd)
{
	int i, log_fd, num_fds = 0;
	struct listen_addr *la;
	struct pollfd *pfd = NULL;

	init_pfd(conf, &pfd, monitor_fd, &num_fds);

	/* Main loop */
	log_fd = -1;
	for(;exit_flag == 0;) {
		if (reopen_flag && log_fd != -1) {
			logit(LOG_INFO, "log reopen requested");
			close(log_fd);
			log_fd = -1;
			reopen_flag = 0;
		}
		if (reconf_flag) {
			logit(LOG_INFO, "reconfiguration requested");
			if (client_reconfigure(monitor_fd, conf) == -1)
				logerrx("reconfigure failed, exiting");
			init_pfd(conf, &pfd, monitor_fd, &num_fds);
			reconf_flag = 0;
		}
		if (log_fd == -1)
			log_fd = start_log(monitor_fd);

		if (info_flag) {
			struct filter_rule *fr;

			info_flag = 0;
			TAILQ_FOREACH(fr, &conf->filter_list, entry)
				logit(LOG_INFO, "%s", format_rule(fr));
			dump_peers(peers);
		}
		
		i = poll(pfd, num_fds, INFTIM);
		if (i <= 0) {
			if (i == 0 || errno == EINTR)
				continue;
			logerr("%s: poll", __func__);
		}

		/* monitor exited */
		if (pfd[0].revents != 0) {
			logit(LOG_DEBUG, "%s: monitor closed", __func__);
			break;
		}

		i = 1;
		TAILQ_FOREACH(la, &conf->listen_addrs, entry) {
			if ((pfd[i].revents & POLLIN) != 0)
				process_input(conf, peers, pfd[i].fd, log_fd);
			i++;
		}
	}

	if (exit_flag != 0)
		logit(LOG_NOTICE, "Exiting on signal %d", exit_flag);
}

static void
startup_listen_init(struct flowd_config *conf)
{
	struct listen_addr *la;

	TAILQ_FOREACH(la, &conf->listen_addrs, entry) {
		if ((la->fd = open_listener(&la->addr, la->port)) == -1) {
			logerrx("Listener setup of [%s]:%d failed", 
			    addr_ntop_buf(&la->addr), la->port);
		}
	}
}

int
main(int argc, char **argv)
{
	int ch;
	extern char *optarg;
	extern int optind;
	const char *config_file = DEFAULT_CONFIG;
	struct flowd_config conf;
	int monitor_fd;
	struct peers peers;

#ifndef HAVE_SETPROCTITLE
	compat_init_setproctitle(argc, &argv);
#endif
	umask(0077);
	closefrom(STDERR_FILENO + 1);

#ifdef HAVE_TZSET
	tzset();
#endif
	loginit(PROGNAME, 1, 0);

	bzero(&conf, sizeof(conf));
	bzero(&peers, sizeof(peers));
	peers.max_peers = DEFAULT_MAX_PEERS;
	SPLAY_INIT(&peers.peer_tree);
	TAILQ_INIT(&peers.peer_list);

	while ((ch = getopt(argc, argv, "dhD:f:")) != -1) {
		switch (ch) {
		case 'd':
			conf.opts |= FLOWD_OPT_DONT_FORK;
			conf.opts |= FLOWD_OPT_VERBOSE;
			loginit(PROGNAME, 1, 1);
			break;
		case 'h':
			usage();
			return (0);
		case 'D':
			if (cmdline_symset(optarg) < 0)
				logerrx("Could not parse macro "
				    "definition %s", optarg);
			break;
		case 'f':
			config_file = optarg;
			break;
		default:
			fprintf(stderr, "Invalid commandline option.\n");
			usage();
			exit(1);
		}
	}

	if (read_config(config_file, &conf) == -1)
		logerrx("Config file has errors");

	/* Start listening (do this early to report errors before privsep) */
	startup_listen_init(&conf);

	/* Start the monitor - we continue as the unprivileged child */
	privsep_init(&conf, &monitor_fd, config_file);

	signal(SIGINT, sighand_exit);
	signal(SIGTERM, sighand_exit);
	signal(SIGHUP, sighand_reconf);
	signal(SIGUSR1, sighand_reopen);
	signal(SIGUSR2, sighand_info);
#ifdef SIGINFO
	signal(SIGINFO, sighand_info);
#endif

	flowd_mainloop(&conf, &peers, monitor_fd);

	return (0);
}
