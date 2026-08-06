/*
 * netlink - the two-player TCP transport under the shim's netpacket interface.
 *
 * The frontend knows nothing about any of this. The core asks for a netpacket
 * interface, the shim answers, and this is what sits underneath: host listens,
 * client connects, packets are framed over TCP.
 *
 * Receiving happens on its own thread, which is what lets a link survive the
 * frontend blocking - a menu, a sleep, a long save. That is the reason the
 * patched minarch needed hooks in Menu_loop and the sleep path; here it costs
 * nothing.
 */

#ifndef NETLINK_H
#define NETLINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETLINK_DEFAULT_PORT 55437
#define NETLINK_MAX_PACKET   2048

typedef enum {
	NETLINK_ROLE_NONE = 0,
	NETLINK_ROLE_HOST,
	NETLINK_ROLE_CLIENT,
} NetLinkRole;

/* Read a session description. Returns false when no session is configured,
 * which is the normal passthrough case and not an error.
 *
 * Format is one key=value per line:
 *   role=host|client
 *   port=55437        (optional)
 *   peer=192.168.1.42 (clients only)
 */
bool NetLink_configure(const char* session_path);

bool NetLink_isConfigured(void);
NetLinkRole NetLink_getRole(void);

/* Bring the link up. Non-blocking: connecting happens on the worker thread, so
 * the first frames run before a peer is present. */
bool NetLink_start(void);
void NetLink_stop(void);

bool NetLink_isConnected(void);

/* libretro netpacket assigns the host client_id 0 and clients ids above it.
 * Two players, so the remote is simply the other one. */
uint16_t NetLink_localClientId(void);
uint16_t NetLink_remoteClientId(void);

/* Edge-triggered: each transition is reported exactly once, so the caller can
 * drive the core's start/connected and stop/disconnected callbacks. */
bool NetLink_consumeConnectEvent(void);
bool NetLink_consumeDisconnectEvent(void);

bool NetLink_send(int flags, const void* buf, size_t len, uint16_t client_id);

/* Called once per frame from retro_run. When the frontend stops calling it -
 * a menu, a sleep, a long save - we tell the peer, so it can stall its own core
 * instead of running ahead and filling a buffer that nobody is draining.
 *
 * Deliberately not inferred from "no game data received": a turn-based game
 * legitimately goes quiet while a player thinks, and pausing then would be
 * wrong. Frontend liveness is the honest signal, and only we can observe it. */
void NetLink_markFrame(void);

/* Bracket the call into the core. Time spent inside it is not a frontend stall,
 * however long it lasts: a link-capable core may legitimately block there
 * waiting on its peer. Reporting that as a stall makes the peer pause, which
 * removes the very data the blocked core is waiting for - a deadlock. */
void NetLink_setCoreRunning(bool running);

/* Why we are asking the peer to hold. Only a frontend that has stopped calling
 * retro_run counts: a core blocked *inside* retro_run is working, not absent,
 * and pausing its peer would starve it of what it is waiting for. */
typedef enum {
	NETLINK_PAUSE_FRONTEND = 0, /* menu, sleep, long save */
} NetLinkPauseReason;

/* True while the peer has told us it is stalled. The caller should skip running
 * the core; returning from retro_run without advancing is a legal frame skip
 * and keeps the frontend responsive. */
bool NetLink_isPeerPaused(void);

/* Copy the oldest queued packet out. False when the queue is empty. */
bool NetLink_popPacket(void* out, size_t out_cap, size_t* out_len);

/* Number of packets dropped because the queue was full - a desync warning
 * sign worth logging rather than hiding. */
unsigned NetLink_droppedPackets(void);

#endif
