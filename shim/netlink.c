#include "netlink.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define NETLINK_MAGIC    0x4E504C4BU /* 'NPLK' */
#define NETLINK_PROTOCOL 1

#define QUEUE_SIZE    64
#define HEARTBEAT_MS  1000
#define TIMEOUT_MS    5000
#define ACCEPT_POLL_MS 100

enum {
	CMD_HELLO = 0x00,
	CMD_DATA  = 0x01,
	CMD_PING  = 0x02,
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

	bool connect_event;
	bool disconnect_event;

	struct timeval last_rx;
	struct timeval last_tx;
} nl;

static void nl_log(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fputs("[netlink] ", stderr);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

//////////////////////////////////////////////////////////////////////////////
// helpers
//////////////////////////////////////////////////////////////////////////////

static long ms_since(const struct timeval* then) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - then->tv_sec) * 1000L + (now.tv_usec - then->tv_usec) / 1000L;
}

static bool write_all(int fd, const void* buf, size_t len) {
	const uint8_t* p = buf;
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n > 0) { p += n; len -= (size_t)n; continue; }
		if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
		return false;
	}
	return true;
}

/* Blocking read of exactly len bytes, with a deadline so a half-open peer
 * cannot wedge the worker thread. */
static bool read_all(int fd, void* buf, size_t len, int timeout_ms) {
	uint8_t* p = buf;
	struct timeval start;
	gettimeofday(&start, NULL);

	while (len) {
		ssize_t n = recv(fd, p, len, 0);
		if (n > 0) { p += n; len -= (size_t)n; continue; }
		if (n == 0) return false; /* peer closed */
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			if (timeout_ms >= 0 && ms_since(&start) > timeout_ms) return false;
			usleep(1000);
			continue;
		}
		return false;
	}
	return true;
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
	NetLinkHello mine = { .magic = htonl(NETLINK_MAGIC), .protocol = htonl(NETLINK_PROTOCOL) };
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

	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
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

static void drop_connection(void) {
	pthread_mutex_lock(&nl.lock);
	if (nl.fd >= 0) { close(nl.fd); nl.fd = -1; }
	if (nl.connected) {
		nl.connected = false;
		nl.disconnect_event = true;
		nl_log("peer lost\n");
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
			nl.connected = true;
			nl.connect_event = true;
			gettimeofday(&nl.last_rx, NULL);
			gettimeofday(&nl.last_tx, NULL);
			pthread_mutex_unlock(&nl.lock);

			nl_log("connected as %s (client_id %u)\n",
			       nl.role == NETLINK_ROLE_HOST ? "host" : "client",
			       NetLink_localClientId());
			continue;
		}

		/* --- connected: read one frame --- */
		NetLinkHeader hdr;
		ssize_t n = recv(nl.fd, &hdr, sizeof(hdr), MSG_PEEK);
		if (n == 0) { drop_connection(); continue; }
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				/* Idle. Heartbeat so the peer knows we are alive, and give up
				 * if we have heard nothing for too long - gpSP in particular
				 * needs a prompt disconnect to unstick the RFU state machine. */
				if (ms_since(&nl.last_rx) > TIMEOUT_MS) {
					nl_log("timed out after %dms of silence\n", TIMEOUT_MS);
					drop_connection();
					continue;
				}
				if (ms_since(&nl.last_tx) > HEARTBEAT_MS) {
					pthread_mutex_lock(&nl.lock);
					if (!send_framed(CMD_PING, NULL, 0, NetLink_localClientId())) {
						pthread_mutex_unlock(&nl.lock);
						drop_connection();
						continue;
					}
					pthread_mutex_unlock(&nl.lock);
				}
				usleep(500);
				continue;
			}
			drop_connection();
			continue;
		}

		if (!read_all(nl.fd, &hdr, sizeof(hdr), TIMEOUT_MS)) { drop_connection(); continue; }

		uint16_t size = ntohs(hdr.size);
		if (size > NETLINK_MAX_PACKET) {
			nl_log("oversized packet (%u bytes), dropping link\n", size);
			drop_connection();
			continue;
		}

		uint8_t buf[NETLINK_MAX_PACKET];
		if (size && !read_all(nl.fd, buf, size, TIMEOUT_MS)) { drop_connection(); continue; }

		gettimeofday(&nl.last_rx, NULL);

		if (hdr.cmd == CMD_DATA && size) {
			pthread_mutex_lock(&nl.lock);
			queue_push(buf, size);
			pthread_mutex_unlock(&nl.lock);
		}
		/* CMD_PING needs no handling beyond refreshing last_rx above. */
	}

	drop_connection();
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

bool NetLink_send(int flags, const void* buf, size_t len, uint16_t client_id) {
	(void)flags;     /* TCP_NODELAY means every packet is already flushed. */
	(void)client_id; /* Two players: the only destination is the peer. */

	if (!buf || !len) return true; /* flush-only request */
	if (len > NETLINK_MAX_PACKET) {
		nl_log("refusing to send %zu bytes (max %d)\n", len, NETLINK_MAX_PACKET);
		return false;
	}

	pthread_mutex_lock(&nl.lock);
	bool ok = nl.connected && send_framed(CMD_DATA, buf, len, NetLink_localClientId());
	pthread_mutex_unlock(&nl.lock);

	if (!ok && nl.connected) drop_connection();
	return ok;
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
