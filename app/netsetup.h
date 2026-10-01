/*
 * Session setup: discovery, session files, and the pre-arm safety checks.
 *
 * Deliberately separate from the UI so it can be exercised without a screen.
 */

#ifndef NETSETUP_H
#define NETSETUP_H

#include <stdbool.h>
#include <stdint.h>

#define NS_MAX_PEERS 8
#define NS_IP_LEN    24

/* Shared screen (one instance each, inputs synced) versus link cable (the cores
 * carry their own serial/RFU traffic).
 *
 * No longer chosen here: the shim derives it from the core it ends up wrapping
 * (gambatte and gpSP get link cable, everything else gets shared screen), so
 * both sides agree by virtue of running the same ROM. Retained because the
 * discovery announcement still carries the field. */
typedef enum { NS_MODE_NETPLAY, NS_MODE_LINK } NS_Mode;

/* Which medium a session runs over.
 *
 * Replaces a pair of booleans - hotspot on the peer, adhoc at arm time - that
 * could only ever describe the two WiFi transports. The third one is a USB
 * cable, and it is not a variation on either: it moves no radio, it has no
 * association, and its teardown owes the supplicant nothing. A boolean is a
 * question ("did we host?") where what the code needs is an answer ("what is
 * this link?"), and every site that asked the boolean had to re-derive the
 * transport for itself.
 *
 *   WIFI   both devices already on a network; nothing moved
 *   ADHOC  this device serves the network, or joined the peer's
 *   CABLE  point-to-point over USB; no WiFi involvement at all
 */
typedef enum {
	NS_LINK_WIFI = 0,
	NS_LINK_ADHOC,
	NS_LINK_CABLE,
} NS_LinkKind;

#define NS_SSID_LEN 24
#define NS_PSK_LEN  24

typedef struct {
	char    ip[NS_IP_LEN];
	char    platform[16];
	NS_Mode mode;
	NS_LinkKind link;               /* what this peer would be reached over */
	bool    scanned;                /* found by SSID scan, not by announcement */
	char    ssid[NS_SSID_LEN];
	char    psk[NS_PSK_LEN];
} NS_Peer;

/* --- environment ------------------------------------------------------- */

void NS_init(void);                  /* resolve pak path, platform, sd root */
const char* NS_platform(void);
const char* NS_pakPath(void);
const char* NS_statePath(void); /* durable state in .userdata/shared/Netplay */
bool NS_localIP(char* out, int len);

/* --- assumption checks -------------------------------------------------
 *
 * The shim leans on a few NextUI behaviours that are internal details rather
 * than promises. Three of them fail silently if they ever change - the launcher
 * simply never runs and games launch as stock - and one of those silently moves
 * save states. Checking at arm time turns all of that into a message.
 */

typedef enum {
	NS_CHECK_OK = 0,
	NS_CHECK_WARN,
	NS_CHECK_FAIL,
} NS_CheckResult;

typedef struct {
	NS_CheckResult result;
	char label[48];
	char detail[96];
} NS_Check;

/* Returns the number of checks run; worst result via *worst. */
int NS_runChecks(NS_Check* out, int max, NS_CheckResult* worst);

/* --- session ------------------------------------------------------------ */

typedef enum { NS_ROLE_HOST, NS_ROLE_CLIENT } NS_Role;

/* Write session.conf + shared Netplay/session, and install launch stubs.
 * Writes no mode= line - the shim picks the mode from the core. */
bool NS_arm(NS_Role role, const char* peer_ip, char* err, int errlen);

/* Remove session, force flag and stubs. */
void NS_disarm(void);

/* Wi-Fi power save costs tens of milliseconds on the sparse per-frame traffic
 * netplay generates. Held off for as long as a session is armed; the prior
 * value is recorded and put back by NS_disarm. */
void NS_wifiPowerSaveDisable(void);
void NS_wifiPowerSaveRestore(void);

/* Put the platform's own WiFi back. Safe to call at any time. The stranded
 * check runs at startup, NS_wifiRestore is the manual escape hatch for when it
 * does not fire.
 *
 * Three outcomes, not two. This returned a bare "we tried" and the caller read
 * it as "we succeeded", so a device whose restore had just failed - and logged
 * that it failed - was told on screen that it had reconnected. A failure the
 * user is not shown is a failure they cannot act on. */
typedef enum {
	NS_RECOVERY_NONE = 0,   /* nothing was owed */
	NS_RECOVERY_DONE,       /* the client stack is back, with an address */
	NS_RECOVERY_FAILED,     /* attempted; the record is retained for a retry */
} NS_Recovery;

NS_Recovery NS_wifiRecoverIfStranded(void);
void NS_wifiRestore(void);

/* Drop the session, keep the launch stubs. Games launch stock (the shim is a
 * passthrough with no session file) but the next session needs no reinstall. */
void NS_endSession(void);

bool NS_isArmed(void);         /* a session file exists */
bool NS_stubsInstalled(void);  /* launch stubs are in place, session or not */

/* Durable facts used by a newly opened setup app. The session file owns the
 * role/network/host fields; the detached broker contributes liveness and the
 * host's current guest table. */
typedef struct {
	bool    armed;
	NS_Role role;
	NS_LinkKind link;               /* the medium the session file names */
	char    network[NS_SSID_LEN];
	char    psk[NS_PSK_LEN];
	char    host[NS_IP_LEN];
	bool    broker_running;
	int     guest_count;
	struct {
		char id[32];
		char ip[NS_IP_LEN];
	} guest[4];
} NS_SessionInfo;

bool NS_sessionInfo(NS_SessionInfo* out);

/* Remove an active-session record that belongs to a previous device boot.
 * Empty hosts and temporarily absent peers are valid and are never considered
 * stale. Returns true only when a leftover session was actually removed. */
bool NS_cleanupStaleSession(void);

/* Host-only detached owner of discovery and compatibility negotiation. */
bool NS_brokerStart(char* err, int errlen);
void NS_brokerStop(void);

/* --- settings -----------------------------------------------------------
 *
 * Persisted to .userdata/shared/Netplay/settings as key=value. Read once at startup; written on
 * every change, because a handheld can lose power at any moment and a setting
 * that survives only until a clean exit is worse than no setting at all.
 */

/* Instanced cores run both consoles locally and network only the inputs, so the
 * serial link becomes an in-process call. That is only meaningful for link-cable
 * systems, where each player has their own screen. A shared-screen system is
 * already one instance by definition and instancing it would be pure overhead -
 * hence a fixed, short compatibility list rather than "any core". */
#define NS_INST_CORES 2
extern const char* const NS_INST_CORE[NS_INST_CORES];   /* gambatte, mgba */

typedef enum {
	NS_INST_OFF = 0,     /* never */
	NS_INST_ALL,         /* every compatible core that is installed */
	NS_INST_SELECTED,    /* only the ones ticked below */
} NS_InstMode;

typedef struct {
	bool        share_cores;    /* frozen: retained so the setting can return later */
	bool        compatibility_cores; /* fall back to the pak's matched core set */
	bool        force_compatibility; /* testing: choose packaged builds even if installed match */
	bool        verbose_logs; /* retain a complete immutable log for each game launch */
	bool        simple_client;  /* planned: accept an invitation for a matching local game */
	bool        add_gameswitcher; /* expose the Netplay quick app in NextUI recents/switcher */
	NS_InstMode instanced;
	bool        inst_core[NS_INST_CORES];
} NS_Settings;

NS_Settings* NS_settings(void);   /* live, mutable; call NS_settingsSave after */
void NS_settingsLoad(void);
void NS_settingsSave(void);

/* Is this core present anywhere we would load it from? Used to grey out an
 * instanced-core choice rather than offer something that cannot run. */
bool NS_coreInstalled(const char* core);

typedef struct {
	bool installed;
	bool managed;
	bool missing;
	bool mismatch;
	bool mismatch_ignored;
	char backup_date[32];
} NS_MgbaStatus;

bool NS_mgbaStatus(NS_MgbaStatus* out);
bool NS_mgbaInstall(char* err, int errlen);

/* Arm-time convenience, and deliberately the narrow one: installs this build's
 * mGBA pak only when the device has none at all. A device that already carries
 * one - stock or third-party - is never touched, because a session runs the
 * pak's own cores/override/<platform>/mgba_libretro.so anyway and does not need
 * to own the emulator. `installed` reports which of the two happened; a false
 * return means the check itself failed. Logs its own outcome, so a caller that
 * only wants the side effect can pass NULL/NULL/0. */
bool NS_mgbaEnsure(bool* installed, char* err, int errlen);
bool NS_mgbaIgnoreMismatch(char* err, int errlen);
bool NS_mgbaRestore(bool pak, bool saves, bool states, char* err, int errlen);

/* --- core manifest ------------------------------------------------------
 *
 * What this device could bring to a session. Built at arm time so the peer's
 * installed and packaged builds are known before a game is picked.
 *
 * Identity is the source revision (library_version), not the file CRC: two
 * platforms building the same commit produce different files that may emulate
 * identically, so CRC alone is not a useful compatibility verdict.
 */
#define NS_MAX_MANIFEST 12

typedef struct {
	char     core[32];       /* "fceumm" */
	char     version[32];    /* library_version, e.g. "(SVN) afe65ef" */
	char     path[256];
	uint32_t crc;
	uint32_t size;
	uint16_t machine;        /* ELF e_machine: 40 ARM, 183 AArch64 */
	uint32_t glibc;          /* highest GLIBC_x.y required, major*1000+minor */
	bool     installed;
	char     compat_version[32]; /* pak fallback's source/build identity */
	bool     compat_available;
} NS_CoreInfo;

/* Fills out with every netplay-capable core installed here. Costs one dlopen
 * per core - retro_get_system_info is callable before retro_init, so no ROM is
 * needed. Returns the count. */
int NS_coreManifest(NS_CoreInfo* out, int max);

/* What this device can load, major*1000+minor. */
uint32_t NS_runtimeGlibc(void);

/* --- core compatibility -------------------------------------------------
 *
 * The devices compare installed-core manifests at arm time. Matching installed
 * builds remain first choice. When they differ and both devices have the same
 * pak compatibility build, both session files select that local fallback.
 * No executable code crosses the network.
 */
#define NS_CORE_PORT 55439

/* Host broker: accept and serve one exchange if a client is waiting.
 * Non-blocking accept keeps the idle detached process inexpensive. */
bool NS_compatServeStart(void);
void NS_compatServeTick(void);
void NS_compatServeStop(void);
const char* NS_compatLastPeer(void);

/* Client: compare manifests with the host. Returns the number of compatibility
 * fallbacks selected on both devices, or -1 on failure. */
int NS_compatSync(const char* host_ip, char* err, int errlen);

/* --- discovery ---------------------------------------------------------- */

/* Host: announce presence so clients can find us. Safe to call repeatedly. */
/* Ad hoc networking.
 *
 * Negotiation happens over whatever network both devices are already on: the
 * host advertises the credentials it *will* use, the client picks it up during
 * discovery, and only then do both move. That avoids typing an SSID on a
 * handheld and avoids the chicken-and-egg of needing the new network to learn
 * about the new network.
 *
 * The host's address on its own network is fixed (NS_HOTSPOT_HOST_IP), so the
 * client knows the peer address before it switches.
 */
#define NS_HOTSPOT_HOST_IP "10.0.0.1"

/* Fixed credentials. Measured 1.7/4.3/21.5ms min/avg/max over this link versus
 * 6.5/26-43/118-300ms through an access point - one hop instead of two. That
 * worst case sits far inside the shim's input-delay window, which the
 * two-hop path did not. */
#define NS_ADHOC_PREFIX "nextui"
/* Legacy fixed name, still accepted when joining so an older host is reachable. */
#define NS_ADHOC_SSID   "nextui-netplay"
#define NS_ADHOC_PSK  "playwithme"

/* Join retry shape. Each attempt is bounded so five failures cannot take
 * minutes with nothing on screen. */
#define NS_JOIN_ATTEMPTS 5
#define NS_JOIN_GAP_S    3
#define NS_JOIN_ASSOC_S  8
/* Seconds to wait for a lease, per request, of which two are made per attempt.
 * Polled rather than delegated to the client's own retry budget: `udhcpc -t 6`
 * multiplies its -T pause straight into wall clock (~18s a call, 36s an
 * attempt) and a client that is not installed at all returns instantly. */
#define NS_JOIN_DHCP_S   6

/* Frames of input lag traded for tolerance of a late peer. Written into both
 * session files from here rather than left to the shim's default, because the
 * two devices must use the same number: each side substitutes neutral input for
 * its own first input_delay frames, so a mismatch diverges during priming and
 * never recovers.
 *
 * Two values, because the two transports are not remotely comparable and a
 * single constant tuned for one is unusable on the other. Measured RTT:
 *
 *   ad hoc, one hop   1.7 / 4.3 / 21.5 ms   (min/avg/max)
 *   via access point  6.2 / 26-43 / 118-300 ms
 *
 * 3 frames is 50ms, roughly twice the ad hoc worst case. Applying that same 3
 * over the access point gave 33-47fps and 63-74% stalled frames, because a
 * 50ms budget cannot absorb a 300ms spike. 10 frames is 166ms, which covers the
 * typical access-point maximum but not its worst - the honest position is that
 * lockstep over an access point is marginal at any delay, and ad hoc is the
 * transport this is built for. */
#define NS_INPUT_DELAY_ADHOC 3
#define NS_INPUT_DELAY_WIFI  10

/* One hop over a cable, no radio and no beacon. Kept as its own constant rather
 * than aliasing the ad-hoc value: the two are not the same link, and a value
 * tuned for one transport being reused for the other is exactly the mistake the
 * two constants above were split to fix. Floor only - the shim may raise it from
 * the measured round trip and never lowers it.
 *
 * **One frame, not three.** The design chose 3 from the ad-hoc measurements
 * (1.7 / 4.3 / 21.5 ms min/avg/max) as the closest available analogue, because
 * the cable had not been measured yet. It has now: `ping` across the cable, two
 * Bricks, 4/4 packets both directions, **0.329 / 0.557 / 0.661 ms**. One tenth of
 * the ad-hoc average, with no access point and no second hop, so the jitter that
 * made 3 right there does not exist here - and 3 frames is ~50 ms of deliberate
 * input lag on a link whose round trip is half a millisecond, which is felt.
 *
 * 1 (16.7 ms) is the floor the shim can still raise if a measurement ever asks
 * for more; it never lowers it, so this is the value that decides how the cable
 * feels. The shim's own parser accepts 1..20, so 1 is the lowest this can be. */
#define NS_INPUT_DELAY_CABLE 1

/* --- link kind --------------------------------------------------------- */

/* Cable link. Point-to-point, so both addresses are fixed and neither side
 * needs DHCP, a lease, or an address to discover - the same trick the ad-hoc
 * host already uses with NS_HOTSPOT_HOST_IP, so the client knows its peer
 * before the link is up.
 *
 * Deliberately not 10.0.0.0/24: that is the ad-hoc subnet, and a device that
 * hosted and then did not fully release the interface has collided with it
 * before. Both previous transport additions in this project produced an address
 * or interface collision within two days of landing. */
#define NS_CABLE_HOST_IP   "10.77.0.1"
#define NS_CABLE_CLIENT_IP "10.77.0.2"
#define NS_CABLE_PREFIX    "24"

/* The medium this app is on, or was armed on: the session file when one exists,
 * otherwise the process-local flags. One reader, so the app, the status line and
 * the launcher cannot disagree about which transport a session uses. */
NS_LinkKind NS_linkKind(void);

/* The session-file spelling: "wifi" / "adhoc" / "cable". launcher/state-path.sh
 * reads the same three tokens, so the file format has exactly one vocabulary. */
const char* NS_linkKindName(NS_LinkKind kind);
NS_LinkKind NS_linkKindFromName(const char* name);

/* What to call the transport on screen. Separate from the file spelling above
 * because "adhoc" is not "ad hoc", and the UI says the latter in three places. */
const char* NS_linkKindLabel(NS_LinkKind kind);

/* The input-delay floor this transport gets. */
int NS_inputDelayForKind(NS_LinkKind kind);

/* --- the cable link -----------------------------------------------------
 *
 * The third transport, and the only one this app does not bring up itself: a
 * detached daemon owns the USB gadget (or the enumerating end of it) and the
 * point-to-point interface, and everything below is a question asked about it or
 * a process it owns. Nothing here guesses at the link's state - the daemon is the
 * only thing that can tell a bound gadget with the far end unplugged from a live
 * link, and a port in host mode with nothing on it from one with a peer.
 */

/* Whether this device could ever do a cable link: a controller, a TUN device, a
 * writable configfs and functionfs. Asked before the choice is offered, so a
 * device that cannot do it never shows an option that must fail.
 *
 * Cached, and deliberately not NS_canHostAdhoc's answer: that asks whether there
 * is an AP-capable interface, and a device without one would lose this option
 * with it. */
bool NS_cableSupported(void);

/* Whether the far end of the cable is there, as the daemon last published it.
 *
 * Dynamic and deliberately uncached: the answer changes when someone plugs a
 * cable in, and an answer that outlives live hardware is the failure the address
 * probes in this file have already produced twice. Callers that draw per frame
 * ask it on a cadence, as they already ask about the radio. */
bool NS_cablePeerPresent(void);

/* The daemon is running. Useful separately from the answer above: a cable arm is
 * a cable arm because its daemon is up, which the session file cannot say yet. */
bool NS_cableActive(void);

/* Bring the link up, before the session is armed - the ordering both WiFi
 * transports use, and the reason the daemon takes its role from an argument
 * rather than from the session file. Each reports a reason worth showing and
 * stops whatever it started. */
bool NS_cableHostStart(char* err, int errlen);   /* this device presents the gadget */
bool NS_cableJoin(char* err, int errlen);        /* this device enumerates the peer */

void NS_cableStop(void);

/* Put back what a crashed cable session took from the firmware: the controller,
 * and the port's role if it was forced. Mirrors NS_wifiRecoverIfStranded - the
 * record's existence is the recovery state, and it is removed only once nothing
 * is left owing: either the repair worked, or it turned out there was nothing
 * here to repair. */
NS_Recovery NS_cableRecoverIfStranded(void);

/* The address a peer would dial on this transport. A cable link's addresses are
 * fixed and known before the link exists; every other transport keeps answering
 * with the radio's own address, which is why this sits beside NS_localIP rather
 * than replacing it. */
bool NS_linkIP(NS_LinkKind kind, char* out, int len);

/* Generate throwaway credentials for a session. */
void NS_hotspotCredentials(char* ssid, int ssid_len, char* psk, int psk_len);

/* Host: hand wlan0 from the client stack to hostapd, serve DHCP. */
bool NS_hotspotStart(const char* ssid, const char* psk, char* err, int errlen);

/* Client: associate with the host's network and take a lease. */
bool NS_hotspotJoin(const char* ssid, const char* psk, char* err, int errlen);

/* Put the normal WiFi back. Safe to call when no hotspot is running. */
void NS_hotspotStop(void);

bool NS_hotspotActive(void);

/* Whether this device has an AP-capable interface, so the UI can omit the
 * option rather than offer something that must fail. */
bool NS_canHostAdhoc(void);

/* Stations on our ad hoc network. ip is empty until the client has taken a
 * lease and talked to us - associated is not the same as reachable. */
typedef struct {
	char mac[32];
	char ip[NS_IP_LEN];
} NS_Client;

#define NS_MAX_CLIENTS 4
int NS_hotspotClients(NS_Client* out, int max);

/* Called roughly once a second while NS_hotspotJoin works, so the caller can
 * animate instead of appearing hung. */
typedef void (*NS_ProgressFn)(const char* stage, int attempt, int attempts);
void NS_setProgressCallback(NS_ProgressFn fn);

void NS_announceStart(NS_Mode mode);
void NS_announceHotspot(const char* ssid, const char* psk);
void NS_announceTick(void);
void NS_announceStop(void);

/* Client: listen for announcements. Call repeatedly; accumulates into peers.
 * Returns the number known so far. */
/* Ad hoc networks we could join, found by scanning for our SSID prefix.
 *
 * Complements discovery rather than replacing it. A host that has already moved
 * to its own network cannot be heard by UDP broadcast on the network we are
 * still on - looking for its SSID is the only way to find it. Costs about two
 * seconds, so call it on demand, not per frame. */
int NS_scanAdhoc(NS_Peer* out, int max);

void NS_discoverStart(void);
int  NS_discoverTick(NS_Peer* peers, int max);
void NS_discoverStop(void);

#endif
