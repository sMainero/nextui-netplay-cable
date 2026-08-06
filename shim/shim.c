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
 *   - retro_(un)serialize    rollback, and detection of frontend-initiated
 *                            loads (rewind/load state) that would desync us
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
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "netlink.h"
#include "overlay.h"

#define SHIM_ENV_REAL_CORE "NETPLAY_REAL_CORE"
#define SHIM_ENV_SESSION   "NETPLAY_SESSION"

/* Bound on packets handed to the core per frame. Without a cap, a peer that
 * has raced ahead can starve the frame. */
#define MAX_PACKETS_PER_FRAME 64

//////////////////////////////////////////////////////////////////////////////
// logging - minarch redirects the emulator's stderr to $LOGS_PATH/<TAG>.txt
//////////////////////////////////////////////////////////////////////////////

static void shim_log(const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	fputs("[netplay-shim] ", stderr);
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

// Set while we are the ones calling core.unserialize (rollback). Any
// unserialize that arrives without this set came from the frontend - rewind or
// load state - and means our lockstep state no longer matches the peer's.
static int shim_owns_unserialize = 0;

// No session file -> pure passthrough. Read once at load; the launcher decides
// whether a session is armed before minarch ever starts.
static int session_active = 0;

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
static void ensure_loaded(void) {
	if (core.handle) return;

	const char* path = getenv(SHIM_ENV_REAL_CORE);
	if (!path || !path[0]) {
		shim_log("FATAL: " SHIM_ENV_REAL_CORE " is unset - nothing to wrap\n");
		exit(EXIT_FAILURE);
	}

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

	const char* session = getenv(SHIM_ENV_SESSION);
	session_active = (session && session[0] && NetLink_configure(session));
	if (session_active) load_option_overrides(session);

	shim_log("wrapping %s (session=%s)\n", path, session_active ? "armed" : "none");

	// Bring the link up now rather than at load_game: the host has to be
	// listening before the client tries to connect, and connecting happens on
	// the netlink thread, so nothing blocks here.
	if (session_active && !NetLink_start()) {
		shim_log("link failed to start - continuing without netplay\n");
		session_active = 0;
	}
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

// Present a dimmed copy of the last frame with a status line and a sweeping
// bar beneath it, both anchored to the bottom so they never cover the action.
static void present_paused_frame(unsigned frame_counter) {
	if (!fe_video_refresh || !last_frame || !last_frame_w) return;

	static void*  scratch;
	static size_t scratch_cap;
	size_t need = last_frame_pitch * last_frame_h;
	if (need > scratch_cap) {
		void* grown = realloc(scratch, need);
		if (!grown) return;
		scratch = grown;
		scratch_cap = need;
	}
	memcpy(scratch, last_frame, need);

	OVL_Target t = {
		.pixels = scratch,
		.width  = last_frame_w,
		.height = last_frame_h,
		.pitch  = last_frame_pitch,
		.format = (OVL_Format)pixel_format,
	};
	OVL_dim(&t);

	static const char* MSG = "Waiting for other player (menu open)...";

	int scale = (last_frame_w >= 480) ? 2 : 1;
	// Separate margins: the bottom one is visual breathing room, the side one
	// only decides wrapping. Keeping the sides tight lets a GBA fit the message
	// on one line - it is 233px against a 240px screen.
	int margin_y = 4 * scale;
	int margin_x = 2 * scale;
	int avail    = (int)last_frame_w - margin_x * 2;

	char lines[3][OVL_MAX_LINE];
	int  nlines = OVL_wrap(MSG, scale, avail, lines, 3);
	if (nlines <= 0) return;

	int line_h = OVL_GLYPH_H * scale;
	int leading = 2 * scale;
	int bar_h  = 2 * scale;
	int gap    = 3 * scale;

	int block_h = nlines * line_h + (nlines - 1) * leading + gap + bar_h;
	int top     = (int)last_frame_h - margin_y - block_h;
	if (top < 0) top = 0;

	// Centre on ink, not advance width, so a line ending in '.' is not pushed
	// left by the blank columns a period reserves.
	int widest = 0, widest_x = 0;
	for (int i = 0; i < nlines; i++) {
		int bearing, ink = OVL_textInk(lines[i], scale, &bearing);
		if (ink > widest) {
			widest = ink;
			widest_x = ((int)last_frame_w - ink) / 2;
		}
	}
	if (widest_x < 0) widest_x = 0;

	int y = top;
	for (int i = 0; i < nlines; i++) {
		int bearing, ink = OVL_textInk(lines[i], scale, &bearing);
		int x = ((int)last_frame_w - ink) / 2 - bearing;
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

	fe_video_refresh(scratch, last_frame_w, last_frame_h, last_frame_pitch);
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
	// TODO(netplay): serve port 1 from the peer while a session is running.
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
	if (netpacket_started) {
		if (core_netpacket.disconnected) core_netpacket.disconnected(NetLink_remoteClientId());
		if (core_netpacket.stop)         core_netpacket.stop();
		netpacket_started = 0;
	}
	if (session_active) NetLink_stop();

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

	if (session_active) {
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
			present_paused_frame(paused_frames++);
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
	return core.serialize_size();
}

bool retro_serialize(void* data, size_t size) {
	if (!core.handle) return false;
	return core.serialize(data, size);
}

bool retro_unserialize(const void* data, size_t size) {
	if (!core.handle) return false;

	// A load we did not initiate is the frontend rewinding or loading a state.
	// Both silently break lockstep, so a live session has to react.
	if (session_active && !shim_owns_unserialize) {
		// TODO(netplay): resync from the peer, or drop the link with an overlay.
		shim_log("frontend-initiated unserialize during session - would desync\n");
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

bool retro_load_game(const struct retro_game_info* game) {
	ensure_loaded();
	return core.load_game(game);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
	ensure_loaded();
	if (!core.load_game_special) return false;
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
	return core.get_memory_data(id);
}

size_t retro_get_memory_size(unsigned id) {
	if (!core.handle) return 0;
	return core.get_memory_size(id);
}
