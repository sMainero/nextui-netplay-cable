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

#define SHIM_ENV_REAL_CORE "NETPLAY_REAL_CORE"
#define SHIM_ENV_SESSION   "NETPLAY_SESSION"

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
	session_active = (session && session[0]);

	shim_log("wrapping %s (session=%s)\n", path, session_active ? "armed" : "none");
}

#undef RESOLVE

//////////////////////////////////////////////////////////////////////////////
// callback wrappers
//
// Pure forwards for now. These exist so the interception path is exercised
// from the first build: if a game plays identically through these, the seams
// are sound and netplay logic can be added behind them.
//////////////////////////////////////////////////////////////////////////////

static bool shim_environment(unsigned cmd, void* data) {
	// TODO(netplay): answer RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE here
	// rather than forwarding it, so GB/GBA link works on a stock frontend.
	return fe_environment ? fe_environment(cmd, data) : false;
}

static void shim_video_refresh(const void* data, unsigned width, unsigned height, size_t pitch) {
	// TODO(netplay): draw session status overlays into the frame here.
	if (fe_video_refresh) fe_video_refresh(data, width, height, pitch);
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
	// TODO(netplay): gate frame advance on the peer while a session is running.
	// Returning without running the core is a legal frame skip and keeps the
	// frontend loop responsive; that is how a stall will be implemented.
	core.run();
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
