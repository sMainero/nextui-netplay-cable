/*
 * netplay shim - a libretro core that wraps another libretro core.
 *
 * minarch loads this instead of the real core; we dlopen the real one and
 * forward every entry point to it. Sitting between the frontend and the core
 * gives us the seams netplay needs without patching minarch:
 *
 *   - retro_set_environment  we answer SET_NETPACKET_INTERFACE ourselves, so
 *                            GB/GBA link works on a stock frontend
 *   - retro_set_input_state  port 1 can be served from the network
 *   - retro_run              frame advance can be gated on the peer
 *   - retro_(un)serialize    hide frontend save states during sessions while
 *                            retaining protocol-owned state synchronization
 *
 * With no session armed this is a pure passthrough and the game behaves
 * exactly as it would without us.
 *
 * IMPORTANT: this .so must be installed under the real core's filename.
 * minarch derives core.name from basename(core_path) truncated at the last
 * underscore (ma_core.c Core_getName), and core.name feeds both config_dir
 * and states_dir. Loading us as "netplay_shim_libretro.so" would relocate the
 * user's save states. The launcher copies us to <realcore>_libretro.so.
 */

#include <dlfcn.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <gnu/libc-version.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>

#include "libretro.h"
#include "netlink.h"
#include "overlay.h"
#include "sha256.h"

#define SHIM_ENV_REAL_CORE "NETPLAY_REAL_CORE"
#define SHIM_ENV_SESSION   "NETPLAY_SESSION"

/* Bound on packets handed to the core per frame. Without a cap, a peer that
 * has raced ahead can starve the frame. */
#define MAX_PACKETS_PER_FRAME 64

/* Frames of input delay for shared-screen netplay. Each frame buys ~16ms of
 * tolerance at the cost of the same in input lag. Measured RTT here averages
 * ~35ms but spikes past 200ms, and every spike that outruns the window becomes
 * a visible stall - so this is tunable per session via `input_delay=N`. */
#define INPUT_DELAY_DEFAULT 6

/* How often the two sides compare state. A divergence is otherwise silent. */
#define HASH_INTERVAL 300

/* How often to report pacing, in advanced frames (~10s of game time). */
#define PACING_INTERVAL 600

/* Consecutive stalled frames before the "waiting" overlay appears. A lockstep
 * session misses its deadline by a frame or two constantly, and drawing the
 * message for each of those turns a barely-perceptible hitch into flashing
 * text across the whole screen. Below this the previous frame simply stays up,
 * which is what a dropped frame should look like. ~200ms at 60fps. */
#define STALL_OVERLAY_FRAMES 12

/* Recovery is a bounded transaction. The frame counter jumps beyond every
 * input that could have been queued before the barrier, so a delayed pre-reset
 * packet cannot be mistaken for input on the recovered timeline. */
#define RECOVERY_TIMEOUT_MS 10000
#define RECOVERY_FRAME_JUMP 512

//////////////////////////////////////////////////////////////////////////////
// logging - minarch redirects the emulator's stderr to $LOGS_PATH/<TAG>.txt
//////////////////////////////////////////////////////////////////////////////

/* Wall-clock prefix, "HH:MM:SS.mmm".
 *
 * Wall clock rather than elapsed because the hard bugs here are two-device
 * stories - a host freezing while the client sits in (waiting...), a watchdog
 * counting strikes while the app believes the session ended. Matching stamps
 * across both handhelds' logs and the watchdog's own log is worth more than
 * saving a subtraction, and these devices keep time across power-off.
 *
 * Milliseconds because the interesting gaps are sub-second: the handshake, the
 * state transfer, the dlopen probe.
 *
 * localtime_r is ~222ns against ~1425ns for the write it accompanies, and it is
 * called only when the second changes - so the common case is one gettimeofday.
 */
static void log_stamp(char* out, int len) {
	struct timeval tv;
	gettimeofday(&tv, NULL);

	static time_t cached_sec = 0;
	static char   cached_hms[32] = "00:00:00";
	if (tv.tv_sec != cached_sec) {
		struct tm tm;
		localtime_r(&tv.tv_sec, &tm);
		strftime(cached_hms, sizeof(cached_hms), "%H:%M:%S", &tm);
		cached_sec = tv.tv_sec;
	}
	snprintf(out, len, "%s.%03d", cached_hms, (int)(tv.tv_usec / 1000));
}

static void shim_log(const char* fmt, ...) {
	char ts[48];
	log_stamp(ts, sizeof(ts));

	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[%s] [netplay-shim] ", ts);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

//////////////////////////////////////////////////////////////////////////////
// the wrapped core
//////////////////////////////////////////////////////////////////////////////

static struct {
	void* handle;

	void     (*init)(void);
	void     (*deinit)(void);
	unsigned (*api_version)(void);
	void     (*get_system_info)(struct retro_system_info*);
	void     (*get_system_av_info)(struct retro_system_av_info*);
	void     (*set_environment)(retro_environment_t);
	void     (*set_video_refresh)(retro_video_refresh_t);
	void     (*set_audio_sample)(retro_audio_sample_t);
	void     (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void     (*set_input_poll)(retro_input_poll_t);
	void     (*set_input_state)(retro_input_state_t);
	void     (*set_controller_port_device)(unsigned, unsigned);
	void     (*reset)(void);
	void     (*run)(void);
	size_t   (*serialize_size)(void);
	bool     (*serialize)(void*, size_t);
	bool     (*unserialize)(const void*, size_t);
	void     (*cheat_reset)(void);
	void     (*cheat_set)(unsigned, bool, const char*);
	bool     (*load_game)(const struct retro_game_info*);
	bool     (*load_game_special)(unsigned, const struct retro_game_info*, size_t);
	void     (*unload_game)(void);
	unsigned (*get_region)(void);
	void*    (*get_memory_data)(unsigned);
	size_t   (*get_memory_size)(unsigned);
} core;

// Callbacks the frontend handed us. We pass our own wrappers to the real core
// and forward through these, so we can intercept in either direction.
static retro_environment_t        fe_environment;
static retro_video_refresh_t      fe_video_refresh;
static retro_audio_sample_t       fe_audio_sample;
static retro_audio_sample_batch_t fe_audio_sample_batch;
static retro_input_poll_t         fe_input_poll;
static retro_input_state_t        fe_input_state;

// Shared-screen netplay: one instance each, same game, inputs synced. Distinct
// from link play, where the cores exchange their own serial/RFU traffic.
static int      netplay_mode = 0;
static int      input_delay = INPUT_DELAY_DEFAULT;
static int      netplay_synced = 0;
static uint32_t netplay_frame = 0;
static uint32_t timeline_start_frame = 0;
static uint32_t frame_buttons[2];      // [0] = host's, [1] = client's
static uint32_t local_inputs[256];     // our own, kept for the delay window
static uint32_t last_scheduled;        // highest frame we have already sent
static int      input_scheduled;

typedef enum {
	RECOVERY_IDLE = 0,
	RECOVERY_CLIENT_WAIT_BEGIN,
	RECOVERY_CLIENT_WAIT_STATE,
	RECOVERY_CLIENT_WAIT_COMMIT,
	RECOVERY_HOST_WAIT_ACK,
} RecoveryPhase;

static RecoveryPhase recovery_phase;
static uint32_t recovery_epoch;
static uint32_t recovery_resume_frame;
static unsigned recovery_count;
static int recovery_failed;
static struct timeval recovery_started;
static uint32_t connection_generation;
static int connection_identity_sent;
static int connection_identity_checked;
static int connection_sync_pending;
static int netplay_ever_synced;

static uint8_t rom_sha256[32];
static int rom_hash_ready;
static char session_id[65];
static char checkpoint_path[192];

static void* checkpoint_candidate;
static size_t checkpoint_candidate_len;
static uint32_t checkpoint_candidate_frame;
static uint32_t checkpoint_candidate_hash;

// Pacing stats. Without these a "laggy" session is unattributable: from the
// outside a core that cannot hit 60fps and a peer whose inputs arrive late look
// identical - both just advance slowly. Frames-per-second answers the first,
// stall share answers the second.
static uint32_t stat_frames;      // advanced since the last report
static uint32_t stat_stalls;      // frames spent waiting for peer input
static uint32_t stat_stall_max;   // longest single wait, in frames
static uint32_t stall_run;        // current consecutive stall length
static struct timeval stat_since;

// No session file -> pure passthrough. Read once at load; the launcher decides
// whether a session is armed before minarch ever starts.
static int session_active = 0;
/* Core-managed auxiliary saves (memory cards, high scores, etc.) bypass
 * RETRO_MEMORY_SAVE_RAM. A shared-screen guest sees a fresh process-local
 * directory in /tmp so those files can function during play but never touch
 * the user's persistent save tree. */
static char guest_save_dir[256];

// Core options forced by the session, as `option.<key>=<value>` lines. gpSP's
// link emulation is per-game (gpsp_serial: mul_poke, mul_aw1, mul_aw2, rfu) and
// gambatte encodes a peer IP as twelve single-digit options, so the cap is not
// merely generous - it has to hold a whole address plus mode and port.
// does nothing on "auto" for a game it does not recognise, so a link session has
// to be able to pin it - and both peers must agree.
#define MAX_OPTION_OVERRIDES 24
static struct { char key[64]; char value[64]; } option_override[MAX_OPTION_OVERRIDES];
static int option_override_count = 0;
static int option_update_pending = 0;

// The core's netpacket interface, if it registered one. This is the whole point
// of the shim for link play: the frontend never sees the request, so GB/GBA
// link works on a completely stock minarch.
// Last frame the core produced, kept so a paused session has something to show.
// Skipping core.run() means video_refresh is never called, and minarch simply
// re-presents whatever was on screen - a freeze indistinguishable from a crash.
static void*    last_frame;
static size_t   last_frame_cap;
static unsigned last_frame_w, last_frame_h;
static size_t   last_frame_pitch;
static unsigned pixel_format = RETRO_PIXEL_FORMAT_RGB565;

// Audio geometry, for feeding silence while paused rather than letting the
// frontend's buffer run dry and crackle.
static double av_fps = 60.0, av_sample_rate = 44100.0;

// Video geometry from the same call. Needed because a pause can happen before
// the core has ever produced a frame, which is the normal case at the start of
// a shared-screen session - there is no last frame to copy, so the status
// message has to be drawn into a buffer we make ourselves.
static unsigned av_base_width, av_base_height;

static struct retro_netpacket_callback core_netpacket;
static int  have_netpacket = 0;   // core registered an interface
static int  netpacket_started = 0; // we have called its start()

// The session file is read twice on purpose: netlink takes the transport keys,
// this takes the option overrides. Keeping them separate beats threading core
// option concerns through the network layer.
static void load_option_overrides(const char* session_path) {
	FILE* f = fopen(session_path, "r");
	if (!f) return;

	char line[256];
	while (fgets(line, sizeof(line), f) && option_override_count < MAX_OPTION_OVERRIDES) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		if (strncmp(line, "option.", 7) != 0) continue;

		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';

		const char* k = line + 7;
		const char* v = eq + 1;
		if (strlen(k) >= sizeof(option_override[0].key) ||
		    strlen(v) >= sizeof(option_override[0].value)) {
			shim_log("ignoring over-long option override '%s'\n", k);
			continue;
		}
		strcpy(option_override[option_override_count].key, k);
		strcpy(option_override[option_override_count].value, v);
		shim_log("forcing core option %s=%s\n",
		         option_override[option_override_count].key,
		         option_override[option_override_count].value);
		option_override_count++;
	}
	fclose(f);

	if (option_override_count) option_update_pending = 1;
}

static void load_session_id(const char* path) {
	session_id[0] = checkpoint_path[0] = '\0';
	FILE* f = fopen(path, "r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char* end = strpbrk(line, "\r\n");
		if (end) *end = '\0';
		if (strncmp(line, "session_id=", 11)) continue;
		const char* value = line + 11;
		size_t n = strlen(value);
		if (!n || n >= sizeof(session_id)) break;
		bool safe = true;
		for (size_t i = 0; i < n; i++)
			if (!((value[i] >= '0' && value[i] <= '9') ||
			      (value[i] >= 'a' && value[i] <= 'f') ||
			      (value[i] >= 'A' && value[i] <= 'F'))) safe = false;
		if (safe) snprintf(session_id, sizeof(session_id), "%s", value);
		break;
	}
	fclose(f);
	if (session_id[0])
		snprintf(checkpoint_path, sizeof(checkpoint_path),
		         "/tmp/netplay-host-%s.state", session_id);
}

// Mode is normally derived from the core (see core_wants_link). An explicit
// mode= line still wins, which keeps the session file authoritative for testing
// and leaves a way out if the core list is ever wrong.
//
// Returns 1 for shared screen, 0 for link cable, -1 when the session says
// nothing and the core should decide.
static int session_mode_override(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return -1;
	char line[256];
	int v = -1;
	while (fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		if (!strcmp(line, "mode=netplay"))    v = 1;
		else if (!strcmp(line, "mode=link"))  v = 0;
	}
	fclose(f);
	return v;
}

// Case-insensitive substring, ASCII only. strcasestr is a GNU extension and
// these cores get built against several libcs.
static int contains_ci(const char* hay, const char* needle) {
	if (!hay || !needle || !*needle) return 0;
	for (; *hay; hay++) {
		const char* h = hay;
		const char* n = needle;
		while (*h && *n) {
			int a = (*h >= 'A' && *h <= 'Z') ? *h + 32 : *h;
			int b = (*n >= 'A' && *n <= 'Z') ? *n + 32 : *n;
			if (a != b) break;
			h++; n++;
		}
		if (!*n) return 1;
	}
	return 0;
}

// Cores that emulate a real cable carry their own serial/RFU traffic over the
// netpacket interface, so they get the link path; everything else gets
// shared-screen netplay, which needs only serialize/unserialize and therefore
// works for any core.
//
// Matched on library_name rather than the file name: the pak stages cores under
// cores/override/<platform>/, and the name the core reports is the thing that
// actually identifies it.
static int core_wants_link(void) {
	static const char* LINK_CORES[] = { "gambatte", "gpsp" };

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);
	if (!info.library_name) return 0;

	for (size_t i = 0; i < sizeof(LINK_CORES) / sizeof(LINK_CORES[0]); i++) {
		if (contains_ci(info.library_name, LINK_CORES[i])) return 1;
	}
	return 0;
}

// Core options that a session cannot work without, applied unless the session
// file already sets them.
//
// pcsx_rearmed's drc_thread compiles recompiler blocks on a worker thread. Until
// a block is ready the emulation thread runs the interpreter instead, and the
// two paths do not agree on cycle timing - so the serialized state depends on
// when the compiler thread happened to finish. Measured: five runs of the same
// binary, same ROM, same frame count produced five different state hashes, and
// disabling this one option made all five identical. Two devices running the
// *same* build would desync, which no amount of pinning or core-shipping could
// have fixed.
//
// Only ever consulted for shared-screen sessions; link-cable cores carry their
// own traffic and never compare state.
static const struct {
	const char* core;
	const char* key;
	const char* value;
	bool session_may_override;
}
REQUIRED_OPTIONS[] = {
	{ "pcsx-rearmed", "pcsx_rearmed_drc_thread", "disabled", true },
	/* PCSX otherwise writes its second, core-managed card behind the frontend's
	 * back. Shared-screen sessions deliberately expose only card 1 through the
	 * libretro save-memory buffer, which lets the host own persistence and lets
	 * the guest use an in-memory copy without touching its filesystem. */
	{ "pcsx-rearmed", "pcsx_rearmed_memcard1", "libretro", false },
	{ "pcsx-rearmed", "pcsx_rearmed_memcard2", "none", false },
};

static void apply_required_options(void) {
	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);
	if (!info.library_name) return;

	for (size_t i = 0; i < sizeof(REQUIRED_OPTIONS) / sizeof(REQUIRED_OPTIONS[0]); i++) {
		if (!contains_ci(info.library_name, REQUIRED_OPTIONS[i].core)) continue;

		// The session file wins. Someone testing a theory about this option
		// should not have it silently overwritten.
		int already = -1;
		for (int j = 0; j < option_override_count; j++) {
			if (!strcmp(option_override[j].key, REQUIRED_OPTIONS[i].key)) { already = j; break; }
		}
		if (already >= 0 && REQUIRED_OPTIONS[i].session_may_override) {
			shim_log("session already sets %s - leaving it alone\n", REQUIRED_OPTIONS[i].key);
			continue;
		}
		if (already >= 0) {
			snprintf(option_override[already].value,
			         sizeof(option_override[already].value), "%s", REQUIRED_OPTIONS[i].value);
			shim_log("forcing %s=%s (required for host-only save persistence)\n",
			         REQUIRED_OPTIONS[i].key, REQUIRED_OPTIONS[i].value);
			option_update_pending = 1;
			continue;
		}
		if (option_override_count >= MAX_OPTION_OVERRIDES) {
			shim_log("WARNING: no room to force %s - session may desync\n", REQUIRED_OPTIONS[i].key);
			return;
		}
		snprintf(option_override[option_override_count].key,
		         sizeof(option_override[0].key), "%s", REQUIRED_OPTIONS[i].key);
		snprintf(option_override[option_override_count].value,
		         sizeof(option_override[0].value), "%s", REQUIRED_OPTIONS[i].value);
		shim_log("forcing %s=%s (required for shared-screen netplay in %s)\n",
		         REQUIRED_OPTIONS[i].key, REQUIRED_OPTIONS[i].value, info.library_name);
		option_override_count++;
		option_update_pending = 1;
	}
}

// Both peers must agree on the delay, so it is written by the app into both
// session files rather than negotiated.
static int session_input_delay(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return INPUT_DELAY_DEFAULT;
	char line[256];
	int v = INPUT_DELAY_DEFAULT;
	while (fgets(line, sizeof(line), f)) {
		int n;
		if (sscanf(line, "input_delay=%d", &n) == 1 && n >= 1 && n <= 20) v = n;
	}
	fclose(f);
	return v;
}

static const char* find_option_override(const char* key) {
	for (int i = 0; i < option_override_count; i++) {
		if (!strcmp(option_override[i].key, key)) return option_override[i].value;
	}
	return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// loading
//////////////////////////////////////////////////////////////////////////////

#define RESOLVE(field, name)                                            \
	do {                                                                \
		core.field = dlsym(core.handle, name);                          \
		if (!core.field) missing = missing ? missing : name;            \
	} while (0)

// Resolved lazily: minarch calls retro_get_system_info before it calls any of
// the retro_set_* registration functions, so there is no single safe place to
// do this up front. Every exported entry point calls this first.
//
// Failure is fatal by design. There is no useful degraded mode - and returning
// a zeroed retro_system_info would segfault minarch, which strcpys
// valid_extensions unchecked. Stock minarch exits when a core won't load; we
// match that so the log says what happened.
/* Frozen executable-sharing implementation. The setting remains visible, but
 * unauthenticated peer binaries must not reach dlopen. Compatibility selection
 * is now performed by the setup app using local, packaged core manifests. */
#if 0
/* CRC32 of a file, plus its ELF machine type. Enough to say whether two builds
 * are the same and whether the other one could even load here.
 *
 * Deliberately a file hash rather than library_name/library_version: the core
 * has to be identified *before* dlopen, because the whole point is to decide
 * which file to open. */
static bool fingerprint_core(const char* path, NetLinkCoreId* out) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;

	static uint32_t table[256];
	static int table_built = 0;
	if (!table_built) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		table_built = 1;
	}

	uint32_t crc = 0xFFFFFFFFu;
	size_t total = 0;
	unsigned char buf[16384];
	size_t n;
	uint16_t machine = 0;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		if (total == 0 && n >= 20 && !memcmp(buf, "\177ELF", 4))
			machine = (uint16_t)(buf[18] | (buf[19] << 8));
		for (size_t i = 0; i < n; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
		total += n;
	}
	fclose(f);

	out->crc = crc ^ 0xFFFFFFFFu;
	out->size = (uint32_t)total;
	out->machine = machine;
	out->can_send = 0;
	return total > 0;
}

/* Highest GLIBC_x.y a file requires, as major*1000+minor.
 *
 * Scans for the literal strings rather than walking the version-needs section:
 * they live in .dynstr either way, and a byte scan cannot be fooled by a
 * malformed section header. Verified against readelf on real builds - tg5050
 * cores come back 2.33, every other platform 2.17. */
static uint32_t glibc_floor(const char* path) {
	FILE* f = fopen(path, "rb");
	if (!f) return 0;

	uint32_t best = 0;
	char win[512];
	size_t fill = 0;
	size_t n;
	/* Overlapping window, so a match spanning two reads is not missed. */
	while ((n = fread(win + fill, 1, sizeof(win) - fill - 1, f)) > 0) {
		size_t have = fill + n;
		win[have] = '\0';
		for (size_t i = 0; i + 10 < have; i++) {
			if (memcmp(win + i, "GLIBC_", 6)) continue;
			unsigned maj = 0, min = 0;
			if (sscanf(win + i + 6, "%u.%u", &maj, &min) == 2) {
				uint32_t v = maj * 1000 + min;
				if (v > best) best = v;
			}
		}
		if (have > 32) {
			memmove(win, win + have - 32, 32);
			fill = 32;
		} else fill = have;
	}
	fclose(f);
	return best;
}

/* What this device provides. gnu_get_libc_version is glibc-specific, which is
 * fine - these platforms are all glibc. */
static uint32_t runtime_glibc(void) {
	const char* v = gnu_get_libc_version();
	unsigned maj = 0, min = 0;
	if (v && sscanf(v, "%u.%u", &maj, &min) == 2) return maj * 1000 + min;
	return 0;
}

/* library_version without loading a ROM: retro_get_system_info is callable
 * before retro_init, so a core can be identified for a few milliseconds of
 * dlopen rather than a full launch. */
static void probe_core_version(const char* path, char* out, int len) {
	out[0] = '\0';
	void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) return;
	void (*gsi)(struct retro_system_info*) = dlsym(h, "retro_get_system_info");
	if (gsi) {
		struct retro_system_info i;
		memset(&i, 0, sizeof(i));
		gsi(&i);
		if (i.library_version) snprintf(out, len, "%s", i.library_version);
	}
	dlclose(h);
}

/* Does the session permit handing our core to a peer? Written by the app from
 * its settings; absent means the default, which is on. */
static int session_share_cores(const char* path) {
	FILE* f = fopen(path, "r");
	if (!f) return 1;
	char line[256];
	int v = 1;
	while (fgets(line, sizeof(line), f)) {
		int n;
		if (sscanf(line, " share_cores = %d", &n) == 1) v = n;
	}
	fclose(f);
	return v;
}

/* Production waits long enough for a peer coming up over WiFi. Host tests that
 * deliberately run one side can shorten this without baking test timing into
 * the protocol. Values outside a useful range are ignored. */
static int negotiate_timeout_ms(void) {
	const char* value = getenv("NETPLAY_NEGOTIATE_TIMEOUT_MS");
	if (!value || !value[0]) return 15000;
	char* end = NULL;
	long ms = strtol(value, &end, 10);
	return end && *end == '\0' && ms >= 50 && ms <= 60000 ? (int)ms : 15000;
}

/* Where an adopted core is written. /tmp is RAM on these devices, which is the
 * right place: it is fast, and a borrowed core must not outlive the session or
 * quietly become what this device runs from then on. */
#define ADOPTED_CORE_PATH "/tmp/netplay_adopted_core.so"

/* Wait for the handshake, compare builds, and if they differ take the host's.
 *
 * Bounded, and every failure falls back to our own core rather than refusing to
 * start: a session that plays with a desync risk is more use than one that will
 * not launch. Returns the path to dlopen.
 */
/* A core the app already staged for this session, if any.
 *
 * This is the fast path the negotiation exists to avoid needing: when the app
 * has exchanged manifests at arm time and fetched the peer's build while the
 * user was still browsing, the launch has nothing to wait for. Named for the
 * core we were asked to wrap, so one staged directory serves every system. */
static const char* staged_core(const char* path) {
	const char* base = strrchr(path, '/');
	base = base ? base + 1 : path;

	const char* pak = getenv("NETPLAY_PAK");
	if (!pak || !pak[0]) return NULL;

	static char staged[512];
	snprintf(staged, sizeof(staged), "%s/cores/staged/%s", pak, base);

	struct stat st;
	if (stat(staged, &st) != 0 || st.st_size <= 0) return NULL;

	void* probe = dlopen(staged, RTLD_NOW | RTLD_LOCAL);
	if (!probe) {
		shim_log("staged core will not load (%s) - ignoring it\n", dlerror());
		return NULL;
	}
	dlclose(probe);
	shim_log("using the core staged for this session: %s\n", staged);
	return staged;
}

static const char* negotiate_core(const char* path, const char* session) {
	/* Checked before anything is exchanged: if the app already settled this,
	 * there is nothing to negotiate and no reason to wait for a handshake. */
	const char* pre = staged_core(path);
	if (pre) {
		NetLink_start();
		return pre;
	}

	NetLinkCoreId mine;
	if (!fingerprint_core(path, &mine)) return path;
	mine.can_send = session_share_cores(session) ? 1 : 0;
	mine.core_glibc = glibc_floor(path);
	mine.runtime_glibc = runtime_glibc();
	probe_core_version(path, mine.version, sizeof(mine.version));
	NetLink_setCoreId(&mine);

	if (!NetLink_start()) return path;

	/* The handshake runs on the netlink thread. Bounded wait - this is on the
	 * path to the first frame, so it must not be able to hang a launch. */
	NetLinkCoreId theirs;
	int waited = 0;
	int negotiate_ms = negotiate_timeout_ms();
	while (!NetLink_peerCoreId(&theirs) && waited < negotiate_ms) {
		usleep(100 * 1000);
		waited += 100;
	}
	if (!NetLink_peerCoreId(&theirs)) {
		shim_log("peer core identity never arrived - using ours\n");
		return path;
	}

	if (theirs.crc == mine.crc && theirs.size == mine.size) {
		shim_log("core builds match (crc=%08x)\n", mine.crc);
		return path;
	}

	/* Same source revision is enough. Different platforms build the same commit
	 * into different files, and those emulate identically - transferring
	 * between them would be megabytes spent to arrive back where we started. */
	if (mine.version[0] && !strcmp(mine.version, theirs.version)) {
		shim_log("different build, same revision '%s' - no transfer needed\n", mine.version);
		return path;
	}

	shim_log("core mismatch: ours crc=%08x '%s', theirs crc=%08x '%s'\n",
	         mine.crc, mine.version, theirs.crc, theirs.version);

	/* Architecture is the one difference no transfer can bridge. */
	if (theirs.machine != mine.machine) {
		shim_log("peer is a different architecture (%u vs %u) - cannot share cores; "
		         "shared screen will desync unless the builds happen to agree\n",
		         theirs.machine, mine.machine);
		return path;
	}

	/* Who donates is a loadability question before it is an authority question.
	 * A tg5050-built core needs glibc 2.33 where every other platform provides
	 * 2.17, so host-wins would hand over a file the peer cannot open. */
	bool we_could_load_theirs   = theirs.core_glibc <= mine.runtime_glibc;
	bool they_could_load_ours   = mine.core_glibc <= theirs.runtime_glibc;

	if (!we_could_load_theirs && !they_could_load_ours) {
		shim_log("neither core is loadable on the other device "
		         "(theirs needs %u.%u, we have %u.%u; ours needs %u.%u, they have %u.%u)\n",
		         theirs.core_glibc / 1000, theirs.core_glibc % 1000,
		         mine.runtime_glibc / 1000, mine.runtime_glibc % 1000,
		         mine.core_glibc / 1000, mine.core_glibc % 1000,
		         theirs.runtime_glibc / 1000, theirs.runtime_glibc % 1000);
		return path;
	}

	/* Default is host-donates, overridden only when loadability says otherwise. */
	bool we_donate = (NetLink_getRole() == NETLINK_ROLE_HOST);
	if (!we_could_load_theirs && they_could_load_ours) {
		if (!we_donate) shim_log("we cannot load the host's core - offering ours instead\n");
		we_donate = true;
	} else if (!they_could_load_ours && we_could_load_theirs) {
		if (we_donate) shim_log("peer cannot load our core - taking theirs instead\n");
		we_donate = false;
	}

	if (we_donate) {
		if (!mine.can_send) {
			shim_log("core sharing disabled here - peer keeps its own build\n");
			return path;
		}
		FILE* f = fopen(path, "rb");
		if (!f) return path;
		void* buf = malloc(mine.size);
		if (!buf) { fclose(f); return path; }
		size_t got = fread(buf, 1, mine.size, f);
		fclose(f);
		if (got == mine.size && NetLink_sendCore(buf, got))
			shim_log("sent our core to the peer (%zu bytes)\n", got);
		else
			shim_log("could not send our core\n");
		free(buf);
		return path;
	}

	if (!theirs.can_send) {
		shim_log("peer is not sharing its core - keeping ours\n");
		return path;
	}

	void* data = NULL;
	size_t len = 0;
	waited = 0;
	while (!NetLink_takeCore(&data, &len) && waited < 60000) {
		usleep(100 * 1000);
		waited += 100;
	}
	if (!data) {
		shim_log("peer's core never arrived - using ours\n");
		return path;
	}

	FILE* out = fopen(ADOPTED_CORE_PATH, "wb");
	if (!out) { free(data); return path; }
	size_t wrote = fwrite(data, 1, len, out);
	fclose(out);
	free(data);
	if (wrote != len) {
		shim_log("could not write the adopted core (%zu of %zu) - using ours\n", wrote, len);
		remove(ADOPTED_CORE_PATH);
		return path;
	}

	/* Prove it loads before committing. A core that fails dlopen here would
	 * otherwise take the whole launch down, and our own core still works. */
	void* probe = dlopen(ADOPTED_CORE_PATH, RTLD_NOW | RTLD_LOCAL);
	if (!probe) {
		shim_log("adopted core will not load (%s) - using ours\n", dlerror());
		remove(ADOPTED_CORE_PATH);
		return path;
	}
	dlclose(probe);

	shim_log("adopted the peer's core (%zu bytes)\n", len);
	return ADOPTED_CORE_PATH;
}
#endif

static void ensure_loaded(void) {
	if (core.handle) return;

	const char* path = getenv(SHIM_ENV_REAL_CORE);
	if (!path || !path[0]) {
		shim_log("FATAL: " SHIM_ENV_REAL_CORE " is unset - nothing to wrap\n");
		exit(EXIT_FAILURE);
	}

	const char* session = getenv(SHIM_ENV_SESSION);
	session_active = (session && session[0] && NetLink_configure(session));
	if (session_active) NetLink_start();

	// RTLD_LOCAL keeps the real core's retro_* symbols out of the global
	// namespace, where they would collide with the ones we export.
	core.handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!core.handle) {
		shim_log("FATAL: dlopen(%s) failed: %s\n", path, dlerror());
		exit(EXIT_FAILURE);
	}

	const char* missing = NULL;
	RESOLVE(init,                       "retro_init");
	RESOLVE(deinit,                     "retro_deinit");
	RESOLVE(api_version,                "retro_api_version");
	RESOLVE(get_system_info,            "retro_get_system_info");
	RESOLVE(get_system_av_info,         "retro_get_system_av_info");
	RESOLVE(set_environment,            "retro_set_environment");
	RESOLVE(set_video_refresh,          "retro_set_video_refresh");
	RESOLVE(set_audio_sample,           "retro_set_audio_sample");
	RESOLVE(set_audio_sample_batch,     "retro_set_audio_sample_batch");
	RESOLVE(set_input_poll,             "retro_set_input_poll");
	RESOLVE(set_input_state,            "retro_set_input_state");
	RESOLVE(set_controller_port_device, "retro_set_controller_port_device");
	RESOLVE(reset,                      "retro_reset");
	RESOLVE(run,                        "retro_run");
	RESOLVE(serialize_size,             "retro_serialize_size");
	RESOLVE(serialize,                  "retro_serialize");
	RESOLVE(unserialize,                "retro_unserialize");
	RESOLVE(cheat_reset,                "retro_cheat_reset");
	RESOLVE(cheat_set,                  "retro_cheat_set");
	RESOLVE(load_game,                  "retro_load_game");
	RESOLVE(unload_game,                "retro_unload_game");
	RESOLVE(get_region,                 "retro_get_region");
	RESOLVE(get_memory_data,            "retro_get_memory_data");
	RESOLVE(get_memory_size,            "retro_get_memory_size");

	// Optional - not every core implements it, and minarch tolerates its absence.
	core.load_game_special = dlsym(core.handle, "retro_load_game_special");

	if (missing) {
		shim_log("FATAL: %s is missing %s\n", path, missing);
		exit(EXIT_FAILURE);
	}

	if (session_active) {
		load_option_overrides(session);
		load_session_id(session);

		// get_system_info is resolved above, so the core can be asked what it is
		// before anything else happens. Both sides run the same ROM, so they
		// reach the same answer without having to negotiate it.
		int override = session_mode_override(session);
		netplay_mode = (override >= 0) ? override : !core_wants_link();

		struct retro_system_info info;
		memset(&info, 0, sizeof(info));
		core.get_system_info(&info);
		shim_log("mode=%s for %s (%s)\n",
		         netplay_mode ? "shared-screen" : "link-cable",
		         info.library_name ? info.library_name : "unknown core",
		         override >= 0 ? "from session" : "from core");

		input_delay = session_input_delay(session);

		// After the mode is known: only shared screen compares state, and only
		// then does a nondeterministic core matter.
		if (netplay_mode) apply_required_options();
		if (netplay_mode && NetLink_getRole() == NETLINK_ROLE_CLIENT) {
			snprintf(guest_save_dir, sizeof(guest_save_dir),
			         "/tmp/netplay-guest-%s%ld",
			         session_id[0] ? session_id : "process-", (long)getpid());
			if (mkdir(guest_save_dir, 0700) != 0 && errno != EEXIST) {
				shim_log("WARNING: cannot create volatile guest save directory %s: %s\n",
				         guest_save_dir, strerror(errno));
				guest_save_dir[0] = '\0';
			} else {
				shim_log("guest core-managed saves redirected to %s\n", guest_save_dir);
			}
		}
	}

	shim_log("wrapping %s (session=%s)\n", path, session_active ? "armed" : "none");

	// The transport starts before the core is opened so link-capable cores see a
	// ready netpacket interface during their own initialization.
}

#undef RESOLVE

//////////////////////////////////////////////////////////////////////////////
// callback wrappers
//
// Pure forwards for now. These exist so the interception path is exercised
// from the first build: if a game plays identically through these, the seams
// are sound and netplay logic can be added behind them.
//////////////////////////////////////////////////////////////////////////////

// Bridges the core's send to the wire. Handed to the core in start().
static void shim_netpacket_send(int flags, const void* buf, size_t len, uint16_t client_id) {
	NetLink_send(flags, buf, len, client_id);
}

// The core may call this to read mid-frame rather than waiting for the next
// poll. Receiving happens on the netlink thread, so there is nothing to pump
// here - packets are already queued and will be delivered by deliver_packets().
static void shim_netpacket_poll_receive(void) {
}

static bool shim_environment(unsigned cmd, void* data) {
	if (cmd == RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE) {
		// Only claim to support this when we can actually back it. With no
		// session armed we forward to the frontend, which says no - so a
		// disarmed launch leaves the core in exactly the state it would be in
		// without the shim, rather than waiting on a session that never starts.
		if (!session_active) {
			return fe_environment ? fe_environment(cmd, data) : false;
		}

		// Answer this ourselves instead of forwarding. A stock minarch returns
		// false here, which makes the core disable link support entirely.
		if (!data) {
			memset(&core_netpacket, 0, sizeof(core_netpacket));
			have_netpacket = 0;
			shim_log("core withdrew its netpacket interface\n");
			return true;
		}

		const struct retro_netpacket_callback* cb = data;
		if (!cb->start || !cb->receive) {
			shim_log("core offered an unusable netpacket interface\n");
			return false;
		}

		core_netpacket = *cb;
		have_netpacket = 1;
		shim_log("core registered netpacket interface (protocol=%s)\n",
		         cb->protocol_version ? cb->protocol_version : "core version");
		return true;
	}

	// Answer forced options ourselves. This replaces the patched minarch's
	// minarch_setCoreOptionValue / forceCoreOptionUpdate: the core simply never
	// sees the frontend's value for a key we are pinning.
	// Track the format so the paused frame can be dimmed correctly.
	if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT && data) {
		pixel_format = *(const enum retro_pixel_format*)data;
	}

	if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE && session_active && data) {
		struct retro_variable* var = data;
		const char* forced = var->key ? find_option_override(var->key) : NULL;
		if (forced) {
			var->value = forced;
			return true;
		}
	}

	if (cmd == RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY && guest_save_dir[0] && data) {
		*(const char**)data = guest_save_dir;
		return true;
	}

	if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE && session_active && data) {
		bool fe_says = fe_environment ? fe_environment(cmd, data) : false;
		if (option_update_pending) {
			option_update_pending = 0;
			*(bool*)data = true;
			return true;
		}
		return fe_says;
	}

	return fe_environment ? fe_environment(cmd, data) : false;
}

// Bring both sides to a bit-identical starting point. The host ships its state;
// the client adopts it. Without this the two simulations differ from frame one
// and every synced input afterwards is meaningless.
// Identify the emulator, not the game. Two devices can run the same ROM under
// genuinely different cores.
static uint32_t core_identity(void) {
	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	core.get_system_info(&info);

	uint32_t h = 2166136261u;
	const char* parts[2] = { info.library_name, info.library_version };
	for (int i = 0; i < 2; i++)
		for (const char* p = parts[i]; p && *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }

	/* Serialize size differs between builds far more often than the version
	 * string does, so fold it in. */
	size_t sz = core.serialize_size();
	h ^= (uint32_t)sz;
	h *= 16777619u;

	/* Log the inputs, not just the hash. A single number tells you two builds
	 * disagree but not which field disagrees - and "same version string, still
	 * a mismatch" is exactly the case worth telling apart, because a differing
	 * serialize_size means the two sides cannot exchange state at all, while a
	 * differing version is only a build to line up. */
	shim_log("core identity: name='%s' version='%s' serialize_size=%zu -> %08x\n",
	         info.library_name ? info.library_name : "?",
	         info.library_version ? info.library_version : "?",
	         sz, h);
	return h;
}

#define AUTHORITATIVE_MAGIC 0x4E505356u /* NPSV */
#define AUTHORITATIVE_VERSION 1u

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t version;
	uint32_t state_size;
	uint32_t sram_size;
	uint32_t rtc_size;
} AuthoritativeHeader;

static uint32_t hash_bytes(const void* data, size_t len);

static bool persistent_memory_sizes(uint32_t* sram_size, uint32_t* rtc_size) {
	size_t sram = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	size_t rtc = core.get_memory_size(RETRO_MEMORY_RTC);
	if (sram > UINT32_MAX || rtc > UINT32_MAX) return false;
	*sram_size = (uint32_t)sram;
	*rtc_size = (uint32_t)rtc;
	return true;
}

static bool parse_authoritative_state(const void* data, size_t len,
		AuthoritativeHeader* parsed) {
	if (!data || len < sizeof(AuthoritativeHeader)) return false;
	AuthoritativeHeader wire;
	memcpy(&wire, data, sizeof(wire));
	AuthoritativeHeader h = {
		.magic = ntohl(wire.magic),
		.version = ntohl(wire.version),
		.state_size = ntohl(wire.state_size),
		.sram_size = ntohl(wire.sram_size),
		.rtc_size = ntohl(wire.rtc_size),
	};
	uint64_t payload64 = (uint64_t)h.state_size + h.sram_size + h.rtc_size;
	if (h.magic != AUTHORITATIVE_MAGIC || h.version != AUTHORITATIVE_VERSION ||
	    payload64 > NETLINK_MAX_STATE - sizeof(AuthoritativeHeader) ||
	    len != sizeof(AuthoritativeHeader) + (size_t)payload64)
		return false;
	if (parsed) *parsed = h;
	return true;
}

/* Package the core state together with the raw persistent-memory regions. The
 * files that minarch used to populate those regions (.sav, .srm, compressed or
 * otherwise) never enter the protocol. */
static bool capture_authoritative_state(void** out, size_t* out_len, uint32_t* out_hash) {
	size_t state_size = core.serialize_size();
	uint32_t sram_size, rtc_size;
	if (!state_size || state_size > UINT32_MAX ||
	    !persistent_memory_sizes(&sram_size, &rtc_size)) return false;
	uint64_t payload64 = (uint64_t)state_size + sram_size + rtc_size;
	if (payload64 > NETLINK_MAX_STATE - sizeof(AuthoritativeHeader)) return false;
	size_t payload = (size_t)payload64;

	void* sram = sram_size ? core.get_memory_data(RETRO_MEMORY_SAVE_RAM) : NULL;
	void* rtc = rtc_size ? core.get_memory_data(RETRO_MEMORY_RTC) : NULL;
	if ((sram_size && !sram) || (rtc_size && !rtc)) return false;

	size_t total = sizeof(AuthoritativeHeader) + payload;
	uint8_t* buf = malloc(total);
	if (!buf) return false;
	AuthoritativeHeader wire = {
		.magic = htonl(AUTHORITATIVE_MAGIC),
		.version = htonl(AUTHORITATIVE_VERSION),
		.state_size = htonl((uint32_t)state_size),
		.sram_size = htonl(sram_size),
		.rtc_size = htonl(rtc_size),
	};
	memcpy(buf, &wire, sizeof(wire));
	uint8_t* state = buf + sizeof(wire);
	memset(state, 0, state_size);
	if (!core.serialize(state, state_size)) { free(buf); return false; }
	if (sram_size) memcpy(state + state_size, sram, sram_size);
	if (rtc_size) memcpy(state + state_size + sram_size, rtc, rtc_size);

	*out = buf;
	*out_len = total;
	if (out_hash) *out_hash = hash_bytes(buf, total);
	return true;
}

static bool apply_authoritative_state(const void* data, size_t len) {
	AuthoritativeHeader h;
	uint32_t local_sram, local_rtc;
	if (!parse_authoritative_state(data, len, &h) ||
	    h.state_size != core.serialize_size() ||
	    !persistent_memory_sizes(&local_sram, &local_rtc) ||
	    h.sram_size != local_sram || h.rtc_size != local_rtc)
		return false;

	const uint8_t* state = (const uint8_t*)data + sizeof(AuthoritativeHeader);
	if (!core.unserialize(state, h.state_size)) return false;
	void* sram = h.sram_size ? core.get_memory_data(RETRO_MEMORY_SAVE_RAM) : NULL;
	void* rtc = h.rtc_size ? core.get_memory_data(RETRO_MEMORY_RTC) : NULL;
	if ((h.sram_size && !sram) || (h.rtc_size && !rtc)) return false;
	if (h.sram_size) memcpy(sram, state + h.state_size, h.sram_size);
	if (h.rtc_size) memcpy(rtc, state + h.state_size + h.sram_size, h.rtc_size);
	return true;
}

#define CHECKPOINT_MAGIC 0x4E50434Bu /* NPCK */
typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t core_identity;
	uint32_t state_size;
	uint32_t frame;
	uint32_t state_hash;
	uint8_t rom_sha256[32];
} CheckpointHeader;

static uint32_t hash_bytes(const void* data, size_t len) {
	uint32_t h = 2166136261u;
	const uint8_t* p = data;
	for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 16777619u; }
	return h;
}

static void set_checkpoint_candidate(void* data, size_t len, uint32_t frame, uint32_t hash) {
	free(checkpoint_candidate);
	checkpoint_candidate = data;
	checkpoint_candidate_len = len;
	checkpoint_candidate_frame = frame;
	checkpoint_candidate_hash = hash;
}

static bool promote_checkpoint(void) {
	if (!checkpoint_path[0] || !checkpoint_candidate || !checkpoint_candidate_len) return false;
	char tmp[224];
	snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", checkpoint_path, (long)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return false;
	CheckpointHeader h = {
		.magic = CHECKPOINT_MAGIC, .version = 2,
		.core_identity = core_identity(),
		.state_size = (uint32_t)checkpoint_candidate_len,
		.frame = checkpoint_candidate_frame,
		.state_hash = checkpoint_candidate_hash,
	};
	memcpy(h.rom_sha256, rom_sha256, sizeof(h.rom_sha256));
	const uint8_t* parts[2] = { (const uint8_t*)&h, checkpoint_candidate };
	size_t lengths[2] = { sizeof(h), checkpoint_candidate_len };
	bool ok = true;
	for (int part = 0; part < 2 && ok; part++) {
		const uint8_t* p = parts[part];
		size_t left = lengths[part];
		while (left) {
			ssize_t n = write(fd, p, left);
			if (n > 0) { p += n; left -= (size_t)n; }
			else if (n < 0 && errno == EINTR) continue;
			else { ok = false; break; }
		}
	}
	if (ok) ok = fsync(fd) == 0;
	if (close(fd) != 0) ok = false;
	if (ok) ok = rename(tmp, checkpoint_path) == 0;
	if (!ok) unlink(tmp);
	if (ok) shim_log("confirmed host checkpoint promoted at frame %u\n", h.frame);
	else shim_log("could not persist confirmed host checkpoint: %s\n", strerror(errno));
	return ok;
}

static bool load_checkpoint(void** data, size_t* len, uint32_t* frame) {
	if (!checkpoint_path[0]) return false;
	FILE* f = fopen(checkpoint_path, "rb");
	if (!f) return false;
	CheckpointHeader h;
	bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) &&
	          h.magic == CHECKPOINT_MAGIC && h.version == 2 &&
	          h.core_identity == core_identity() &&
	          h.state_size <= NETLINK_MAX_STATE &&
	          !memcmp(h.rom_sha256, rom_sha256, sizeof(h.rom_sha256));
	void* buf = ok ? malloc(h.state_size) : NULL;
	if (!buf || fread(buf, 1, h.state_size, f) != h.state_size) ok = false;
	if (ok && (!parse_authoritative_state(buf, h.state_size, NULL) ||
	           hash_bytes(buf, h.state_size) != h.state_hash)) ok = false;
	if (fgetc(f) != EOF) ok = false;
	fclose(f);
	if (!ok) { free(buf); shim_log("ignored invalid or stale host checkpoint\n"); return false; }
	*data = buf; *len = h.state_size; *frame = h.frame;
	shim_log("restored last confirmed host checkpoint from frame %u\n", h.frame);
	return true;
}

static void reset_netplay_timeline(uint32_t frame);
static void recovery_clock_start(void);

static bool identity_matches(const NetLinkSessionIdentity* peer) {
	uint32_t mine = core_identity();
	size_t state_size = core.serialize_size();
	uint32_t sram_size = 0, rtc_size = 0;
	if (!persistent_memory_sizes(&sram_size, &rtc_size)) return false;
	if (memcmp(peer->rom_sha256, rom_sha256, 32)) {
		shim_log("refusing peer: ROM content hashes differ\n");
		return false;
	}
	if (peer->core_identity != mine || peer->state_size != state_size ||
	    peer->sram_size != sram_size || peer->rtc_size != rtc_size) {
		shim_log("refusing peer: core/state/persistent-memory identity differs "
		         "(peer %08x/%u/%u/%u, ours %08x/%zu/%u/%u)\n",
		         peer->core_identity, peer->state_size, peer->sram_size, peer->rtc_size,
		         mine, state_size, sram_size, rtc_size);
		return false;
	}
	return true;
}

/* Every TCP generation is a new synchronization barrier. This covers both a
 * guest relaunch (the live host donates its current state) and a host relaunch
 * (the new host first loads its last peer-confirmed checkpoint). */
static void netplay_handshake(void) {
	if (!NetLink_isConnected() || !rom_hash_ready) return;
	uint32_t generation = NetLink_connectionGeneration();
	if (generation != connection_generation) {
		connection_generation = generation;
		connection_identity_sent = connection_identity_checked = 0;
		connection_sync_pending = 1;
		netplay_synced = 0;
		recovery_failed = 0;
		recovery_started.tv_sec = recovery_started.tv_usec = 0;
		recovery_phase = NetLink_getRole() == NETLINK_ROLE_CLIENT
		               ? RECOVERY_CLIENT_WAIT_BEGIN : RECOVERY_IDLE;
		NetLink_resetSync();
		shim_log("connection %u requires an authoritative sync\n", generation);
	}

	if (!connection_identity_sent) {
		NetLinkSessionIdentity mine;
		memset(&mine, 0, sizeof(mine));
		memcpy(mine.rom_sha256, rom_sha256, 32);
		mine.core_identity = core_identity();
		mine.state_size = (uint32_t)core.serialize_size();
		if (!persistent_memory_sizes(&mine.sram_size, &mine.rtc_size)) {
			recovery_failed = 1;
			return;
		}
		connection_identity_sent = NetLink_sendSessionIdentity(&mine);
	}

	if (!connection_identity_checked) {
		NetLinkSessionIdentity peer;
		if (!NetLink_takeSessionIdentity(&peer)) return;
		if (!identity_matches(&peer)) {
			recovery_failed = 1;
			return;
		}
		connection_identity_checked = 1;
		shim_log("ROM and core identity match peer\n");
	}

	if (!connection_sync_pending || NetLink_getRole() != NETLINK_ROLE_HOST ||
	    recovery_phase != RECOVERY_IDLE) return;

	size_t sz = 0;
	void* buf = NULL;
	uint32_t source_frame = 0;
	bool restored = false;
	if (!netplay_ever_synced)
		restored = load_checkpoint(&buf, &sz, &source_frame);
	if (!restored) {
		if (!capture_authoritative_state(&buf, &sz, NULL)) return;
		source_frame = netplay_ever_synced ? netplay_frame : 0;
	}
	if (!apply_authoritative_state(buf, sz)) {
		free(buf); recovery_failed = 1;
		shim_log("host could not adopt synchronization state\n");
		return;
	}

	recovery_epoch++;
	if (!recovery_epoch) recovery_epoch++;
	recovery_resume_frame = (restored || netplay_ever_synced)
	                      ? source_frame + RECOVERY_FRAME_JUMP : 0;
	if (!NetLink_beginResync(recovery_epoch, recovery_resume_frame) ||
	    !NetLink_sendState(buf, sz)) {
		free(buf); return;
	}
	set_checkpoint_candidate(buf, sz, recovery_resume_frame, hash_bytes(buf, sz));
	reset_netplay_timeline(recovery_resume_frame);
	recovery_phase = RECOVERY_HOST_WAIT_ACK;
	recovery_clock_start();
	shim_log("authoritative %s state sent for connection %u; resume frame %u\n",
	         restored ? "checkpoint" : (netplay_ever_synced ? "live" : "initial"),
	         generation, recovery_resume_frame);
}

static void reset_netplay_timeline(uint32_t frame) {
	netplay_frame = frame;
	timeline_start_frame = frame;
	memset(local_inputs, 0, sizeof(local_inputs));
	memset(frame_buttons, 0, sizeof(frame_buttons));
	last_scheduled = 0;
	input_scheduled = 0;
	stall_run = 0;
	stat_frames = stat_stalls = stat_stall_max = 0;
	memset(&stat_since, 0, sizeof(stat_since));
	NetLink_resetSync();
}

static void recovery_clock_start(void) {
	gettimeofday(&recovery_started, NULL);
}

static bool recovery_timed_out(void) {
	if (!recovery_started.tv_sec) return false;
	struct timeval now;
	gettimeofday(&now, NULL);
	long ms = (now.tv_sec - recovery_started.tv_sec) * 1000L
	        + (now.tv_usec - recovery_started.tv_usec) / 1000L;
	return ms > RECOVERY_TIMEOUT_MS;
}

/* Advance an authoritative-state recovery transaction at a frame boundary.
 * True means the core must remain paused for this retro_run call. */
static bool netplay_recovery_tick(void) {
	if (recovery_failed) return true;

	if (recovery_phase != RECOVERY_IDLE && recovery_timed_out()) {
		recovery_failed = 1;
		shim_log("authoritative resync timed out - session cannot continue safely\n");
		return true;
	}

	if (NetLink_getRole() == NETLINK_ROLE_HOST) {
		uint32_t mismatch_frame;
		if (recovery_phase == RECOVERY_IDLE &&
		    NetLink_takeResyncRequest(&mismatch_frame)) {
			size_t sz = 0;
			void* buf = NULL;
			if (!capture_authoritative_state(&buf, &sz, NULL)) {
				recovery_failed = 1;
				shim_log("cannot create authoritative state for resync\n");
				return true;
			}

			recovery_epoch++;
			if (!recovery_epoch) recovery_epoch++;
			recovery_resume_frame = netplay_frame + RECOVERY_FRAME_JUMP;
			recovery_clock_start();

			bool sent = NetLink_beginResync(recovery_epoch, recovery_resume_frame) &&
			            NetLink_sendState(buf, sz);
			bool adopted = apply_authoritative_state(buf, sz);
			free(buf);

			if (!sent || !adopted) {
				recovery_failed = 1;
				shim_log("authoritative resync state could not be sent or adopted\n");
				return true;
			}

			reset_netplay_timeline(recovery_resume_frame);
			recovery_phase = RECOVERY_HOST_WAIT_ACK;
			shim_log("authoritative resync %u sent after mismatch at frame %u; "
			         "resume frame %u\n", recovery_epoch, mismatch_frame,
			         recovery_resume_frame);
			return true;
		}

		if (recovery_phase == RECOVERY_HOST_WAIT_ACK) {
			uint32_t epoch;
			bool loaded;
			if (NetLink_takeResyncAck(&epoch, &loaded) && epoch == recovery_epoch) {
				if (!loaded || !NetLink_commitResync(epoch)) {
					recovery_failed = 1;
					shim_log("client could not adopt authoritative resync %u\n", epoch);
					return true;
				}
				if (connection_sync_pending) {
					promote_checkpoint();
					netplay_synced = netplay_ever_synced = 1;
					connection_sync_pending = 0;
				}
				recovery_phase = RECOVERY_IDLE;
				recovery_started.tv_sec = recovery_started.tv_usec = 0;
				recovery_count++;
				shim_log("authoritative resync %u committed (%u %s this session)\n",
				         epoch, recovery_count, recovery_count == 1 ? "recovery" : "recoveries");
			}
			return true;
		}
		return false;
	}

	if (recovery_phase == RECOVERY_CLIENT_WAIT_BEGIN) {
		uint32_t epoch, frame;
		if (NetLink_takeResyncBegin(&epoch, &frame)) {
			recovery_epoch = epoch;
			recovery_resume_frame = frame;
			recovery_phase = RECOVERY_CLIENT_WAIT_STATE;
			shim_log("authoritative resync %u beginning; resume frame %u\n",
			         epoch, frame);
		}
	}

	if (recovery_phase == RECOVERY_CLIENT_WAIT_STATE) {
		void* buf = NULL;
		size_t len = 0;
		if (NetLink_takeState(&buf, &len)) {
			bool loaded = apply_authoritative_state(buf, len);
			free(buf);

			if (loaded) reset_netplay_timeline(recovery_resume_frame);
			if (!NetLink_ackResync(recovery_epoch, loaded) || !loaded) {
				recovery_failed = 1;
				shim_log("could not adopt authoritative resync %u (%zu-byte bundle)\n",
				         recovery_epoch, len);
				return true;
			}
			recovery_phase = RECOVERY_CLIENT_WAIT_COMMIT;
			shim_log("authoritative resync %u adopted; waiting for commit\n", recovery_epoch);
		}
	}

	if (recovery_phase == RECOVERY_CLIENT_WAIT_COMMIT) {
		uint32_t epoch;
		if (NetLink_takeResyncCommit(&epoch) && epoch == recovery_epoch) {
			if (connection_sync_pending) {
				netplay_synced = netplay_ever_synced = 1;
				connection_sync_pending = 0;
			}
			recovery_phase = RECOVERY_IDLE;
			recovery_started.tv_sec = recovery_started.tv_usec = 0;
			recovery_count++;
			shim_log("authoritative resync %u committed (%u %s this session)\n",
			         epoch, recovery_count, recovery_count == 1 ? "recovery" : "recoveries");
			return true;
		}
	}

	return recovery_phase != RECOVERY_IDLE;
}

// Report how the session is actually pacing. Attribution is the whole point:
// fps well under the core's own rate means this device cannot keep up, while a
// high stall share with healthy fps means we are waiting on the peer. Both feel
// like "lag" and the fixes are opposite.
static void netplay_reportPacing(void) {
	if (!stat_since.tv_sec) { gettimeofday(&stat_since, NULL); return; }
	if (netplay_frame % PACING_INTERVAL) return;

	struct timeval now;
	gettimeofday(&now, NULL);
	long ms = (now.tv_sec - stat_since.tv_sec) * 1000L
	        + (now.tv_usec - stat_since.tv_usec) / 1000L;
	if (ms <= 0) return;

	// Integer tenths throughout - no float formatting in a core .so.
	unsigned fps10   = (unsigned)((stat_frames * 10000UL) / (unsigned long)ms);
	unsigned total   = stat_frames + stat_stalls;
	unsigned stall_pct = total ? (unsigned)((stat_stalls * 100UL) / total) : 0;

	shim_log("pacing: %u frames in %ldms (%u.%u fps), stalled %u frames (%u%%), "
	         "longest %u, delay %d\n",
	         stat_frames, ms, fps10 / 10, fps10 % 10,
	         stat_stalls, stall_pct, stat_stall_max, input_delay);

	stat_frames = stat_stalls = stat_stall_max = 0;
	stat_since = now;
}

// Cheap rolling checksum; we only need to notice divergence, not locate it.
static bool capture_state(void** out, size_t* out_len, uint32_t* out_hash) {
	return capture_authoritative_state(out, out_len, out_hash);
}

static bool netplay_checkDivergence(void) {
	#define OWN_HASHES 8
	static uint32_t own_frame[OWN_HASHES];
	static uint32_t own_hash[OWN_HASHES];
	static int own_valid[OWN_HASHES];

	if (NetLink_getRole() == NETLINK_ROLE_HOST) {
		uint32_t frame, hash;
		bool matched;
		while (NetLink_takeCheckpointAck(&frame, &hash, &matched)) {
			if (matched && checkpoint_candidate &&
			    frame == checkpoint_candidate_frame && hash == checkpoint_candidate_hash)
				promote_checkpoint();
			else if (!matched)
				shim_log("guest rejected checkpoint at frame %u\n", frame);
		}
	}

	// Both sides hash at exactly the same frames.
	//
	// retro_serialize is not guaranteed to be side-effect free, and picodrive's
	// is not: an extra call costs 25 bytes of divergence within 900 frames
	// (shim/test/statecheck.c --serprobe). The client used to hash only when it
	// reached the frame a host hash described, so a hash that arrived late meant
	// the host had serialized and the client had not - and the check that exists
	// to detect divergence became a cause of it. The observed signature was a
	// session in sync until the first missed check and desynced from then on.
	//
	// So hashing is unconditional and identically timed on both sides, and the
	// comparison is done separately, whenever the peer's hash turns up.
	if (netplay_frame % HASH_INTERVAL == 0) {
		void* snapshot = NULL;
		size_t snapshot_len = 0;
		uint32_t mine = 0;
		if (!capture_state(&snapshot, &snapshot_len, &mine)) return false;
		if (NetLink_getRole() == NETLINK_ROLE_HOST) {
			if (netplay_frame == 0) shim_log("state at frame 0: %08x\n", mine);
			set_checkpoint_candidate(snapshot, snapshot_len, netplay_frame, mine);
			NetLink_sendHash(netplay_frame, mine);
			return false;
		}
		free(snapshot);
		unsigned slot = (netplay_frame / HASH_INTERVAL) % OWN_HASHES;
		own_frame[slot] = netplay_frame;
		own_hash[slot] = mine;
		own_valid[slot] = 1;
	}

	if (NetLink_getRole() == NETLINK_ROLE_HOST) return false;
	uint32_t f, h;
	while (NetLink_takeHash(&f, &h)) {
		unsigned s = (f / HASH_INTERVAL) % OWN_HASHES;
		if (!own_valid[s] || own_frame[s] != f) {
			shim_log("no local hash for frame %u to compare (now at %u)\n", f, netplay_frame);
			continue;
		}
		if (f == 0)
			shim_log("state at frame 0: ours %08x, host %08x - handshake %s\n",
			         own_hash[s], h,
			         own_hash[s] == h ? "equalised both sides" : "did NOT equalise");
		if (own_hash[s] != h) {
			NetLink_ackCheckpoint(f, h, false);
			shim_log("DESYNC at frame %u (host %08x, ours %08x)\n", f, h, own_hash[s]);
			if (recovery_phase == RECOVERY_IDLE && NetLink_requestResync(f)) {
				recovery_phase = RECOVERY_CLIENT_WAIT_BEGIN;
				recovery_clock_start();
				shim_log("requested authoritative state from host\n");
				return true;
			}
		} else {
			NetLink_ackCheckpoint(f, h, true);
			shim_log("in sync at frame %u (%08x)\n", f, own_hash[s]);
		}
	}
	return false;
}

// Drive the core's netpacket callbacks to match the link state. Called once per
// frame from retro_run, before the core runs.
static void pump_netpacket(void) {
	if (!have_netpacket || !session_active) return;

	if (NetLink_consumeConnectEvent()) {
		core_netpacket.start(NetLink_localClientId(),
		                     shim_netpacket_send,
		                     shim_netpacket_poll_receive);
		netpacket_started = 1;

		// Two players, and the peer is present the moment we are connected.
		if (core_netpacket.connected) {
			core_netpacket.connected(NetLink_remoteClientId());
		}
		shim_log("netpacket session started\n");
	}

	if (NetLink_consumeDisconnectEvent() && netpacket_started) {
		// Order matters: the core wants to hear about the player leaving before
		// the session ends. gpSP needs this to unstick its RFU state machine.
		if (core_netpacket.disconnected) {
			core_netpacket.disconnected(NetLink_remoteClientId());
		}
		if (core_netpacket.stop) {
			core_netpacket.stop();
		}
		netpacket_started = 0;
		shim_log("netpacket session stopped\n");
	}

	if (!netpacket_started) return;

	uint8_t buf[NETLINK_MAX_PACKET];
	size_t len;
	int delivered = 0;
	while (delivered < MAX_PACKETS_PER_FRAME && NetLink_popPacket(buf, sizeof(buf), &len)) {
		core_netpacket.receive(buf, len, NetLink_remoteClientId());
		delivered++;
	}

	if (core_netpacket.poll) core_netpacket.poll();
}

static void shim_video_refresh(const void* data, unsigned width, unsigned height, size_t pitch) {
	if (data && session_active) {
		size_t need = pitch * height;
		if (need > last_frame_cap) {
			void* grown = realloc(last_frame, need);
			if (grown) { last_frame = grown; last_frame_cap = need; }
		}
		if (last_frame && need <= last_frame_cap) {
			memcpy(last_frame, data, need);
			last_frame_w = width;
			last_frame_h = height;
			last_frame_pitch = pitch;
		}
	}
	if (fe_video_refresh) fe_video_refresh(data, width, height, pitch);
}

static size_t pixel_bytes(void) {
	return pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
}

// Whether the frontend has a frame of ours to hold on screen. When it does, a
// short stall can simply present nothing and let that frame stand.
static int have_last_frame(void) {
	return last_frame && last_frame_w && last_frame_h;
}

// Present a dimmed copy of the last frame with a status line and a sweeping
// bar beneath it, both anchored to the bottom so they never cover the action.
//
// A NULL msg re-presents the last frame untouched. That is not the same as
// returning without presenting: skipping fe_video_refresh entirely does not
// leave the previous frame on screen - it leaves whatever the frontend last
// had, which is how a pre-first-frame stall produced a black screen. During a
// burst of short stalls that alternation between real frames and nothing is
// visible as flicker, so a stalled frame must still hand the frontend
// something, even when there is nothing new to say.
static void present_paused_frame(unsigned frame_counter, const char* msg) {
	if (!fe_video_refresh) return;

	// Nothing to overlay: hand back the frame we already have, no copy, no dim.
	// This runs at frame rate during a stall burst, so it stays cheap.
	if (!msg && have_last_frame()) {
		fe_video_refresh(last_frame, last_frame_w, last_frame_h, last_frame_pitch);
		return;
	}

	unsigned fw    = last_frame_w;
	unsigned fh    = last_frame_h;
	size_t   fpitch = last_frame_pitch;
	int      have_last = have_last_frame();

	// Shared-screen netplay gates ahead of the first core.run(), so there is no
	// last frame precisely when the waiting message matters most. Presenting
	// nothing leaves the frontend on a black screen with no explanation - which
	// is indistinguishable from a hang, and was exactly that on device. Fall
	// back to a black frame in the core's own geometry.
	if (!have_last) {
		fw     = av_base_width;
		fh     = av_base_height;
		fpitch = (size_t)fw * pixel_bytes();
		if (!fw || !fh) return;   // av_info not seen yet; nothing sane to draw
	}

	static void*  scratch;
	static size_t scratch_cap;
	size_t need = fpitch * fh;
	if (need > scratch_cap) {
		void* grown = realloc(scratch, need);
		if (!grown) return;
		scratch = grown;
		scratch_cap = need;
	}
	if (have_last) memcpy(scratch, last_frame, need);
	else           memset(scratch, 0, need);

	OVL_Target t = {
		.pixels = scratch,
		.width  = fw,
		.height = fh,
		.pitch  = fpitch,
		.format = (OVL_Format)pixel_format,
	};
	// Dimming an already-black frame is a no-op, so this stays unconditional.
	OVL_dim(&t);

	const char* MSG = msg;

	int scale = (fw >= 480) ? 2 : 1;
	// Separate margins: the bottom one is visual breathing room, the side one
	// only decides wrapping. Keeping the sides tight lets a GBA fit the message
	// on one line - it is 233px against a 240px screen.
	int margin_y = 4 * scale;
	int margin_x = 2 * scale;
	int avail    = (int)fw - margin_x * 2;

	char lines[3][OVL_MAX_LINE];
	int  nlines = OVL_wrap(MSG, scale, avail, lines, 3);
	if (nlines <= 0) return;

	int line_h = OVL_GLYPH_H * scale;
	int leading = 2 * scale;
	int bar_h  = 2 * scale;
	int gap    = 3 * scale;

	int block_h = nlines * line_h + (nlines - 1) * leading + gap + bar_h;
	int top     = (int)fh - margin_y - block_h;
	if (top < 0) top = 0;

	// Centre on ink, not advance width, so a line ending in '.' is not pushed
	// left by the blank columns a period reserves.
	int widest = 0, widest_x = 0;
	for (int i = 0; i < nlines; i++) {
		int bearing, ink = OVL_textInk(lines[i], scale, &bearing);
		if (ink > widest) {
			widest = ink;
			widest_x = ((int)fw - ink) / 2;
		}
	}
	if (widest_x < 0) widest_x = 0;

	int y = top;
	for (int i = 0; i < nlines; i++) {
		int bearing, ink = OVL_textInk(lines[i], scale, &bearing);
		int x = ((int)fw - ink) / 2 - bearing;
		if (x < 0) x = 0;
		OVL_drawText(&t, x, y, lines[i], scale);
		y += line_h + leading;
	}

	// The sweep sits directly beneath the text and spans the same width, so the
	// two read as one element rather than unrelated marks.
	int track_x = widest_x;
	int span = widest / 4 ? widest / 4 : 1;
	int head = (int)((frame_counter * 2) % (unsigned)(widest + span));
	int from = head > span ? head - span : 0;
	int to   = head < widest ? head : widest;

	if (to > from)
		OVL_fillRect(&t, track_x + from, top + block_h - bar_h, to - from, bar_h);

	fe_video_refresh(scratch, fw, fh, fpitch);
}

// Silence for one frame. Without it the frontend's audio buffer drains and
// crackles, which sounds like a fault rather than a pause.
static void present_paused_audio(void) {
	if (!fe_audio_sample_batch || av_fps <= 0.0) return;
	size_t frames = (size_t)(av_sample_rate / av_fps);
	if (!frames || frames > 4096) return;
	static int16_t silence[4096 * 2];
	fe_audio_sample_batch(silence, frames);
}

static void shim_audio_sample(int16_t left, int16_t right) {
	if (fe_audio_sample) fe_audio_sample(left, right);
}

static size_t shim_audio_sample_batch(const int16_t* data, size_t frames) {
	return fe_audio_sample_batch ? fe_audio_sample_batch(data, frames) : 0;
}

static void shim_input_poll(void) {
	if (fe_input_poll) fe_input_poll();
}

static int16_t shim_input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
	// Shared-screen netplay drives both ports from the synced buffers. Stock
	// minarch returns 0 for every port above 0, which is exactly why the patched
	// build had to add this - the second player has to come from somewhere.
	if (netplay_mode && netplay_synced && device == RETRO_DEVICE_JOYPAD && index == 0 && port < 2) {
		uint32_t b = frame_buttons[port];
		if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)b;
		return (b >> id) & 1;
	}
	return fe_input_state ? fe_input_state(port, device, index, id) : 0;
}

//////////////////////////////////////////////////////////////////////////////
// libretro entry points
//////////////////////////////////////////////////////////////////////////////

void retro_set_environment(retro_environment_t cb) {
	fe_environment = cb;
	ensure_loaded();
	core.set_environment(shim_environment);
}

void retro_set_video_refresh(retro_video_refresh_t cb) {
	fe_video_refresh = cb;
	ensure_loaded();
	core.set_video_refresh(shim_video_refresh);
}

void retro_set_audio_sample(retro_audio_sample_t cb) {
	fe_audio_sample = cb;
	ensure_loaded();
	core.set_audio_sample(shim_audio_sample);
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) {
	fe_audio_sample_batch = cb;
	ensure_loaded();
	core.set_audio_sample_batch(shim_audio_sample_batch);
}

void retro_set_input_poll(retro_input_poll_t cb) {
	fe_input_poll = cb;
	ensure_loaded();
	core.set_input_poll(shim_input_poll);
}

void retro_set_input_state(retro_input_state_t cb) {
	fe_input_state = cb;
	ensure_loaded();
	core.set_input_state(shim_input_state);
}

void retro_init(void) {
	ensure_loaded();
	core.init();
}

void retro_deinit(void) {
	if (!core.handle) return;

	// Tell the core the session is over before it tears itself down, otherwise
	// it deinits holding a netpacket interface it thinks is still live.
	if (netplay_mode) {
		netplay_synced = 0;
		input_scheduled = 0;
		last_scheduled = 0;
		NetLink_resetSync();
	}

	if (netpacket_started) {
		if (core_netpacket.disconnected) core_netpacket.disconnected(NetLink_remoteClientId());
		if (core_netpacket.stop)         core_netpacket.stop();
		netpacket_started = 0;
	}
	if (session_active) NetLink_stop();
	free(checkpoint_candidate);
	checkpoint_candidate = NULL;
	checkpoint_candidate_len = 0;

	core.deinit();
}

unsigned retro_api_version(void) {
	ensure_loaded();
	return core.api_version();
}

void retro_get_system_info(struct retro_system_info* info) {
	ensure_loaded();
	// Verbatim: minarch reads valid_extensions and need_fullpath from here.
	core.get_system_info(info);
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	ensure_loaded();
	core.get_system_av_info(info);
	if (info->timing.fps > 0.0)         av_fps = info->timing.fps;
	if (info->timing.sample_rate > 0.0) av_sample_rate = info->timing.sample_rate;
	if (info->geometry.base_width)      av_base_width  = info->geometry.base_width;
	if (info->geometry.base_height)     av_base_height = info->geometry.base_height;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
	if (!core.handle) return;
	core.set_controller_port_device(port, device);
}

void retro_reset(void) {
	if (!core.handle) return;
	core.reset();
}

void retro_run(void) {
	if (!core.handle) return;

	// Netpacket state and inbound packets are settled before the core runs, so
	// a frame sees everything that arrived since the last one.
	pump_netpacket();

	if (session_active && netplay_mode) {
		NetLink_markFrame();
		netplay_handshake();

		if (netplay_recovery_tick()) {
			if (fe_input_poll) fe_input_poll();
			static unsigned recovery_wait;
			present_paused_frame(recovery_wait++, recovery_failed
			                     ? "Resync failed - exit the game."
			                     : (netplay_synced ? "Resynchronizing from host..."
			                                       : "Rejoining from host..."));
			present_paused_audio();
			return;
		}

		if (!netplay_synced) {
			// Nothing to show yet and nothing to run; keep the frontend alive.
			if (fe_input_poll) fe_input_poll();
			static unsigned waiting;
			present_paused_frame(waiting++, "Starting session...");
			present_paused_audio();
			return;
		}

		// Sample and send each frame's input exactly once. A stall returns
		// without advancing netplay_frame, so re-sampling here would overwrite
		// an input the peer may already have committed to - the two sides would
		// then run the same frame from different inputs.
		uint32_t target = netplay_frame + (uint32_t)input_delay;
		if (!input_scheduled || target > last_scheduled) {
			uint32_t local = fe_input_state
				? (uint32_t)fe_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK)
				: 0;
			local_inputs[target % 256] = local;
			NetLink_sendInput(target, local);
			last_scheduled = target;
			input_scheduled = 1;
		}

		// Our own input for the current frame was decided INPUT_DELAY frames
		// ago; the first few frames are primed neutral.
		uint32_t mine = (netplay_frame - timeline_start_frame < (uint32_t)input_delay)
		              ? 0 : local_inputs[netplay_frame % 256];

		uint32_t theirs = 0;
		if (netplay_frame - timeline_start_frame >= (uint32_t)input_delay &&
		    !NetLink_getRemoteInput(netplay_frame, &theirs)) {
			// Peer input for this frame has not arrived. Stall rather than run
			// ahead: advancing without it would desync immediately.
			if (fe_input_poll) fe_input_poll();

			stat_stalls++;
			stall_run++;
			if (stall_run > stat_stall_max) stat_stall_max = stall_run;

			// Always present something - see present_paused_frame. Only say
			// anything once the wait is long enough to be worth explaining: a
			// held frame reads as a hitch, while a message that appears and
			// vanishes every few frames reads as a fault.
			present_paused_frame(stall_run,
			                     (stall_run > STALL_OVERLAY_FRAMES || !have_last_frame())
			                         ? "Waiting for other player..." : NULL);
			present_paused_audio();
			return;
		}
		stall_run = 0;

		int host = (NetLink_getRole() == NETLINK_ROLE_HOST);
		frame_buttons[0] = host ? mine : theirs;
		frame_buttons[1] = host ? theirs : mine;

		if (netplay_checkDivergence()) {
			if (fe_input_poll) fe_input_poll();
			present_paused_frame(0, "Desync detected - resynchronizing...");
			present_paused_audio();
			return;
		}
		netplay_frame++;
		stat_frames++;
		netplay_reportPacing();
	}
	else if (session_active) {
		NetLink_markFrame();

		// Peer's frontend is blocked - a menu, a sleep. Running ahead would only
		// fill a queue it is not draining, so skip the frame. This is a legal
		// dupe frame; the frontend loop keeps polling input and stays responsive.
		if (NetLink_isPeerPaused()) {
			// Input is polled only from inside core.run(). Skip that without
			// doing this and PAD_poll never runs, so no button - including
			// MENU - is ever seen and the frontend is frozen, not merely
			// paused. minarch does the same at each of its own frame skips.
			if (fe_input_poll) fe_input_poll();

			static unsigned paused_frames;
			present_paused_frame(paused_frames++, "Waiting for other player (menu open)...");
			present_paused_audio();
			return;
		}
	}

	// TODO(netplay): gate frame advance on the peer for input-lockstep netplay.
	// Returning without running the core is a legal frame skip; link play does
	// not need it - the cores keep their own timing.
	//
	// Bracketed so a core that blocks in here waiting on its peer is not
	// mistaken for a frontend that has gone away.
	NetLink_setCoreRunning(true);
	core.run();
	NetLink_setCoreRunning(false);
}

size_t retro_serialize_size(void) {
	if (!core.handle) return 0;
	/* Save states are deliberately unavailable for every armed session. Returning
	 * zero is the libretro capability signal minarch uses to omit its save/load
	 * actions. Protocol synchronization calls core.serialize_* directly and is
	 * unaffected; battery-backed SRAM remains normal. */
	if (session_active) return 0;
	return core.serialize_size();
}

bool retro_serialize(void* data, size_t size) {
	if (!core.handle) return false;
	if (session_active) return false;
	return core.serialize(data, size);
}

bool retro_unserialize(const void* data, size_t size) {
	if (!core.handle) return false;
	if (session_active) {
		shim_log("frontend save-state load blocked during session\n");
		return false;
	}

	return core.unserialize(data, size);
}

void retro_cheat_reset(void) {
	if (!core.handle) return;
	core.cheat_reset();
}

void retro_cheat_set(unsigned index, bool enabled, const char* code) {
	if (!core.handle) return;
	core.cheat_set(index, enabled, code);
}

static bool hash_content_path(Sha256* hash, const char* path, unsigned depth) {
	if (depth > 4) return false;
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	uint8_t buf[32768];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) != 0) sha256_update(hash, buf, n);
	bool ok = !ferror(f);
	fclose(f);
	if (!ok) return false;

	const char* ext = strrchr(path, '.');
	bool cue = ext && !strcasecmp(ext, ".cue");
	bool m3u = ext && (!strcasecmp(ext, ".m3u") || !strcasecmp(ext, ".m3u8"));
	if (!cue && !m3u) return true;
	f = fopen(path, "r");
	if (!f) return false;
	char dir[1024], line[2048];
	snprintf(dir, sizeof(dir), "%s", path);
	char* slash = strrchr(dir, '/');
	if (slash) *slash = '\0'; else snprintf(dir, sizeof(dir), ".");
	while (ok && fgets(line, sizeof(line), f)) {
		char* ref = line;
		char* end = strpbrk(ref, "\r\n"); if (end) *end = '\0';
		if (cue) {
			while (*ref == ' ' || *ref == '\t') ref++;
			if (strncasecmp(ref, "FILE", 4) || (ref[4] != ' ' && ref[4] != '\t')) continue;
			ref += 4; while (*ref == ' ' || *ref == '\t') ref++;
			if (*ref == '"') { ref++; end = strchr(ref, '"'); }
			else { end = strpbrk(ref, " \t"); }
			if (end) *end = '\0';
		} else {
			while (*ref == ' ' || *ref == '\t') ref++;
			if (!*ref || *ref == '#') continue;
			end = ref + strlen(ref);
			while (end > ref && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
		}
		if (!*ref) continue;
		char child[2048];
		if (ref[0] == '/') snprintf(child, sizeof(child), "%s", ref);
		else snprintf(child, sizeof(child), "%s/%s", dir, ref);
		static const uint8_t separator[] = { 0, 'R', 'E', 'F', 0 };
		sha256_update(hash, separator, sizeof(separator));
		ok = hash_content_path(hash, child, depth + 1);
	}
	fclose(f);
	return ok;
}

static bool hash_game_content(const struct retro_game_info* game, uint8_t out[32]) {
	Sha256 hash;
	sha256_init(&hash);
	if (game && game->data && game->size) {
		sha256_update(&hash, game->data, game->size);
	} else if (game && game->path) {
		if (!hash_content_path(&hash, game->path, 0)) return false;
	} else return false;
	sha256_final(&hash, out);
	return true;
}

bool retro_load_game(const struct retro_game_info* game) {
	ensure_loaded();
	if (session_active && netplay_mode) {
		rom_hash_ready = hash_game_content(game, rom_sha256);
		if (!rom_hash_ready) {
			shim_log("cannot hash ROM content - shared-screen netplay disabled\n");
			netplay_mode = 0;
		} else {
			shim_log("ROM SHA-256: %02x%02x%02x%02x...%02x%02x%02x%02x\n",
			         rom_sha256[0], rom_sha256[1], rom_sha256[2], rom_sha256[3],
			         rom_sha256[28], rom_sha256[29], rom_sha256[30], rom_sha256[31]);
		}
	} else rom_hash_ready = 0;
	return core.load_game(game);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
	ensure_loaded();
	if (!core.load_game_special) return false;
	if (session_active && netplay_mode) {
		shim_log("multi-content games do not yet have a complete ROM-set hash - shared-screen netplay disabled\n");
		netplay_mode = 0;
	}
	return core.load_game_special(game_type, info, num_info);
}

void retro_unload_game(void) {
	if (!core.handle) return;
	core.unload_game();
}

unsigned retro_get_region(void) {
	if (!core.handle) return RETRO_REGION_NTSC;
	return core.get_region();
}

void* retro_get_memory_data(unsigned id) {
	if (!core.handle) return NULL;
	/* Minarch uses these entry points for both its startup read and its
	 * menu/sleep/exit writes. On a shared-screen guest, hide persistent memory
	 * from the frontend in both directions. Protocol code above deliberately
	 * calls core.get_memory_* directly, so the guest core still receives and
	 * uses the host's authoritative in-memory copy. */
	if (session_active && netplay_mode &&
	    NetLink_getRole() == NETLINK_ROLE_CLIENT &&
	    (id == RETRO_MEMORY_SAVE_RAM || id == RETRO_MEMORY_RTC))
		return NULL;
	return core.get_memory_data(id);
}

size_t retro_get_memory_size(unsigned id) {
	if (!core.handle) return 0;
	if (session_active && netplay_mode &&
	    NetLink_getRole() == NETLINK_ROLE_CLIENT &&
	    (id == RETRO_MEMORY_SAVE_RAM || id == RETRO_MEMORY_RTC))
		return 0;
	return core.get_memory_size(id);
}
