#include "netlink.h"

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
#define NETLINK_PROTOCOL 9

#define QUEUE_SIZE    512
/* Inputs are indexed by frame; the ring only has to outlast the input delay
 * plus any burst of catch-up packets. */
#define INPUT_RING    256
#define HASH_RING       8
#define STATE_CHUNK   1024
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
	CMD_STATE  = 0x06, /* {total, offset, bytes} */
	CMD_HASH   = 0x07, /* {frame, hash} */
	CMD_RESYNC_REQUEST = 0x08, /* {mismatched frame} */
	CMD_RESYNC_BEGIN   = 0x09, /* {epoch, resume_frame, kind}; CMD_STATE follows */
	CMD_RESYNC_ACK     = 0x0A, /* {epoch, loaded} */
	CMD_RESYNC_COMMIT  = 0x0B, /* {epoch} */
	CMD_SESSION_IDENTITY = 0x0C, /* {rom sha256, mode, delay, core/state/save identity} */
	CMD_CHECKPOINT_ACK = 0x0D, /* {frame, hash, matched} */
};

typedef struct __attribute__((packed)) {
	uint8_t  cmd;
	uint16_t size;      /* network order */
	uint16_t client_id; /* network order */
} NetLinkHeader;

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t protocol;
} NetLinkHello;

typedef struct {
	uint8_t data[NETLINK_MAX_PACKET];
	size_t  len;
} QueuedPacket;

static struct {
	NetLinkRole role;
	uint16_t    port;
	char        peer[64];
	bool        configured;

	int  listen_fd;
	int  fd;
	bool connected;
	bool running;

	pthread_t       thread;
	pthread_mutex_t lock;

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

	uint8_t* state_buf;
	size_t   state_len;
	size_t   state_have;
	bool     state_ready;

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

	struct timeval last_run;      /* last retro_run, per NetLink_markFrame */
	struct timeval peer_paused_at;
	bool           told_peer_paused;
	bool           core_running;
	bool           peer_paused;

	bool connect_event;
	bool disconnect_event;
	uint32_t connection_generation;

	NetLinkSessionIdentity peer_session_identity;
	bool peer_session_identity_ready;
	uint32_t checkpoint_ack_frame;
	uint32_t checkpoint_ack_hash;
	bool checkpoint_ack_matched;
	bool checkpoint_ack_ready;

	struct timeval last_rx;
	struct timeval last_tx;
} nl;

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
static bool send_framed(uint8_t cmd, const void* data, size_t len, uint16_t client_id) {
	if (nl.fd < 0) return false;

	NetLinkHeader hdr = {
		.cmd       = cmd,
		.size      = htons((uint16_t)len),
		.client_id = htons(client_id),
	};

	if (!write_all(nl.fd, &hdr, sizeof(hdr))) return false;
	if (len && data && !write_all(nl.fd, data, len)) return false;

	gettimeofday(&nl.last_tx, NULL);
	return true;
}

//////////////////////////////////////////////////////////////////////////////
// configuration
//////////////////////////////////////////////////////////////////////////////

bool NetLink_configure(const char* session_path) {
	memset(&nl, 0, sizeof(nl));
	nl.listen_fd = -1;
	nl.fd = -1;
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
	nl_log("session: role=%s port=%u%s%s\n",
	       nl.role == NETLINK_ROLE_HOST ? "host" : "client",
	       nl.port, nl.peer[0] ? " peer=" : "", nl.peer[0] ? nl.peer : "");
	return true;
}

bool NetLink_isConfigured(void)  { return nl.configured; }
NetLinkRole NetLink_getRole(void) { return nl.role; }

uint16_t NetLink_localClientId(void) {
	return nl.role == NETLINK_ROLE_HOST ? 0 : 1;
}
uint16_t NetLink_remoteClientId(void) {
	return nl.role == NETLINK_ROLE_HOST ? 1 : 0;
}

//////////////////////////////////////////////////////////////////////////////
// connection setup, on the worker thread
//////////////////////////////////////////////////////////////////////////////

static bool exchange_hello(int fd) {
	NetLinkHello mine = {
		.magic = htonl(NETLINK_MAGIC), .protocol = htonl(NETLINK_PROTOCOL),
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

static void queue_push(const uint8_t* data, size_t len) {
	unsigned next = (nl.q_tail + 1) % QUEUE_SIZE;
	if (next == nl.q_head) {
		/* Full. Dropping is better than blocking the reader, but it means the
		 * core missed data, so make it visible. */
		nl.dropped++;
		return;
	}
	memcpy(nl.queue[nl.q_tail].data, data, len);
	nl.queue[nl.q_tail].len = len;
	nl.q_tail = next;
}

// Always say why. "peer lost" alone is not diagnosable after the fact, and the
// interesting failures here are all distinguishable at the point of detection.
static void drop_connection(const char* reason) {
	pthread_mutex_lock(&nl.lock);
	if (nl.fd >= 0) { close(nl.fd); nl.fd = -1; }
	if (nl.connected) {
		nl.connected = false;
		nl.disconnect_event = true;
		/* Whatever the peer's last state was, it is gone now. Leaving this set
		 * strands us waiting for a CMD_RESUME that can never arrive. */
		nl.peer_paused = false;
		nl.told_peer_paused = false;
		nl_log("peer lost: %s (rx %ldms ago, tx %ldms ago, %u pkts in, %u dropped)\n",
		       reason, ms_since(&nl.last_rx), ms_since(&nl.last_tx),
		       nl.rx_count, nl.dropped);
	}
	pthread_mutex_unlock(&nl.lock);
}

static void* worker(void* arg) {
	(void)arg;

	while (nl.running) {
		/* --- not connected: keep trying --- */
		if (!nl.connected) {
			int fd = (nl.role == NETLINK_ROLE_HOST) ? do_listen_accept() : do_connect();
			if (fd < 0) {
				usleep(ACCEPT_POLL_MS * 1000);
				continue;
			}

			configure_socket(fd);
			if (!exchange_hello(fd)) {
				close(fd);
				usleep(ACCEPT_POLL_MS * 1000);
				continue;
			}

			pthread_mutex_lock(&nl.lock);
			nl.fd = fd;
			nl.peer_session_identity_ready = false;
			nl.checkpoint_ack_ready = false;
			nl.connected = true;
			nl.connect_event = true;
			nl.connection_generation++;
			if (!nl.connection_generation) nl.connection_generation++;
			gettimeofday(&nl.last_rx, NULL);
			gettimeofday(&nl.last_tx, NULL);
			pthread_mutex_unlock(&nl.lock);

			nl_log("connected as %s (client_id %u)\n",
			       nl.role == NETLINK_ROLE_HOST ? "host" : "client",
			       NetLink_localClientId());
			continue;
		}

		/* --- connected: wait for a frame, or wake to heartbeat --- */
		struct pollfd pfd = { .fd = nl.fd, .events = POLLIN, .revents = 0 };
		int pr = poll(&pfd, 1, POLL_WAIT_MS);

		if (pr < 0 && errno != EINTR) {
			char why[64];
			snprintf(why, sizeof(why), "poll error: %s", strerror(errno));
			drop_connection(why);
			continue;
		}

		if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
			drop_connection("socket error from poll");
			continue;
		}

		/* Liveness is checked every iteration, not only when the socket is idle.
		 * Putting the heartbeat in the idle branch starves it exactly when the
		 * peer is sending steadily: poll() always reports POLLIN, the idle
		 * branch never runs, and the peer times us out mid-session. */
		if (ms_since(&nl.last_rx) > TIMEOUT_MS) {
			drop_connection("silence timeout");
			continue;
		}
		if (ms_since(&nl.last_tx) > HEARTBEAT_MS) {
			pthread_mutex_lock(&nl.lock);
			bool sent = send_framed(CMD_PING, NULL, 0, NetLink_localClientId());
			pthread_mutex_unlock(&nl.lock);
			if (!sent) { drop_connection("heartbeat send failed"); continue; }
		}

		/* Tell the peer when our frontend stalls, and when it comes back. The
		 * worker thread is the only part of us still running at that point. */
		if (nl.last_run.tv_sec) {
			/* Inside the core is not stalled, no matter how long it takes. */
			bool stalled = !nl.core_running && ms_since(&nl.last_run) > STALL_MS;
			if (stalled != nl.told_peer_paused) {
				pthread_mutex_lock(&nl.lock);
				uint8_t reason = NETLINK_PAUSE_FRONTEND;
				bool sent = send_framed(stalled ? CMD_PAUSE : CMD_RESUME,
				                        stalled ? &reason : NULL, stalled ? 1 : 0,
				                        NetLink_localClientId());
				pthread_mutex_unlock(&nl.lock);
				if (sent) {
					nl.told_peer_paused = stalled;
					nl_log("frontend %s - told peer\n", stalled ? "stalled" : "resumed");
				}
			}
		}

		if (pr <= 0 || !(pfd.revents & POLLIN)) continue;

		/* Backpressure instead of discarding. When the frontend stalls - a menu,
		 * a sleep - nothing drains the queue, so stop taking data off the socket
		 * and let TCP throttle the peer. The bytes wait in the kernel and the
		 * session resumes intact rather than with a hole in it. */
		pthread_mutex_lock(&nl.lock);
		unsigned queued = (nl.q_tail - nl.q_head + QUEUE_SIZE) % QUEUE_SIZE;
		pthread_mutex_unlock(&nl.lock);
		if (queued >= QUEUE_HIGH_WATER) {
			if (!nl.backpressure) {
				nl.backpressure = true;
				nl_log("queue at %u/%u - applying backpressure\n", queued, QUEUE_SIZE);
			}
			usleep(2000);
			continue;
		}
		if (nl.backpressure) {
			nl.backpressure = false;
			nl_log("queue drained to %u - resuming\n", queued);
		}

		NetLinkHeader hdr;
		const char* why = "header read failed";
		if (!read_all_why(nl.fd, &hdr, sizeof(hdr), TIMEOUT_MS, &why)) {
			drop_connection(why);
			continue;
		}

		uint16_t size = ntohs(hdr.size);
		if (size > NETLINK_MAX_PACKET) {
			nl_log("oversized packet (%u bytes)\n", size);
			drop_connection("framing lost");
			continue;
		}

		uint8_t buf[NETLINK_MAX_PACKET];
		if (size && !read_all(nl.fd, buf, size, TIMEOUT_MS)) { drop_connection("payload read failed"); continue; }

		gettimeofday(&nl.last_rx, NULL);

		if (hdr.cmd == CMD_PAUSE || hdr.cmd == CMD_RESUME) {
			pthread_mutex_lock(&nl.lock);
			nl.peer_paused = (hdr.cmd == CMD_PAUSE);
			if (nl.peer_paused) gettimeofday(&nl.peer_paused_at, NULL);
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
			nl.inputs[slot].frame = f;
			nl.inputs[slot].buttons = b;
			nl.inputs[slot].valid = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_STATE && size > 8) {
			uint32_t total = ntohl(*(uint32_t*)buf);
			uint32_t off   = ntohl(*(uint32_t*)(buf + 4));
			size_t   n     = size - 8;

			pthread_mutex_lock(&nl.lock);
			bool valid = total != 0 && total <= NETLINK_MAX_STATE &&
			             off <= total && n <= total - off;
			if (!valid) {
				nl_log("bad state chunk (total=%u off=%u n=%zu) - ignoring\n", total, off, n);
			} else if (!nl.state_buf || nl.state_len != total || off == 0) {
				free(nl.state_buf);
				nl.state_buf = malloc(total);
				nl.state_len = total;
				nl.state_have = 0;
				nl.state_ready = false;
			}
			if (valid && nl.state_buf && nl.state_len == total && off == nl.state_have) {
				memcpy(nl.state_buf + off, buf + 8, n);
				nl.state_have += n;
				if (nl.state_have >= total) nl.state_ready = true;
			} else if (valid && nl.state_buf && off != nl.state_have) {
				nl_log("out-of-order state chunk (wanted=%zu got=%u) - discarding\n",
				       nl.state_have, off);
				free(nl.state_buf);
				nl.state_buf = NULL;
				nl.state_len = nl.state_have = 0;
				nl.state_ready = false;
			}
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
				if (nl.peer_hash_count == HASH_RING) {
					nl.peer_hash_head = (nl.peer_hash_head + 1) % HASH_RING;
					nl.peer_hash_count--;
					nl_log("state-hash queue full - dropped oldest checkpoint\n");
				}
				unsigned slot = (nl.peer_hash_head + nl.peer_hash_count) % HASH_RING;
				nl.peer_hashes[slot].frame = hf;
				nl.peer_hashes[slot].hash = hv;
			nl.peer_hash_count++;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RESYNC_REQUEST && size == 4) {
			pthread_mutex_lock(&nl.lock);
			nl.resync_request_frame = ntohl(*(uint32_t*)buf);
			nl.resync_request_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RESYNC_BEGIN && size == 12) {
			pthread_mutex_lock(&nl.lock);
			nl.resync_begin_epoch = ntohl(*(uint32_t*)buf);
			nl.resync_begin_frame = ntohl(*(uint32_t*)(buf + 4));
			uint32_t kind = ntohl(*(uint32_t*)(buf + 8));
			nl.resync_begin_kind = kind == NETLINK_RECOVERY_RESET
			                     ? NETLINK_RECOVERY_RESET : NETLINK_RECOVERY_SYNC;
			nl.resync_begin_ready = true;
			/* BEGIN owns the state stream which follows it. */
			free(nl.state_buf);
			nl.state_buf = NULL;
			nl.state_len = nl.state_have = 0;
			nl.state_ready = false;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RESYNC_ACK && size == 8) {
			pthread_mutex_lock(&nl.lock);
			nl.resync_ack_epoch = ntohl(*(uint32_t*)buf);
			nl.resync_ack_loaded = ntohl(*(uint32_t*)(buf + 4)) != 0;
			nl.resync_ack_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_RESYNC_COMMIT && size == 4) {
			pthread_mutex_lock(&nl.lock);
			nl.resync_commit_epoch = ntohl(*(uint32_t*)buf);
			nl.resync_commit_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_SESSION_IDENTITY && size == 56) {
			pthread_mutex_lock(&nl.lock);
			memcpy(nl.peer_session_identity.rom_sha256, buf, 32);
			nl.peer_session_identity.mode = ntohl(*(uint32_t*)(buf + 32));
			nl.peer_session_identity.input_delay = ntohl(*(uint32_t*)(buf + 36));
			nl.peer_session_identity.core_identity = ntohl(*(uint32_t*)(buf + 40));
			nl.peer_session_identity.state_size = ntohl(*(uint32_t*)(buf + 44));
			nl.peer_session_identity.sram_size = ntohl(*(uint32_t*)(buf + 48));
			nl.peer_session_identity.rtc_size = ntohl(*(uint32_t*)(buf + 52));
			nl.peer_session_identity_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_CHECKPOINT_ACK && size == 12) {
			pthread_mutex_lock(&nl.lock);
			nl.checkpoint_ack_frame = ntohl(*(uint32_t*)buf);
			nl.checkpoint_ack_hash = ntohl(*(uint32_t*)(buf + 4));
			nl.checkpoint_ack_matched = ntohl(*(uint32_t*)(buf + 8)) != 0;
			nl.checkpoint_ack_ready = true;
			pthread_mutex_unlock(&nl.lock);
		}

		if (hdr.cmd == CMD_DATA && size) {
			pthread_mutex_lock(&nl.lock);
			nl.rx_count++;
			queue_push(buf, size);
			pthread_mutex_unlock(&nl.lock);
		}
		/* CMD_PING needs no handling beyond refreshing last_rx above. */
	}

	drop_connection("worker stopping");
	return NULL;
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
	if (nl.fd >= 0)        { close(nl.fd);        nl.fd = -1; }
	nl.connected = false;

	if (nl.dropped) nl_log("%u packet(s) were dropped this session\n", nl.dropped);
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
	bool p = nl.peer_paused;
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
	bool c = nl.connected;
	pthread_mutex_unlock(&nl.lock);
	return c;
}

bool NetLink_consumeConnectEvent(void) {
	pthread_mutex_lock(&nl.lock);
	bool e = nl.connect_event;
	nl.connect_event = false;
	pthread_mutex_unlock(&nl.lock);
	return e;
}

bool NetLink_consumeDisconnectEvent(void) {
	pthread_mutex_lock(&nl.lock);
	bool e = nl.disconnect_event;
	nl.disconnect_event = false;
	pthread_mutex_unlock(&nl.lock);
	return e;
}

uint32_t NetLink_connectionGeneration(void) {
	pthread_mutex_lock(&nl.lock);
	uint32_t generation = nl.connection_generation;
	pthread_mutex_unlock(&nl.lock);
	return generation;
}

//////////////////////////////////////////////////////////////////////////////
// shared-screen netplay
//////////////////////////////////////////////////////////////////////////////

static void reset_timeline_locked(void) {
	memset(nl.inputs, 0, sizeof(nl.inputs));
	nl.peer_hash_head = nl.peer_hash_count = 0;
	nl.checkpoint_ack_ready = false;
}

void NetLink_resetTimeline(void) {
	pthread_mutex_lock(&nl.lock);
	reset_timeline_locked();
	pthread_mutex_unlock(&nl.lock);
}

void NetLink_resetSync(void) {
	pthread_mutex_lock(&nl.lock);
	reset_timeline_locked();
	free(nl.state_buf);
	nl.state_buf = NULL;
	nl.state_len = nl.state_have = 0;
	nl.state_ready = false;
	nl.resync_request_ready = nl.resync_begin_ready = false;
	nl.resync_ack_ready = nl.resync_commit_ready = false;
	pthread_mutex_unlock(&nl.lock);
}

/* Every framed send has the same failure semantics. A partial control/state
 * transaction cannot be repaired on the same byte stream; close it and let the
 * next connection generation start from a clean identity/state handshake. */
static bool send_command(uint8_t cmd, const void* data, size_t len) {
	pthread_mutex_lock(&nl.lock);
	bool connected = nl.connected;
	bool ok = connected && send_framed(cmd, data, len, NetLink_localClientId());
	pthread_mutex_unlock(&nl.lock);
	if (!ok && connected) drop_connection("protocol send failed");
	return ok;
}

bool NetLink_sendInput(uint32_t frame, uint32_t buttons) {
	uint32_t payload[2] = { htonl(frame), htonl(buttons) };
	return send_command(CMD_INPUT, payload, sizeof(payload));
}

bool NetLink_getRemoteInput(uint32_t frame, uint32_t* buttons) {
	pthread_mutex_lock(&nl.lock);
	unsigned slot = frame % INPUT_RING;
	bool ok = nl.inputs[slot].valid && nl.inputs[slot].frame == frame;
	if (ok && buttons) *buttons = nl.inputs[slot].buttons;
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_sendState(const void* data, size_t len) {
	const uint8_t* p = data;
	for (size_t off = 0; off < len; off += STATE_CHUNK) {
		size_t n = len - off;
		if (n > STATE_CHUNK) n = STATE_CHUNK;

		uint8_t pkt[8 + STATE_CHUNK];
		uint32_t hdr[2] = { htonl((uint32_t)len), htonl((uint32_t)off) };
		memcpy(pkt, hdr, 8);
		memcpy(pkt + 8, p + off, n);

		bool ok = send_command(CMD_STATE, pkt, n + 8);
		if (!ok) return false;
	}
	return true;
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
		bool ok = nl.connected && send_framed(CMD_CORE, pkt, n + 8, NetLink_localClientId());
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

bool NetLink_takeState(void** data, size_t* len) {
	pthread_mutex_lock(&nl.lock);
	if (!nl.state_ready) { pthread_mutex_unlock(&nl.lock); return false; }
	*data = nl.state_buf;
	*len  = nl.state_len;
	nl.state_buf = NULL;
	nl.state_len = nl.state_have = 0;
	nl.state_ready = false;
	pthread_mutex_unlock(&nl.lock);
	return true;
}

bool NetLink_sendHash(uint32_t frame, uint32_t hash) {
	uint32_t payload[2] = { htonl(frame), htonl(hash) };
	return send_command(CMD_HASH, payload, sizeof(payload));
}

bool NetLink_sendSessionIdentity(const NetLinkSessionIdentity* identity) {
	uint8_t wire[56];
	memcpy(wire, identity->rom_sha256, 32);
	uint32_t v = htonl(identity->mode); memcpy(wire + 32, &v, 4);
	v = htonl(identity->input_delay); memcpy(wire + 36, &v, 4);
	v = htonl(identity->core_identity); memcpy(wire + 40, &v, 4);
	v = htonl(identity->state_size); memcpy(wire + 44, &v, 4);
	v = htonl(identity->sram_size); memcpy(wire + 48, &v, 4);
	v = htonl(identity->rtc_size); memcpy(wire + 52, &v, 4);
	return send_command(CMD_SESSION_IDENTITY, wire, sizeof(wire));
}

bool NetLink_takeSessionIdentity(NetLinkSessionIdentity* identity) {
	pthread_mutex_lock(&nl.lock);
	bool ok = nl.peer_session_identity_ready;
	if (ok) { *identity = nl.peer_session_identity; nl.peer_session_identity_ready = false; }
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_ackCheckpoint(uint32_t frame, uint32_t hash, bool matched) {
	uint32_t wire[3] = { htonl(frame), htonl(hash), htonl(matched ? 1u : 0u) };
	return send_command(CMD_CHECKPOINT_ACK, wire, sizeof(wire));
}

bool NetLink_takeCheckpointAck(uint32_t* frame, uint32_t* hash, bool* matched) {
	pthread_mutex_lock(&nl.lock);
	bool ok = nl.checkpoint_ack_ready;
	if (ok) {
		*frame = nl.checkpoint_ack_frame;
		*hash = nl.checkpoint_ack_hash;
		*matched = nl.checkpoint_ack_matched;
		nl.checkpoint_ack_ready = false;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_takeHash(uint32_t* frame, uint32_t* hash) {
	pthread_mutex_lock(&nl.lock);
	bool ok = nl.peer_hash_count != 0;
	if (ok) {
		*frame = nl.peer_hashes[nl.peer_hash_head].frame;
		*hash  = nl.peer_hashes[nl.peer_hash_head].hash;
		nl.peer_hash_head = (nl.peer_hash_head + 1) % HASH_RING;
		nl.peer_hash_count--;
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
	bool ok = nl.resync_request_ready;
	if (ok) { *frame = nl.resync_request_frame; nl.resync_request_ready = false; }
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_takeResyncBegin(uint32_t* epoch, uint32_t* resume_frame, NetLinkRecoveryKind* kind) {
	pthread_mutex_lock(&nl.lock);
	bool ok = nl.resync_begin_ready;
	if (ok) {
		*epoch = nl.resync_begin_epoch;
		*resume_frame = nl.resync_begin_frame;
		*kind = nl.resync_begin_kind;
		nl.resync_begin_ready = false;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_takeResyncAck(uint32_t* epoch, bool* loaded) {
	pthread_mutex_lock(&nl.lock);
	bool ok = nl.resync_ack_ready;
	if (ok) {
		*epoch = nl.resync_ack_epoch;
		*loaded = nl.resync_ack_loaded;
		nl.resync_ack_ready = false;
	}
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_takeResyncCommit(uint32_t* epoch) {
	pthread_mutex_lock(&nl.lock);
	bool ok = nl.resync_commit_ready;
	if (ok) { *epoch = nl.resync_commit_epoch; nl.resync_commit_ready = false; }
	pthread_mutex_unlock(&nl.lock);
	return ok;
}

bool NetLink_send(int flags, const void* buf, size_t len, uint16_t client_id) {
	(void)flags;     /* TCP_NODELAY means every packet is already flushed. */
	(void)client_id; /* Two players: the only destination is the peer. */

	if (!buf || !len) return true; /* flush-only request */
	if (len > NETLINK_MAX_PACKET) {
		nl_log("refusing to send %zu bytes (max %d)\n", len, NETLINK_MAX_PACKET);
		return false;
	}

	return send_command(CMD_DATA, buf, len);
}

bool NetLink_popPacket(void* out, size_t out_cap, size_t* out_len) {
	pthread_mutex_lock(&nl.lock);
	if (nl.q_head == nl.q_tail) {
		pthread_mutex_unlock(&nl.lock);
		return false;
	}

	QueuedPacket* p = &nl.queue[nl.q_head];
	size_t n = p->len < out_cap ? p->len : out_cap;
	memcpy(out, p->data, n);
	*out_len = n;
	nl.q_head = (nl.q_head + 1) % QUEUE_SIZE;

	pthread_mutex_unlock(&nl.lock);
	return true;
}

unsigned NetLink_droppedPackets(void) { return nl.dropped; }
