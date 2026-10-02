#include "netlink.h"
#include "libretro.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define NETLINK_MAGIC    0x4E504C4BU /* 'NPLK' */
/* 10: CMD_STATE carries a transfer kind and completed transfers are queued
 *     rather than overwritten; CMD_SESSION_IDENTITY carries rom_size;
 *     CMD_LINK_VERDICT added.
 * 11: CMD_RTT_PROBE/CMD_RTT_ECHO added; input delay is negotiated from the
 *     measured round trip above the session's transport-specific floor. */
#define NETLINK_PROTOCOL 14

#define QUEUE_SIZE    512
/* Inputs are indexed by frame; the ring only has to outlast the input delay
 * plus any burst of catch-up packets. */
#define INPUT_RING    256
#define HASH_RING       8
#define STATE_CHUNK   1024
/* {total, offset, kind} ahead of every chunk's bytes. */
#define STATE_HEADER    12
/* Completed state transfers waiting for the emulator thread. One transfer is
 * reassembled at a time, but a finished one is queued rather than overwritten:
 * the instanced-link bootstrap sends two payloads back to back (peer save
 * memory, then the paired checkpoint) while the consumer reads at most one per
 * frontend frame. Four is far more than any exchange needs and bounds the
 * memory a peer can pin here. */
#define STATE_QUEUE     4
/* Stop reading the socket above this; TCP holds the rest for us. */
#define QUEUE_HIGH_WATER (QUEUE_SIZE - 64)
#define HEARTBEAT_MS  1000
#define TIMEOUT_MS    5000
#define ACCEPT_POLL_MS 100
/* Long enough for a peer that is present, short enough that shutdown does not
 * wait on a peer that is not. */
#define CONNECT_TIMEOUT_MS 2000
/* Long enough to sleep properly, short enough to keep the heartbeat on time. */
#define POLL_WAIT_MS   200
/* Long enough not to trip on a frame hitch, short enough that a menu does not
 * overrun the peer's queue first. A frame is ~16ms. */
#define STALL_MS       250
/* A peer can pause indefinitely and legitimately - a long menu. But if it stops
 * responding without closing the socket, resuming beats hanging forever. */
#define MAX_PAUSE_MS   30000

enum {
	CMD_HELLO = 0x00,
	CMD_DATA  = 0x01,
	CMD_PING  = 0x02,
	CMD_PAUSE = 0x03, /* our frontend stalled; hold off */
	CMD_RESUME = 0x04,
	CMD_INPUT  = 0x05, /* {frame, buttons} */
	CMD_STATE  = 0x06, /* {total, offset, kind, bytes} */
	CMD_HASH   = 0x07, /* {frame, hash} */
	CMD_RESYNC_REQUEST = 0x08, /* {mismatched frame} */
	CMD_RESYNC_BEGIN   = 0x09, /* {epoch, resume_frame, kind}; CMD_STATE follows */
	CMD_RESYNC_ACK     = 0x0A, /* {epoch, loaded} */
	CMD_RESYNC_COMMIT  = 0x0B, /* {epoch} */
	CMD_SESSION_IDENTITY = 0x0C, /* {rom sha256, mode, delay, core/state/save identity, wall clock} */
	CMD_CHECKPOINT_ACK = 0x0D, /* {frame, hash, matched} */
	CMD_LINK_VERDICT   = 0x0E, /* {can_pair, reason} - instanced-link agreement */
	CMD_RTT_PROBE      = 0x0F, /* {token} - echoed straight back as CMD_RTT_ECHO */
	CMD_RTT_ECHO       = 0x10, /* {token} */
};

/* Round-trip probes, for sizing input delay from what the link actually does
 * rather than from a compile-time guess.
 *
 * A separate message rather than timing CMD_PING: that heartbeat is only sent
 * when the link is otherwise idle (`ms_since(&NL_PRIMARY->last_tx) > HEARTBEAT_MS`), so
 * during a session carrying per-frame input it never fires, and nothing echoes
 * it in any case. Probes are cheap - 5 header bytes and 4 payload, a few times
 * a second - and are what the delay negotiation is derived from. */
#define RTT_PROBE_MS   100
#define RTT_SAMPLES    16

typedef struct __attribute__((packed)) {
	uint8_t  cmd;
	uint16_t size;      /* network order */
	uint16_t client_id; /* network order */
} NetLinkHeader;

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t protocol;
	/* The host fills this with the slot it gave the guest, so a guest knows which
	 * console it is playing as before its core starts. A guest's own greeting
	 * carries 0: its slot is the host's to assign, by arrival order. */
	uint16_t client_id;
} NetLinkHello;

typedef struct {
	uint8_t  data[NETLINK_MAX_PACKET];
	size_t   len;
	/* Which console sent it: the slot of the peer it arrived from, or our own
	 * slot for a packet our own core sent. Four-player link play needs this -
	 * every console sees every packet, and the core tells them apart by id. */
	uint16_t client_id;
} QueuedPacket;

/* One connected peer.
 *
 * The transport used to have exactly one - a single fd, a single queue, and one
 * peer's worth of session state, all in nl below. The emulated GBA multi-play link
 * is four slots (SIOCNT carries the slot, and gpSP's Advance Wars protocol is
 * written for slots 0..3), so the per-connection state lives here and nl keeps only
 * what is true of the link as a whole. Slot 0 is the host; 1..3 are guests, in the
 * order they were accepted.
 *
 * This is stage one of that change and deliberately a pure refactor: the machinery
 * below still names one peer, through NL_PRIMARY, so behaviour is byte-identical
 * with a single connection and shim/test/link.sh proves it. The table is what the
 * per-guest routing and the relay are built on next. */
typedef struct {

	int      fd;
	/* This peer's own slot: 0 for the host, 1..3 for a host's guests. */
	uint16_t client_id;
	bool     connected;


	QueuedPacket queue[QUEUE_SIZE];
	unsigned     q_head, q_tail;
	unsigned     dropped;
	unsigned     rx_count;

	bool backpressure;

	/* netplay */
	struct {
		uint32_t frame;
		uint32_t buttons;
		bool     valid;
	} inputs[INPUT_RING];

	/* In-progress reassembly. */
	uint8_t* state_buf;
	size_t   state_len;
	size_t   state_have;
	uint32_t state_kind;

	/* Completed transfers, oldest first. */
	struct {
		uint8_t* buf;
		size_t   len;
		uint32_t kind;
	}        state_done[STATE_QUEUE];
	unsigned state_done_head, state_done_count;

	struct {
		uint32_t frame;
		uint32_t hash;
	} peer_hashes[HASH_RING];
	unsigned peer_hash_head;
	unsigned peer_hash_count;

	uint32_t resync_request_frame;
	bool     resync_request_ready;
	uint32_t resync_begin_epoch;
	uint32_t resync_begin_frame;
	NetLinkRecoveryKind resync_begin_kind;
	bool     resync_begin_ready;
	uint32_t resync_ack_epoch;
	bool     resync_ack_loaded;
	bool     resync_ack_ready;
	uint32_t resync_commit_epoch;
	bool     resync_commit_ready;

	/* Recent round trips. Kept as a small ring so the median can describe the
	 * normal link while the maximum sizes the jitter-tolerance window. */
	uint32_t rtt_us[RTT_SAMPLES];
	unsigned rtt_count, rtt_next;
	uint32_t rtt_token;
	struct timeval rtt_sent_at;
	bool     rtt_outstanding;
	struct timeval rtt_last_probe;

	struct timeval peer_paused_at;
	bool           told_peer_paused;
	bool           peer_paused;

	bool connect_event;
	bool disconnect_event;
	uint32_t connection_generation;

	NetLinkSessionIdentity peer_session_identity;
	bool peer_session_identity_ready;
	uint32_t link_verdict_reason;
	bool link_verdict_can_pair;
	bool link_verdict_ready;
	uint32_t checkpoint_ack_frame;
	uint32_t checkpoint_ack_hash;
	bool checkpoint_ack_matched;
	bool checkpoint_ack_ready;

	struct timeval last_rx;
	struct timeval last_tx;
} NetLinkPeer;

static struct {
	NetLinkRole role;
	uint16_t    port;
	char        peer[64];
	bool        configured;
	int  listen_fd;
	bool running;
	pthread_t       thread;
	pthread_mutex_t lock;
	struct timeval last_run;      /* last retro_run, per NetLink_markFrame */
	bool           core_running;

	/* The peers. peers[0] is the primary: the connection the session machinery
	 * below talks to, and the only one it uses while shared-screen play is 1:1. */
	NetLinkPeer    peers[NETLINK_MAX_PEERS];
	unsigned       primary;
	/* Our own slot: 0 on the host, assigned by the host on a guest. */
	uint16_t       local_client_id;
} nl;
#define NL_PRIMARY (&nl.peers[nl.primary])

/* Every line carries milliseconds since the link started. Without it a log
 * shows only ordering, which is not enough to tell a prompt failure from one
 * that happened half a minute in. */
static long ms_since(const struct timeval* then) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - then->tv_sec) * 1000L + (now.tv_usec - then->tv_usec) / 1000L;
}

static struct timeval nl_log_epoch;

static void nl_log(const char* fmt, ...) {
	if (!nl_log_epoch.tv_sec) gettimeofday(&nl_log_epoch, NULL);

	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[netlink +%ldms] ", ms_since(&nl_log_epoch));
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}


static bool write_all(int fd, const void* buf, size_t len) {
	const uint8_t* p = buf;
	struct timeval start;
	gettimeofday(&start, NULL);
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n > 0) { p += n; len -= (size_t)n; continue; }
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (ms_since(&start) > TIMEOUT_MS) return false;
			usleep(1000);
			continue;
		}
		return false;
	}
	return true;
}

/* Blocking read of exactly len bytes, with a deadline so a half-open peer
 * cannot wedge the worker thread. */
static bool read_all_why(int fd, void* buf, size_t len, int timeout_ms, const char** why) {
	uint8_t* p = buf;
	struct timeval start;
	gettimeofday(&start, NULL);

	while (len) {
		ssize_t n = recv(fd, p, len, 0);
		if (n > 0) { p += n; len -= (size_t)n; continue; }
		if (n == 0) { if (why) *why = "peer closed the socket"; return false; }
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (timeout_ms >= 0 && ms_since(&start) > timeout_ms) {
				if (why) *why = "read timed out mid-frame";
				return false;
			}
			usleep(1000);
			continue;
		}
		if (why) *why = strerror(errno);
		return false;
	}
	return true;
}

static bool read_all(int fd, void* buf, size_t len, int timeout_ms) {
	return read_all_why(fd, buf, len, timeout_ms, NULL);
}

static void configure_socket(int fd) {
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

	/* Non-blocking so reads can time out and notice a dead peer, which is what
	 * turns into the core's disconnect callback. */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Caller must hold the lock. */
static bool send_framed(NetLinkPeer* p, uint8_t cmd, const void* data, size_t len, uint16_t client_id) {
	if (p->fd < 0) return false;

	NetLinkHeader hdr = {
		.cmd       = cmd,
		.size      = htons((uint16_t)len),
		.client_id = htons(client_id),
	};

	if (!write_all(p->fd, &hdr, sizeof(hdr))) return false;
	if (len && data && !write_all(p->fd, data, len)) return false;

	gettimeofday(&p->last_tx, NULL);
	return true;
}

//////////////////////////////////////////////////////////////////////////////
// configuration
//////////////////////////////////////////////////////////////////////////////

bool NetLink_configure(const char* session_path) {
	memset(&nl, 0, sizeof(nl));
	for (unsigned i = 0; i < NETLINK_MAX_PEERS; i++) nl.peers[i].fd = -1;
	nl.listen_fd = -1;
	/* Which slot the session machinery talks through. A guest has exactly one
	 * connection and it is to the host, whose slot is 0; a host talks to its
	 * first guest, slot 1. Both are the same peer the single-connection
	 * transport used to keep in nl, so everything below is unchanged. */
	nl.primary = 0;
	nl.port = NETLINK_DEFAULT_PORT;
	pthread_mutex_init(&nl.lock, NULL);

	if (!session_path || !session_path[0]) return false;

	FILE* f = fopen(session_path, "r");
	if (!f) {
		nl_log("no session at %s\n", session_path);
		return false;
	}

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* nlch = strpbrk(line, "\r\n");
		if (nlch) *nlch = '\0';
		if (!line[0] || line[0] == '#') continue;

		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		const char* key = line;
		const char* val = eq + 1;

		if (!strcmp(key, "role")) {
			if (!strcmp(val, "host"))        nl.role = NETLINK_ROLE_HOST;
			else if (!strcmp(val, "client")) nl.role = NETLINK_ROLE_CLIENT;
		} else if (!strcmp(key, "port")) {
			int p = atoi(val);
			if (p > 0 && p < 65536) nl.port = (uint16_t)p;
		} else if (!strcmp(key, "peer")) {
			snprintf(nl.peer, sizeof(nl.peer), "%s", val);
		}
	}
	fclose(f);

	if (nl.role == NETLINK_ROLE_NONE) {
		nl_log("session has no usable role\n");
		return false;
	}
	if (nl.role == NETLINK_ROLE_CLIENT && !nl.peer[0]) {
		nl_log("client session has no peer address\n");
		return false;
	}

	nl.configured = true;
	/* Both roles have exactly one slot the session machinery talks through: a guest
	 * talks to the host (slot 0), a host talks to its first guest (slot 1). That is
	 * the peer the single-connection transport used to keep in nl. */
	nl.primary = (nl.role == NETLINK_ROLE_HOST) ? 1 : 0;
	nl_log("session: role=%s port=%u%s%s\n",
	       nl.role == NETLINK_ROLE_HOST ? "host" : "client",
	       nl.port, nl.peer[0] ? " peer=" : "", nl.peer[0] ? nl.peer : "");
	return true;
}

bool NetLink_isConfigured(void)  { return nl.configured; }
NetLinkRole NetLink_getRole(void) { return nl.role; }

uint16_t NetLink_localClientId(void) {
	/* The host is slot 0. A guest's slot is assigned by the host in the greeting,
	 * so until that arrives the answer is 1 - which is what a two-device session
	 * has always used, and what the session machinery below assumes. */
	if (nl.role == NETLINK_ROLE_HOST) return 0;
	return nl.local_client_id ? nl.local_client_id : 1;
}
uint16_t NetLink_remoteClientId(void) {
	return nl.role == NETLINK_ROLE_HOST ? 1 : 0;
}

//////////////////////////////////////////////////////////////////////////////
// connection setup, on the worker thread
//////////////////////////////////////////////////////////////////////////////

static bool exchange_hello(int fd, uint16_t assign_client_id) {
	NetLinkHello mine = {
		.magic = htonl(NETLINK_MAGIC), .protocol = htonl(NETLINK_PROTOCOL),
		.client_id = htons(assign_client_id),
	};
	NetLinkHeader hdr = { .cmd = CMD_HELLO, .size = htons(sizeof(mine)), .client_id = 0 };

	if (!write_all(fd, &hdr, sizeof(hdr)) || !write_all(fd, &mine, sizeof(mine))) return false;

	NetLinkHeader rhdr;
	if (!read_all(fd, &rhdr, sizeof(rhdr), TIMEOUT_MS)) return false;
	if (rhdr.cmd != CMD_HELLO || ntohs(rhdr.size) != sizeof(NetLinkHello)) {
		nl_log("peer sent an unexpected greeting\n");
		return false;
	}

	NetLinkHello theirs;
	if (!read_all(fd, &theirs, sizeof(theirs), TIMEOUT_MS)) return false;
	if (ntohl(theirs.magic) != NETLINK_MAGIC) {
		nl_log("peer is not speaking netlink\n");
		return false;
	}
	if (ntohl(theirs.protocol) != NETLINK_PROTOCOL) {
		nl_log("protocol mismatch: peer=%u ours=%u\n", ntohl(theirs.protocol), NETLINK_PROTOCOL);
		return false;
	}

	/* A guest plays as the console the host assigned it. The host decided that
	 * slot when it accepted the connection - it knows which of its slots is free
	 * - so this is the guest's only way to learn it, and it must arrive before
	 * anything of the guest's is attributed to a console number. The host reads
	 * the field and ignores it: what a guest asks for is not what it gets. */
	if (nl.role != NETLINK_ROLE_HOST) {
		uint16_t slot = ntohs(theirs.client_id);
		if (slot < 1 || slot >= NETLINK_MAX_PEERS) {
			nl_log("host offered no slot (%u)\n", slot);
			return false;
		}
		nl.local_client_id = slot;
		nl_log("host assigned slot %u\n", slot);
	}

	return true;
}

static int do_listen_accept(void) {
	if (nl.listen_fd < 0) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) return -1;

		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = INADDR_ANY;
		addr.sin_port = htons(nl.port);

		if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 1) < 0) {
			nl_log("listen on %u failed: %s\n", nl.port, strerror(errno));
			close(fd);
			return -1;
		}

		int flags = fcntl(fd, F_GETFL, 0);
		if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

		nl.listen_fd = fd;
		nl_log("hosting on port %u\n", nl.port);
	}

	int fd = accept(nl.listen_fd, NULL, NULL);
	if (fd < 0) return -1;
	return fd;
}

static int do_connect(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(nl.port);
	if (inet_pton(AF_INET, nl.peer, &addr.sin_addr) != 1) {
		nl_log("bad peer address '%s'\n", nl.peer);
		close(fd);
		return -1;
	}

	/* Non-blocking, with a deadline.
	 *
	 * A blocking connect() to a peer whose network has gone away does not fail
	 * for the kernel's whole SYN timeout - about two minutes. Setting
	 * nl.running = false does not interrupt a syscall, so NetLink_stop's
	 * pthread_join sat behind it, retro_deinit could not return, and minarch
	 * could not exit: quitting the game left a black screen that only cleared
	 * when something else changed the routing state and the connect finally
	 * failed. Everything else in this file already has a deadline; this was the
	 * one place that did not. */
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
	if (errno != EINPROGRESS) { close(fd); return -1; }

	struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
	if (poll(&pfd, 1, CONNECT_TIMEOUT_MS) <= 0) { close(fd); return -1; }

	int err = 0;
	socklen_t elen = sizeof(err);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err) {
		close(fd);
		return -1;
	}
	return fd;
}

//////////////////////////////////////////////////////////////////////////////
// worker thread
//////////////////////////////////////////////////////////////////////////////

/* from is the console the packet is attributed to, which is the sender's slot as
 * carried in the frame - not the peer it happened to arrive from. They differ
 * when the host relays: a guest hears another guest's packet through the host,
 * and it must still be the other guest's console number. */
static void queue_push(NetLinkPeer* p, const uint8_t* data, size_t len, uint16_t from) {
	unsigned next = (p->q_tail + 1) % QUEUE_SIZE;
	if (next == p->q_head) {
		/* Full. Dropping is better than blocking the reader, but it means the
		 * core missed data, so make it visible. */
		p->dropped++;
		return;
	}
	memcpy(p->queue[p->q_tail].data, data, len);
	p->queue[p->q_tail].len = len;
	p->queue[p->q_tail].client_id = from;
	p->q_tail = next;
}

/* Hand a finished transfer to the emulator thread. Takes ownership of buf on
 * success. Caller holds nl.lock. */
static bool push_state_locked(NetLinkPeer* p, uint8_t* buf, size_t len, uint32_t kind) {
	if (p->state_done_count >= STATE_QUEUE) return false;
	unsigned slot = (p->state_done_head + p->state_done_count) % STATE_QUEUE;
	p->state_done[slot].buf = buf;
	p->state_done[slot].len = len;
	p->state_done[slot].kind = kind;
	p->state_done_count++;
	return true;
}

static void clear_states_locked(NetLinkPeer* p) {
	while (p->state_done_count) {
		free(p->state_done[p->state_done_head].buf);
		p->state_done[p->state_done_head].buf = NULL;
		p->state_done_head = (p->state_done_head + 1) % STATE_QUEUE;
		p->state_done_count--;
	}
	free(p->state_buf);
	p->state_buf = NULL;
	p->state_len = p->state_have = 0;
	p->state_kind = 0;
}

// Always say why. "peer lost" alone is not diagnosable after the fact, and the
// interesting failures here are all distinguishable at the point of detection.
static void drop_connection(NetLinkPeer* p, const char* reason) {
	pthread_mutex_lock(&nl.lock);
	if (p->fd >= 0) { close(p->fd); p->fd = -1; }
	if (p->connected) {
		p->connected = false;
		p->disconnect_event = true;
		/* Whatever the peer's last state was, it is gone now. Leaving this set
		 * strands us waiting for a CMD_RESUME that can never arrive. */
		p->peer_paused = false;
		p->told_peer_paused = false;
		p->rtt_count = p->rtt_next = 0;
		p->rtt_outstanding = false;
		/* Everything the departed peer told us dies with it, so a reconnecting
		 * process cannot be accepted on the strength of its predecessor's
		 * handshake. This runs on the worker thread, ahead of any byte of the
		 * next connection. */
		clear_states_locked(p);
		p->peer_session_identity_ready = false;
		p->link_verdict_ready = false;
		p->resync_request_ready = p->resync_begin_ready = false;
		p->resync_ack_ready = p->resync_commit_ready = false;
		nl_log("peer lost: %s (rx %ldms ago, tx %ldms ago, %u pkts in, %u dropped)\n",
		       reason, ms_since(&p->last_rx), ms_since(&p->last_tx),
		       p->rx_count, p->dropped);
	}
	pthread_mutex_unlock(&nl.lock);
}

/* How many guests this session allows. One unless the shim raises it: the
 * emulated GBA multi-play link is four slots, but shared-screen play is a single
 * partner by design (docs/features.md section 11), and only the shim knows which
 * mode the core chose. */
static unsigned nl_max_guests = 1;

void NetLink_setMaxGuests(unsigned n) {
	if (n >= 1 && n <= NETLINK_MAX_PEERS - 1) nl_max_guests = n;
}

static void service_peer(NetLinkPeer* p, short revents);

/* Take a connection into a slot and note that it is live. The slot number is the
 * peer's client id: 0 for the host (a guest's only connection), 1..3 for the
 * guests a host has accepted, in the order they arrived. */
static void peer_attach(NetLinkPeer* p, int fd) {
	pthread_mutex_lock(&nl.lock);
	p->fd = fd;
	p->connected = true;
	p->connect_event = true;
	p->connection_generation++;
	if (!p->connection_generation) p->connection_generation++;
	p->peer_session_identity_ready = false;
	p->checkpoint_ack_ready = false;
	gettimeofday(&p->last_rx, NULL);
	gettimeofday(&p->last_tx, NULL);
	pthread_mutex_unlock(&nl.lock);
	nl_log("peer %u connected (%s)\n", p->client_id,
	       nl.role == NETLINK_ROLE_HOST ? "guest" : "host");
}

static void* worker(void* arg) {
	(void)arg;

	while (nl.running) {
		/* The host keeps its listener and fills free slots; a guest has exactly one
		 * connection to make, to the host. */
		if (nl.role == NETLINK_ROLE_HOST) {
			for (unsigned slot = 1; slot <= nl_max_guests; slot++) {
				NetLinkPeer* p = &nl.peers[slot];
				if (p->connected) continue;
				int fd = do_listen_accept();
				if (fd < 0) break;               /* nothing waiting - normal */
				configure_socket(fd);
				/* The lowest free slot is the id this guest plays as. */
				if (!exchange_hello(fd, (uint16_t)slot)) { close(fd); continue; }
				p->client_id = (uint16_t)slot;
				peer_attach(p, fd);
			}
		} else if (!nl.peers[0].connected) {
			NetLinkPeer* p = &nl.peers[0];
			int fd = do_connect();
			if (fd < 0) { usleep(ACCEPT_POLL_MS * 1000); continue; }
			configure_socket(fd);
			if (!exchange_hello(fd, 0)) { close(fd); usleep(ACCEPT_POLL_MS * 1000); continue; }
			p->client_id = 0;                        /* the host's slot */
			peer_attach(p, fd);
		}

		/* One poll for the whole set rather than one per peer: polling them in turn
		 * would let the last guest wait a full timeout behind a poll that had
		 * already reported the others' data. */
		struct pollfd pfds[NETLINK_MAX_PEERS];
		NetLinkPeer* ready[NETLINK_MAX_PEERS];
		nfds_t n = 0;
		for (unsigned i = 0; i < NETLINK_MAX_PEERS; i++) {
			if (!nl.peers[i].connected) continue;
			pfds[n].fd = nl.peers[i].fd;
			pfds[n].events = POLLIN;
			pfds[n].revents = 0;
			ready[n] = &nl.peers[i];
			n++;
		}
		if (!n) { usleep(ACCEPT_POLL_MS * 1000); continue; }

		int pr = poll(pfds, n, POLL_WAIT_MS);
		if (pr < 0 && errno != EINTR) {
			char why[64];
			snprintf(why, sizeof(why), "poll error: %s", strerror(errno));
			for (nfds_t k = 0; k < n; k++) drop_connection(ready[k], why);
			continue;
		}
		/* Every peer is serviced every pass, not only those poll() reported:
		 * liveness, the heartbeat and the RTT probe are timely work, and the
		 * single-peer loop this replaces had the same property for the same
		 * reason. */
		for (nfds_t k = 0; k < n; k++) service_peer(ready[k], pfds[k].revents);
	}

	/* The worker is on its way out: every peer is dropped from here, the way
	 * the single-peer loop dropped its one connection when NetLink_stop asked
	 * it to stop. */
	for (unsigned i = 0; i < NETLINK_MAX_PEERS; i++)
		if (nl.peers[i].connected) drop_connection(&nl.peers[i], "worker stopping");
	return NULL;
}

static void service_peer(NetLinkPeer* p, short revents) {
/* Everything one connection needs doing: liveness, heartbeat, RTT probe, the
 * frontend-stall notice, then whatever the peer sent. Called once per connected
 * peer every pass, with revents from the poll the worker ran over the whole
 * set (0 when that peer had nothing). */
		if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
			drop_connection(p, "socket error from poll");
			return;
		}

		/* Liveness is checked every iteration, not only when the socket is idle.
		 * Putting the heartbeat in the idle branch starves it exactly when the
		 * peer is sending steadily: poll() always reports POLLIN, the idle
		 * branch never runs, and the peer times us out mid-session. */
		if (ms_since(&p->last_rx) > TIMEOUT_MS) {
			drop_connection(p, "silence timeout");
			return;
		}
		if (ms_since(&p->last_tx) > HEARTBEAT_MS) {
			pthread_mutex_lock(&nl.lock);
			bool sent = send_framed(p, CMD_PING, NULL, 0, NetLink_localClientId());
			pthread_mutex_unlock(&nl.lock);
			if (!sent) { drop_connection(p, "heartbeat send failed"); return; }
		}

		/* One probe in flight at a time: a lost echo costs one sample rather
		 * than corrupting the estimate with a mismatched pair. */
		if (!p->rtt_outstanding && ms_since(&p->rtt_last_probe) > RTT_PROBE_MS) {
			pthread_mutex_lock(&nl.lock);
			uint32_t token = ++p->rtt_token;
			uint32_t wire = htonl(token);
			bool sent = send_framed(p, CMD_RTT_PROBE, &wire, sizeof(wire),
			                        NetLink_localClientId());
			if (sent) {
				p->rtt_outstanding = true;
				gettimeofday(&p->rtt_sent_at, NULL);
			}
			gettimeofday(&p->rtt_last_probe, NULL);
			pthread_mutex_unlock(&nl.lock);
			if (!sent) { drop_connection(p, "rtt probe send failed"); return; }
		}

		/* Tell the peer when our frontend stalls, and when it comes back. The
		 * worker thread is the only part of us still running at that point. */
		if (nl.last_run.tv_sec) {
			/* Inside the core is not stalled, no matter how long it takes. */
			bool stalled = !nl.core_running && ms_since(&nl.last_run) > STALL_MS;
			if (stalled != p->told_peer_paused) {
				pthread_mutex_lock(&nl.lock);
				uint8_t reason = NETLINK_PAUSE_FRONTEND;
				bool sent = send_framed(p, stalled ? CMD_PAUSE : CMD_RESUME,
				                        stalled ? &reason : NULL, stalled ? 1 : 0,
				                        NetLink_localClientId());
				pthread_mutex_unlock(&nl.lock);
				if (sent) {
					p->told_peer_paused = stalled;
					nl_log("frontend %s - told peer\n", stalled ? "stalled" : "resumed");
				}
			}
		}

		if (!(revents & POLLIN)) return;

		/* Backpressure instead of discarding. When the frontend stalls - a menu,
		 * a sleep - nothing drains the queue, so stop taking data off the socket
		 * and let TCP throttle the peer. The bytes wait in the kernel and the
		 * session resumes intact rather than with a hole in it. */
		pthread_mutex_lock(&nl.lock);
		unsigned queued = (p->q_tail - p->q_head + QUEUE_SIZE) % QUEUE_SIZE;
		pthread_mutex_unlock(&nl.lock);
		if (queued >= QUEUE_HIGH_WATER) {
			if (!p->backpressure) {
				p->backpressure = true;
				nl_log("queue at %u/%u - applying backpressure\n", queued, QUEUE_SIZE);
			}
			usleep(2000);
			return;
		}
		if (p->backpressure) {
			p->backpressure = false;
			nl_log("queue drained to %u - resuming\n", queued);
		}

		NetLinkHeader hdr;
		const char* why = "header read failed";
		if (!read_all_why(p->fd, &hdr, sizeof(hdr), TIMEOUT_MS, &why)) {
			drop_connection(p, why);
			return;
		}

		uint16_t size = ntohs(hdr.size);
		if (size > NETLINK_MAX_PACKET) {
			nl_log("oversized packet (%u bytes)\n", size);
			drop_connection(p, "framing lost");
			return;
		}

		uint8_t buf[NETLINK_MAX_PACKET];
		if (size && !read_all(p->fd, buf, size, TIMEOUT_MS)) { drop_connection(p, "payload read failed"); return; }

		gettimeofday(&p->last_rx, NULL);

		if (hdr.cmd == CMD_PAUSE || hdr.cmd == CMD_RESUME) {
			pthread_mutex_lock(&nl.lock);
			p->peer_paused = (hdr.cmd == CMD_PAUSE);
			if (p->peer_paused) gettimeofday(&p->peer_paused_at, NULL);
			pthread_mutex_unlock(&nl.lock);
			if (hdr.cmd == CMD_PAUSE) {
				const char* why = (size && buf[0] == NETLINK_PAUSE_FRONTEND)
				                ? "frontend (menu/sleep)" : "unspecified";
				nl_log("peer paused: %s\n", why);
			} else {
				nl_log("peer resumed\n");
			}
		}

		if (hdr.cmd == CMD_INPUT && size == 8) {
			uint32_t f = ntohl(*(uint32_t*)buf);
			uint32_t b = ntohl(*(uint32_t*)(buf + 4));
			pthread_mutex_lock(&nl.lock);
			unsigned slot = f % INPUT_RING;
			p->inputs[slot].frame = f;
			p->inputs[slot].buttons = b;
			p->inputs[slot].valid = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_STATE && size > STATE_HEADER) {
			uint32_t total = ntohl(*(uint32_t*)buf);
			uint32_t off   = ntohl(*(uint32_t*)(buf + 4));
			uint32_t kind  = ntohl(*(uint32_t*)(buf + 8));
			size_t   n     = size - STATE_HEADER;

			pthread_mutex_lock(&nl.lock);
			bool valid = total != 0 && total <= NETLINK_MAX_STATE &&
			             off <= total && n <= total - off;
			if (!valid) {
				nl_log("bad state chunk (total=%u off=%u n=%zu) - ignoring\n", total, off, n);
			} else if (off == 0) {
				/* A new transfer starts. Anything half-received is abandoned; a
				 * transfer that already completed is safe on the done queue. */
				free(p->state_buf);
				p->state_buf = malloc(total);
				p->state_len = p->state_buf ? total : 0;
				p->state_have = 0;
				p->state_kind = kind;
			}
			if (valid && p->state_buf && p->state_len == total &&
			    p->state_kind == kind && off == p->state_have) {
				memcpy(p->state_buf + off, buf + STATE_HEADER, n);
				p->state_have += n;
				if (p->state_have >= total) {
					if (!push_state_locked(p, p->state_buf, total, kind)) {
						nl_log("state queue full - dropping a %u-byte kind=%u transfer\n",
						       total, kind);
						free(p->state_buf);
					}
					p->state_buf = NULL;
					p->state_len = p->state_have = 0;
				}
			} else if (valid && p->state_buf) {
				nl_log("out-of-order state chunk (wanted=%zu/kind=%u got=%u/kind=%u)"
				       " - discarding\n", p->state_have, p->state_kind, off, kind);
				free(p->state_buf);
				p->state_buf = NULL;
				p->state_len = p->state_have = 0;
			}
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RTT_PROBE && size == 4) {
			/* Echo verbatim and immediately - this runs on the worker thread, so
			 * the turnaround does not wait on the frontend and the number stays
			 * a measure of the link rather than of the peer's frame loop. */
			pthread_mutex_lock(&nl.lock);
			bool sent = p->connected &&
			            send_framed(p, CMD_RTT_ECHO, buf, 4, NetLink_localClientId());
			pthread_mutex_unlock(&nl.lock);
			if (!sent && p->connected) drop_connection(p, "rtt echo send failed");
		}

		if (hdr.cmd == CMD_RTT_ECHO && size == 4) {
			uint32_t token = ntohl(*(uint32_t*)buf);
			pthread_mutex_lock(&nl.lock);
			if (p->rtt_outstanding && token == p->rtt_token) {
				struct timeval now;
				gettimeofday(&now, NULL);
				long sec = now.tv_sec - p->rtt_sent_at.tv_sec;
				long usec = now.tv_usec - p->rtt_sent_at.tv_usec;
				long long total = (long long)sec * 1000000LL + usec;
				if (total < 0) total = 0;
				if (total > UINT32_MAX) total = UINT32_MAX;
				p->rtt_us[p->rtt_next] = (uint32_t)total;
				p->rtt_next = (p->rtt_next + 1) % RTT_SAMPLES;
				if (p->rtt_count < RTT_SAMPLES) p->rtt_count++;
				p->rtt_outstanding = false;
			}
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_LINK_VERDICT && size == 8) {
			pthread_mutex_lock(&nl.lock);
			p->link_verdict_can_pair = ntohl(*(uint32_t*)buf) != 0;
			p->link_verdict_reason = ntohl(*(uint32_t*)(buf + 4));
			p->link_verdict_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

#if 0 /* Frozen executable transfer path. */
		if (hdr.cmd == CMD_CORE && size > 8) {
			uint32_t total = ntohl(*(uint32_t*)buf);
			uint32_t off   = ntohl(*(uint32_t*)(buf + 4));
			size_t   n     = size - 8;

			/* Same bounds discipline as CMD_STATE: a hostile or corrupt total
			 * must not become a huge malloc, and off/n must not wrap. */
			if (total == 0 || total > NETLINK_MAX_CORE || off > total || n > total - off) {
				nl_log("bad core chunk (total=%u off=%u n=%zu) - ignoring\n", total, off, n);
			} else {
				pthread_mutex_lock(&nl.lock);
				if (!nl.core_buf || nl.core_len != total) {
					free(nl.core_buf);
					nl.core_buf = malloc(total);
					nl.core_len = total;
					nl.core_have = 0;
					nl.core_ready = false;
				}
				if (nl.core_buf) {
					memcpy(nl.core_buf + off, buf + 8, n);
					nl.core_have += n;
					if (nl.core_have >= total) {
						nl.core_ready = true;
						nl_log("core received (%u bytes)\n", total);
					}
				}
				pthread_mutex_unlock(&nl.lock);
			}
		}
#endif

		if (hdr.cmd == CMD_HASH && size == 8) {
			pthread_mutex_lock(&nl.lock);
			uint32_t hf = ntohl(*(uint32_t*)buf);
			uint32_t hv = ntohl(*(uint32_t*)(buf + 4));
			/* Keep every checkpoint until the emulator thread consumes it.
				 * A single slot let a newer hash overwrite a late older one. */
				if (p->peer_hash_count == HASH_RING) {
					p->peer_hash_head = (p->peer_hash_head + 1) % HASH_RING;
					p->peer_hash_count--;
					nl_log("state-hash queue full - dropped oldest checkpoint\n");
				}
				unsigned slot = (p->peer_hash_head + p->peer_hash_count) % HASH_RING;
				p->peer_hashes[slot].frame = hf;
				p->peer_hashes[slot].hash = hv;
			p->peer_hash_count++;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RESYNC_REQUEST && size == 4) {
			pthread_mutex_lock(&nl.lock);
			p->resync_request_frame = ntohl(*(uint32_t*)buf);
			p->resync_request_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RESYNC_BEGIN && size == 12) {
			pthread_mutex_lock(&nl.lock);
			p->resync_begin_epoch = ntohl(*(uint32_t*)buf);
			p->resync_begin_frame = ntohl(*(uint32_t*)(buf + 4));
			uint32_t kind = ntohl(*(uint32_t*)(buf + 8));
			p->resync_begin_kind = kind == NETLINK_RECOVERY_RESET
			                     ? NETLINK_RECOVERY_RESET : NETLINK_RECOVERY_SYNC;
			p->resync_begin_ready = true;
			/* BEGIN owns the state stream which follows it: anything still
			 * queued or half-received belongs to an abandoned attempt. */
			clear_states_locked(p);
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RESYNC_ACK && size == 8) {
			pthread_mutex_lock(&nl.lock);
			p->resync_ack_epoch = ntohl(*(uint32_t*)buf);
			p->resync_ack_loaded = ntohl(*(uint32_t*)(buf + 4)) != 0;
			p->resync_ack_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RESYNC_COMMIT && size == 4) {
			pthread_mutex_lock(&nl.lock);
			p->resync_commit_epoch = ntohl(*(uint32_t*)buf);
			p->resync_commit_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_SESSION_IDENTITY && size == 72) {
			pthread_mutex_lock(&nl.lock);
			memcpy(p->peer_session_identity.rom_sha256, buf, 32);
			p->peer_session_identity.mode = ntohl(*(uint32_t*)(buf + 32));
			p->peer_session_identity.input_delay = ntohl(*(uint32_t*)(buf + 36));
			p->peer_session_identity.core_identity = ntohl(*(uint32_t*)(buf + 40));
			p->peer_session_identity.state_size = ntohl(*(uint32_t*)(buf + 44));
			p->peer_session_identity.sram_size = ntohl(*(uint32_t*)(buf + 48));
			p->peer_session_identity.rtc_size = ntohl(*(uint32_t*)(buf + 52));
			p->peer_session_identity.rom_size = ntohl(*(uint32_t*)(buf + 56));
			p->peer_session_identity.wall_clock_utc =
				((uint64_t)ntohl(*(uint32_t*)(buf + 60)) << 32) |
				ntohl(*(uint32_t*)(buf + 64));
			p->peer_session_identity.rom_crc32 = ntohl(*(uint32_t*)(buf + 68));
			p->peer_session_identity_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_CHECKPOINT_ACK && size == 12) {
			pthread_mutex_lock(&nl.lock);
			p->checkpoint_ack_frame = ntohl(*(uint32_t*)buf);
			p->checkpoint_ack_hash = ntohl(*(uint32_t*)(buf + 4));
			p->checkpoint_ack_matched = ntohl(*(uint32_t*)(buf + 8)) != 0;
			p->checkpoint_ack_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_DATA && size) {
			uint16_t from = ntohs(hdr.client_id);
			NetLinkPeer* broken[NETLINK_MAX_PEERS];
			unsigned broken_n = 0;

			pthread_mutex_lock(&nl.lock);
			p->rx_count++;
			queue_push(p, buf, size, from);
			/* Link play is a bus: what one console sends, every console sees. The
			 * host is the hub, so it copies the packet on to the other guests,
			 * still marked with the sender's slot - which is what the four-player
			 * adapter did, and what gpSP's Advance Wars protocol expects. A guest
			 * has nobody to relay to; the host does the relaying for all of them.
			 * The queue_push above already gave our own core its copy. */
			if (nl.role == NETLINK_ROLE_HOST) {
				for (unsigned i = 1; i < NETLINK_MAX_PEERS; i++) {
					NetLinkPeer* other = &nl.peers[i];
					if (other == p || !other->connected) continue;
					if (!send_framed(other, CMD_DATA, buf, size, from) &&
					    broken_n < NETLINK_MAX_PEERS)
						broken[broken_n++] = other;
				}
			}
			pthread_mutex_unlock(&nl.lock);

			/* A relay that did not finish leaves that guest's stream broken
			 * mid-frame: write_all never reports a half-written frame as success, so
			 * this is a dead connection rather than a dropped packet. Dropped out
			 * here because drop_connection() takes the lock itself. */
			for (unsigned i = 0; i < broken_n; i++)
				drop_connection(broken[i], "relay send failed");
		}
		/* CMD_PING needs no handling beyond refreshing last_rx above. */
}


//////////////////////////////////////////////////////////////////////////////
// public
//////////////////////////////////////////////////////////////////////////////

bool NetLink_start(void) {
	if (!nl.configured || nl.running) return false;

	nl.running = true;
	if (pthread_create(&nl.thread, NULL, worker, NULL) != 0) {
		nl_log("could not start worker thread\n");
		nl.running = false;
		return false;
	}
	return true;
}

void NetLink_stop(void) {
	if (!nl.running) return;

	nl.running = false;
	pthread_join(nl.thread, NULL);

	if (nl.listen_fd >= 0) { close(nl.listen_fd); nl.listen_fd = -1; }
	if (NL_PRIMARY->fd >= 0)        { close(NL_PRIMARY->fd);        NL_PRIMARY->fd = -1; }
	NL_PRIMARY->connected = false;

	if (NL_PRIMARY->dropped) nl_log("%u packet(s) were dropped this session\n", NL_PRIMARY->dropped);
}

void NetLink_markFrame(void) {
	gettimeofday(&nl.last_run, NULL);
}

void NetLink_setCoreRunning(bool running) {
	nl.core_running = running;
	if (!running) gettimeofday(&nl.last_run, NULL);
}

bool NetLink_isPeerPaused(void) {
	pthread_mutex_lock(&nl.lock);
	bool p = NL_PRIMARY->peer_paused;
	/* A live peer may legitimately remain in its menu indefinitely. The worker
	 * keeps exchanging heartbeats while both frontends are paused, and
	 * drop_connection() clears this flag if the process/device actually dies.
	 * A time-based forced resume ran a link core alone after 30 seconds, broke
	 * Gambatte's separate serial socket, and made two-menu resume unrecoverable. */
	pthread_mutex_unlock(&nl.lock);
	return p;
}

bool NetLink_isConnected(void) {
	pthread_mutex_lock(&nl.lock);
	bool c = NL_PRIMARY->connected;
	pthread_mutex_unlock(&nl.lock);
	return c;
}

/* Connecting and disconnecting concern one console at a time, so the event says
 * which one. A host running four players sees guests arrive and leave while the
 * session continues; the shim tells its core which slot each event was about. */
bool NetLink_consumeConnectEvent(uint16_t* client_id) {
	pthread_mutex_lock(&nl.lock);
	for (unsigned i = 0; i < NETLINK_MAX_PEERS; i++) {
		if (!nl.peers[i].connect_event) continue;
		nl.peers[i].connect_event = false;
		if (client_id) *client_id = nl.peers[i].client_id;
		pthread_mutex_unlock(&nl.lock);
		return true;
	}
	pthread_mutex_unlock(&nl.lock);
	return false;
}

/* The slots that are connected right now. A caller that must address each peer -
 * a teardown telling every console the session is over - needs the list, because
 * any of them can be the only one left. Returns how many were written. */
unsigned NetLink_connectedIds(uint16_t* out, unsigned cap) {
	unsigned n = 0;
	pthread_mutex_lock(&nl.lock);
	for (unsigned i = 0; i < NETLINK_MAX_PEERS && n < cap; i++)
		if (nl.peers[i].connected) out[n++] = nl.peers[i].client_id;
	pthread_mutex_unlock(&nl.lock);
	return n;
}

bool NetLink_consumeDisconnectEvent(uint16_t* client_id) {
	pthread_mutex_lock(&nl.lock);
	for (unsigned i = 0; i < NETLINK_MAX_PEERS; i++) {
		if (!nl.peers[i].disconnect_event) continue;
		nl.peers[i].disconnect_event = false;
		if (client_id) *client_id = nl.peers[i].client_id;
		pthread_mutex_unlock(&nl.lock);
		return true;
	}
	pthread_mutex_unlock(&nl.lock);
	return false;
}

uint32_t NetLink_connectionGeneration(void) {
	pthread_mutex_lock(&nl.lock);
	uint32_t generation = NL_PRIMARY->connection_generation;
	pthread_mutex_unlock(&nl.lock);
	return generation;
}

//////////////////////////////////////////////////////////////////////////////
// shared-screen netplay
//////////////////////////////////////////////////////////////////////////////

static void reset_timeline_locked(void) {
	memset(NL_PRIMARY->inputs, 0, sizeof(NL_PRIMARY->inputs));
	NL_PRIMARY->peer_hash_head = NL_PRIMARY->peer_hash_count = 0;
	NL_PRIMARY->checkpoint_ack_ready = false;
}

void NetLink_resetTimeline(void) {
	pthread_mutex_lock(&nl.lock);
	reset_timeline_locked();
	pthread_mutex_unlock(&nl.lock);
}

void NetLink_resetSync(void) {
	pthread_mutex_lock(&nl.lock);
	reset_timeline_locked();
	clear_states_locked(NL_PRIMARY);
	NL_PRIMARY->resync_request_ready = NL_PRIMARY->resync_begin_ready = false;
	NL_PRIMARY->resync_ack_ready = NL_PRIMARY->resync_commit_ready = false;
	/* Deliberately not the peer identity or the link verdict. Both peers detect
	 * a new connection at different moments - the client is connected as soon as
	 * connect() returns, the host only once its greeting completes - so the peer
	 * can legitimately have sent its identity before this side gets here.
	 * Discarding it strands both. Staleness is handled where it belongs, at
	 * drop_connection(): anything from the connection that just died is cleared
	 * before the next one can deliver anything. */
	pthread_mutex_unlock(&nl.lock);
}

/* Every framed send has the same failure semantics. A partial control/state
 * transaction cannot be repaired on the same byte stream; close it and let the
 * next connection generation start from a clean identity/state handshake. */
static bool send_command_to(NetLinkPeer* peer, uint8_t cmd, const void* data, size_t len) {
	pthread_mutex_lock(&nl.lock);
	bool connected = peer->connected;
	bool ok = connected && send_framed(peer, cmd, data, len, NetLink_localClientId());
	pthread_mutex_unlock(&nl.lock);
	if (!ok && connected) drop_connection(peer, "protocol send failed");
	return ok;
}

/* The control conversation is between the session and its one peer in both roles
 * - a guest's host, a host's first guest - so the primary is the right peer. */
static bool send_command(uint8_t cmd, const void* data, size_t len) {
	return send_command_to(NL_PRIMARY, cmd, data, len);
}

bool NetLink_sendInput(uint32_t frame, uint32_t buttons) {
	uint32_t payload[2] = { htonl(frame), htonl(buttons) };
	return send_command(CMD_INPUT, payload, sizeof(payload));
}

bool NetLink_getRemoteInput(uint32_t frame, uint32_t* buttons) {
	pthread_mutex_lock(&nl.lock);
	unsigned slot = frame % INPUT_RING;
	bool ok = NL_PRIMARY->inputs[slot].valid && NL_PRIMARY->inputs[slot].frame == frame;
	if (ok && buttons) *buttons = NL_PRIMARY->inputs[slot].buttons;
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_sendState(NetLinkStateKind kind, const void* data, size_t len) {
	if (!len || len > NETLINK_MAX_STATE) return false;
	const uint8_t* p = data;
	for (size_t off = 0; off < len; off += STATE_CHUNK) {
		size_t n = len - off;
		if (n > STATE_CHUNK) n = STATE_CHUNK;

		uint8_t pkt[STATE_HEADER + STATE_CHUNK];
		uint32_t hdr[3] = { htonl((uint32_t)len), htonl((uint32_t)off), htonl((uint32_t)kind) };
		memcpy(pkt, hdr, STATE_HEADER);
		memcpy(pkt + STATE_HEADER, p + off, n);

		bool ok = send_command(CMD_STATE, pkt, n + STATE_HEADER);
		if (!ok) return false;
	}
	return true;
}

/* Both figures, because they answer different questions. The median describes
 * what the link usually does and is the honest thing to log. The maximum is
 * what an input-delay window has to survive: a budget sized to the median of an
 * access-point link is blown by every spike, which is measurable as a stall
 * share rather than as latency. */
bool NetLink_rttStats(uint32_t* median_us, uint32_t* max_us, unsigned* samples) {
	uint32_t sorted[RTT_SAMPLES];
	unsigned n;
	pthread_mutex_lock(&nl.lock);
	n = NL_PRIMARY->rtt_count;
	memcpy(sorted, NL_PRIMARY->rtt_us, sizeof(sorted));
	pthread_mutex_unlock(&nl.lock);
	if (samples) *samples = n;
	if (!n) return false;
	for (unsigned i = 1; i < n; i++) {
		uint32_t v = sorted[i];
		unsigned j = i;
		while (j && sorted[j - 1] > v) { sorted[j] = sorted[j - 1]; j--; }
		sorted[j] = v;
	}
	if (median_us) *median_us = sorted[n / 2];
	if (max_us) *max_us = sorted[n - 1];
	return true;
}

bool NetLink_sendLinkVerdict(bool can_pair, uint32_t reason) {
	uint32_t wire[2] = { htonl(can_pair ? 1u : 0u), htonl(reason) };
	return send_command(CMD_LINK_VERDICT, wire, sizeof(wire));
}

bool NetLink_takeLinkVerdict(bool* can_pair, uint32_t* reason) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->link_verdict_ready;
	if (ok) {
		if (can_pair) *can_pair = NL_PRIMARY->link_verdict_can_pair;
		if (reason) *reason = NL_PRIMARY->link_verdict_reason;
		NL_PRIMARY->link_verdict_ready = false;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

#if 0 /* Frozen executable-sharing API. */
void NetLink_setCoreId(const NetLinkCoreId* id) {
	pthread_mutex_lock(&nl.lock);
	nl.core_id = *id;
	pthread_mutex_unlock(&nl.lock);
}

bool NetLink_peerCoreId(NetLinkCoreId* out) {
	pthread_mutex_lock(&nl.lock);
	bool known = nl.peer_core_known;
	if (known && out) *out = nl.peer_core_id;
	pthread_mutex_unlock(&nl.lock);
	return known;
}

bool NetLink_sendCore(const void* data, size_t len) {
	if (!len || len > NETLINK_MAX_CORE) return false;
	const uint8_t* p = data;
	for (size_t off = 0; off < len; off += STATE_CHUNK) {
		size_t n = len - off;
		if (n > STATE_CHUNK) n = STATE_CHUNK;

		uint8_t pkt[8 + STATE_CHUNK];
		uint32_t hdr[2] = { htonl((uint32_t)len), htonl((uint32_t)off) };
		memcpy(pkt, hdr, 8);
		memcpy(pkt + 8, p + off, n);

		pthread_mutex_lock(&nl.lock);
		bool ok = NL_PRIMARY->connected && send_framed(NL_PRIMARY, CMD_CORE, pkt, n + 8, NetLink_localClientId());
		pthread_mutex_unlock(&nl.lock);
		if (!ok) return false;
	}
	return true;
}

bool NetLink_takeCore(void** data, size_t* len) {
	pthread_mutex_lock(&nl.lock);
	if (!nl.core_ready) { pthread_mutex_unlock(&nl.lock); return false; }
	*data = nl.core_buf;
	*len  = nl.core_len;
	nl.core_buf = NULL;
	nl.core_len = 0;
	nl.core_have = 0;
	nl.core_ready = false;
	pthread_mutex_unlock(&nl.lock);
	return true;
}
#endif

/* Kinds keep the two bootstrap transfers apart even if they arrive back to
 * back: a payload of the wrong kind stays queued rather than being handed to a
 * caller that would reject it and fail the session. */
bool NetLink_takeState(NetLinkStateKind kind, void** data, size_t* len) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->state_done_count && NL_PRIMARY->state_done[NL_PRIMARY->state_done_head].kind == (uint32_t)kind;
	if (ok) {
		*data = NL_PRIMARY->state_done[NL_PRIMARY->state_done_head].buf;
		*len  = NL_PRIMARY->state_done[NL_PRIMARY->state_done_head].len;
		NL_PRIMARY->state_done[NL_PRIMARY->state_done_head].buf = NULL;
		NL_PRIMARY->state_done_head = (NL_PRIMARY->state_done_head + 1) % STATE_QUEUE;
		NL_PRIMARY->state_done_count--;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_sendHash(uint32_t frame, uint32_t hash) {
	uint32_t payload[2] = { htonl(frame), htonl(hash) };
	return send_command(CMD_HASH, payload, sizeof(payload));
}

bool NetLink_sendSessionIdentity(const NetLinkSessionIdentity* identity) {
	uint8_t wire[72];
	memcpy(wire, identity->rom_sha256, 32);
	uint32_t v = htonl(identity->mode); memcpy(wire + 32, &v, 4);
	v = htonl(identity->input_delay); memcpy(wire + 36, &v, 4);
	v = htonl(identity->core_identity); memcpy(wire + 40, &v, 4);
	v = htonl(identity->state_size); memcpy(wire + 44, &v, 4);
	v = htonl(identity->sram_size); memcpy(wire + 48, &v, 4);
	v = htonl(identity->rtc_size); memcpy(wire + 52, &v, 4);
	v = htonl(identity->rom_size); memcpy(wire + 56, &v, 4);
	/* Split rather than sent as a 64-bit field: everything else on this wire is
	 * a pair of 32-bit halves in network order, and a clock is not worth being
	 * the one place that needs a htonll. */
	v = htonl((uint32_t)(identity->wall_clock_utc >> 32)); memcpy(wire + 60, &v, 4);
	v = htonl((uint32_t)identity->wall_clock_utc); memcpy(wire + 64, &v, 4);
	v = htonl(identity->rom_crc32); memcpy(wire + 68, &v, 4);
	return send_command(CMD_SESSION_IDENTITY, wire, sizeof(wire));
}

bool NetLink_takeSessionIdentity(NetLinkSessionIdentity* identity) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->peer_session_identity_ready;
	if (ok) { *identity = NL_PRIMARY->peer_session_identity; NL_PRIMARY->peer_session_identity_ready = false; }
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_ackCheckpoint(uint32_t frame, uint32_t hash, bool matched) {
	uint32_t wire[3] = { htonl(frame), htonl(hash), htonl(matched ? 1u : 0u) };
	return send_command(CMD_CHECKPOINT_ACK, wire, sizeof(wire));
}

bool NetLink_takeCheckpointAck(uint32_t* frame, uint32_t* hash, bool* matched) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->checkpoint_ack_ready;
	if (ok) {
		*frame = NL_PRIMARY->checkpoint_ack_frame;
		*hash = NL_PRIMARY->checkpoint_ack_hash;
		*matched = NL_PRIMARY->checkpoint_ack_matched;
		NL_PRIMARY->checkpoint_ack_ready = false;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_takeHash(uint32_t* frame, uint32_t* hash) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->peer_hash_count != 0;
	if (ok) {
		*frame = NL_PRIMARY->peer_hashes[NL_PRIMARY->peer_hash_head].frame;
		*hash  = NL_PRIMARY->peer_hashes[NL_PRIMARY->peer_hash_head].hash;
		NL_PRIMARY->peer_hash_head = (NL_PRIMARY->peer_hash_head + 1) % HASH_RING;
		NL_PRIMARY->peer_hash_count--;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

static bool send_u32_control(uint8_t cmd, uint32_t value) {
	uint32_t wire = htonl(value);
	return send_command(cmd, &wire, sizeof(wire));
}

bool NetLink_requestResync(uint32_t frame) { return send_u32_control(CMD_RESYNC_REQUEST, frame); }
bool NetLink_commitResync(uint32_t epoch)  { return send_u32_control(CMD_RESYNC_COMMIT, epoch); }

bool NetLink_beginResync(uint32_t epoch, uint32_t resume_frame, NetLinkRecoveryKind kind) {
	uint32_t wire[3] = { htonl(epoch), htonl(resume_frame), htonl((uint32_t)kind) };
	return send_command(CMD_RESYNC_BEGIN, wire, sizeof(wire));
}

bool NetLink_ackResync(uint32_t epoch, bool loaded) {
	uint32_t wire[2] = { htonl(epoch), htonl(loaded ? 1u : 0u) };
	return send_command(CMD_RESYNC_ACK, wire, sizeof(wire));
}

bool NetLink_takeResyncRequest(uint32_t* frame) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->resync_request_ready;
	if (ok) { *frame = NL_PRIMARY->resync_request_frame; NL_PRIMARY->resync_request_ready = false; }
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_takeResyncBegin(uint32_t* epoch, uint32_t* resume_frame, NetLinkRecoveryKind* kind) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->resync_begin_ready;
	if (ok) {
		*epoch = NL_PRIMARY->resync_begin_epoch;
		*resume_frame = NL_PRIMARY->resync_begin_frame;
		*kind = NL_PRIMARY->resync_begin_kind;
		NL_PRIMARY->resync_begin_ready = false;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_takeResyncAck(uint32_t* epoch, bool* loaded) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->resync_ack_ready;
	if (ok) {
		*epoch = NL_PRIMARY->resync_ack_epoch;
		*loaded = NL_PRIMARY->resync_ack_loaded;
		NL_PRIMARY->resync_ack_ready = false;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_takeResyncCommit(uint32_t* epoch) {
	pthread_mutex_lock(&nl.lock);
	bool ok = NL_PRIMARY->resync_commit_ready;
	if (ok) { *epoch = NL_PRIMARY->resync_commit_epoch; NL_PRIMARY->resync_commit_ready = false; }
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_send(int flags, const void* buf, size_t len, uint16_t client_id) {
	(void)flags;     /* TCP_NODELAY means every packet is already flushed. */

	if (!buf || !len) return true; /* flush-only request */
	if (len > NETLINK_MAX_PACKET) {
		nl_log("refusing to send %zu bytes (max %d)\n", len, NETLINK_MAX_PACKET);
		return false;
	}

	/* A guest's only peer is the host, so whatever its core addressed, the packet
	 * has one place to go. The host is the hub: it addresses a guest by slot, or
	 * every guest at once for a broadcast. The header carries the sender's slot
	 * either way, which is how the receiving core attributes the packet. */
	if (nl.role != NETLINK_ROLE_HOST) return send_command(CMD_DATA, buf, len);

	if (client_id == RETRO_NETPACKET_BROADCAST) {
		bool ok = true;
		for (unsigned i = 1; i < NETLINK_MAX_PEERS; i++) {
			NetLinkPeer* peer = &nl.peers[i];
			if (!peer->connected) continue;
			if (!send_command_to(peer, CMD_DATA, buf, len)) ok = false;
		}
		return ok;
	}
	/* Slot 0 is the host itself, and one past the table is nobody. */
	if (client_id == 0 || client_id >= NETLINK_MAX_PEERS) {
		nl_log("no such peer %u to send %zu bytes to\n", client_id, len);
		return false;
	}
	return send_command_to(&nl.peers[client_id], CMD_DATA, buf, len);
}

bool NetLink_popPacket(void* out, size_t out_cap, size_t* out_len, uint16_t* client_id) {
	pthread_mutex_lock(&nl.lock);
	/* Every peer queues its own arrivals. Within one peer the order is the order
	 * it arrived; across peers there is no meaningful order to preserve - the
	 * cores key on the sender's id, not on a global sequence. */
	for (unsigned i = 0; i < NETLINK_MAX_PEERS; i++) {
		QueuedPacket* q = &nl.peers[i].queue[nl.peers[i].q_head];
		if (nl.peers[i].q_head == nl.peers[i].q_tail) continue;
		size_t n = q->len < out_cap ? q->len : out_cap;
		memcpy(out, q->data, n);
		*out_len = n;
		if (client_id) *client_id = q->client_id;
		nl.peers[i].q_head = (nl.peers[i].q_head + 1) % QUEUE_SIZE;
		pthread_mutex_unlock(&nl.lock);
		return true;
	}
	pthread_mutex_unlock(&nl.lock);
	return false;
}

unsigned NetLink_droppedPackets(void) { return NL_PRIMARY->dropped; }
