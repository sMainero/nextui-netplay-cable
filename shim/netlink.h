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

/* Upper bound on a peer-declared state size. Generous - picodrive's is 678KB -
 * but finite, so a malformed or hostile header cannot ask for a 4GB malloc. */
#define NETLINK_MAX_STATE    (16u * 1024u * 1024u)

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

/* Increments after every successful TCP greeting. The emulator thread uses it
 * to distinguish a replacement process from the peer it originally synced. */
uint32_t NetLink_connectionGeneration(void);

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

/* --- shared-screen netplay ---------------------------------------------
 *
 * One instance per device running the same game in lockstep, each player
 * owning a controller port. Only inputs cross the wire, buffered a few frames
 * ahead so the peer's arrive before they are needed - which is what makes this
 * tolerate latency that a synchronous link cable cannot.
 */

/* Inputs are addressed by frame number, so a late packet is still usable and a
 * duplicate is harmless. */
bool NetLink_sendInput(uint32_t frame, uint32_t buttons);
bool NetLink_getRemoteInput(uint32_t frame, uint32_t* buttons);
/* A new connection needs the full reset. Moving to a committed authoritative
 * timeline must retain ACK/COMMIT controls which may already be in flight. */
void NetLink_resetSync(void);
void NetLink_resetTimeline(void);

/* Both sides must start from bit-identical state, so the host ships one.
 * Chunked: a save state is far larger than a packet. */
bool NetLink_sendState(const void* data, size_t len);
bool NetLink_takeState(void** data, size_t* len);  /* caller frees */

/* Periodic agreement check. Without it a divergence is silent and the two
 * games quietly tell different stories. */
bool NetLink_sendHash(uint32_t frame, uint32_t hash);
bool NetLink_takeHash(uint32_t* frame, uint32_t* hash);

typedef struct {
	uint8_t  rom_sha256[32];
	uint32_t mode;          /* 1 = shared-screen; link identity follows separately */
	uint32_t input_delay;   /* timeline priming must agree exactly */
	uint32_t core_identity;
	uint32_t state_size;
	uint32_t sram_size;
	uint32_t rtc_size;
} NetLinkSessionIdentity;

/* Sent afresh on each TCP connection, before any serialized state is accepted. */
bool NetLink_sendSessionIdentity(const NetLinkSessionIdentity* identity);
bool NetLink_takeSessionIdentity(NetLinkSessionIdentity* identity);

/* The guest confirms the result of each periodic comparison. The host only
 * promotes the candidate snapshot after a matching acknowledgement. */
bool NetLink_ackCheckpoint(uint32_t frame, uint32_t hash, bool matched);
bool NetLink_takeCheckpointAck(uint32_t* frame, uint32_t* hash, bool* matched);

/* Authoritative-state recovery. The client requests it after a confirmed hash
 * mismatch. The host brackets a state transfer with BEGIN, waits for the
 * client's load ACK, then releases both timelines with COMMIT. Epochs make a
 * delayed control packet from an abandoned attempt harmless. */
bool NetLink_requestResync(uint32_t frame);
bool NetLink_takeResyncRequest(uint32_t* frame);
typedef enum {
	NETLINK_RECOVERY_SYNC = 0,
	NETLINK_RECOVERY_RESET = 1,
} NetLinkRecoveryKind;
bool NetLink_beginResync(uint32_t epoch, uint32_t resume_frame, NetLinkRecoveryKind kind);
bool NetLink_takeResyncBegin(uint32_t* epoch, uint32_t* resume_frame, NetLinkRecoveryKind* kind);
bool NetLink_ackResync(uint32_t epoch, bool loaded);
bool NetLink_takeResyncAck(uint32_t* epoch, bool* loaded);
bool NetLink_commitResync(uint32_t epoch);
bool NetLink_takeResyncCommit(uint32_t* epoch);

/* --- core sharing -------------------------------------------------------
 *
 * Two devices can only share a screen if their cores emulate identically, and
 * across NextUI releases that is a coin toss: pcsx_rearmed changed behaviour
 * seven times in six months. Rather than demand matching installs, the client
 * can adopt the host's core file for the session.
 *
 * Viable because build provenance does not affect emulation - only source
 * revision does. A tg5050-built fceumm and a tg5040-built one of the same
 * revision produce identical state despite different toolchains, sizes and
 * CRCs. Verified by loading three platforms' cores on one device.
 *
 * The hard limit is architecture: a 32-bit ARM device cannot load an AArch64
 * object. That is checked before any transfer is attempted.
 */
#if 0 /* Frozen: never accept executable code from an unauthenticated peer. */
#define NETLINK_MAX_CORE (24u * 1024u * 1024u)

/* Identifies a build well enough to know whether two differ. Machine is the
 * ELF e_machine, so an impossible pairing is refused rather than transferred
 * and then failed at dlopen. */
#define NETLINK_VERSION_LEN 32

typedef struct {
	uint32_t crc;      /* of the core file */
	uint32_t size;
	uint16_t machine;  /* ELF e_machine: 40 = ARM, 183 = AArch64 */
	uint8_t  can_send; /* sharing enabled on this side */

	/* library_version, e.g. "(SVN) afe65ef" - the source revision.
	 *
	 * A far better predictor than the CRC. Two platforms building the same
	 * revision produce different files, and measurement says they emulate
	 * identically: of six shipped fceumm builds the CRC gives six groups, the
	 * version five, and actual behaviour three. Matching on CRC alone would
	 * transfer 3MB between two builds that already agree. */
	char     version[NETLINK_VERSION_LEN];

	/* Whether the *other* device could load this file, encoded major*1000+minor:
	 * core_glibc is what the core requires, runtime_glibc what the device
	 * provides. A tg5050 build needs 2.33 where every other platform needs
	 * 2.17, so the host's core is not automatically the loadable one. */
	uint32_t core_glibc;
	uint32_t runtime_glibc;
} NetLinkCoreId;

/* Set before NetLink_start; exchanged during the handshake. */
void NetLink_setCoreId(const NetLinkCoreId* id);
/* The peer's, once connected. False until the handshake completes. */
bool NetLink_peerCoreId(NetLinkCoreId* out);

bool NetLink_sendCore(const void* data, size_t len);
bool NetLink_takeCore(void** data, size_t* len);   /* caller frees */
#endif

/* Copy the oldest queued packet out. False when the queue is empty. */
bool NetLink_popPacket(void* out, size_t out_cap, size_t* out_len);

/* Number of packets dropped because the queue was full - a desync warning
 * sign worth logging rather than hiding. */
unsigned NetLink_droppedPackets(void);

#endif
