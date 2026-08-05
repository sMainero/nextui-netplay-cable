/*
 * Stands in for minarch: loads the shim exactly the way ma_core.c Core_open
 * does - same dlsym list, same call order - and checks that what comes back
 * out the far side is the real core's, unaltered.
 *
 * The order matters. minarch calls retro_get_system_info *before* any of the
 * retro_set_* registration functions, which is why the shim loads lazily.
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "libretro.h"

static int failures = 0;

#define CHECK(cond, ...)                                    \
	do {                                                    \
		if (!(cond)) {                                      \
			printf("FAIL: ");                               \
			printf(__VA_ARGS__);                            \
			printf("\n");                                   \
			failures++;                                     \
		}                                                   \
	} while (0)

static bool fe_environment(unsigned cmd, void* data) {
	printf("fe:environment cmd=%u\n", cmd);
	return false;
}
static void fe_video_refresh(const void* d, unsigned w, unsigned h, size_t p) {
	printf("fe:video_refresh %ux%u\n", w, h);
}
static void fe_audio_sample(int16_t l, int16_t r)                { printf("fe:audio_sample\n"); }
static size_t fe_audio_sample_batch(const int16_t* d, size_t f)  { printf("fe:audio_sample_batch %zu\n", f); return f; }
static void fe_input_poll(void)                                  { printf("fe:input_poll\n"); }
static int16_t fe_input_state(unsigned p, unsigned d, unsigned i, unsigned id) {
	printf("fe:input_state\n");
	return 42; // must arrive at the core intact
}

int main(int argc, char** argv) {
	if (argc < 2) {
		printf("usage: %s <shim.so> [frames] [ms_per_frame]\n", argv[0]);
		return 2;
	}
	// Link tests need the loop to run long enough for a peer to connect and
	// packets to cross; the passthrough test just wants a single frame.
	int frames = (argc > 2) ? atoi(argv[2]) : 1;
	int frame_ms = (argc > 3) ? atoi(argv[3]) : 0;

	void* handle = dlopen(argv[1], RTLD_LAZY);
	CHECK(handle != NULL, "dlopen(%s): %s", argv[1], dlerror());
	if (!handle) return 1;

	// Exactly the symbols ma_core.c resolves.
	void     (*init)(void)                                  = dlsym(handle, "retro_init");
	void     (*deinit)(void)                                = dlsym(handle, "retro_deinit");
	void     (*get_system_info)(struct retro_system_info*)  = dlsym(handle, "retro_get_system_info");
	void     (*get_system_av_info)(struct retro_system_av_info*) = dlsym(handle, "retro_get_system_av_info");
	void     (*set_controller_port_device)(unsigned, unsigned) = dlsym(handle, "retro_set_controller_port_device");
	void     (*reset)(void)                                 = dlsym(handle, "retro_reset");
	void     (*run)(void)                                   = dlsym(handle, "retro_run");
	size_t   (*serialize_size)(void)                        = dlsym(handle, "retro_serialize_size");
	bool     (*serialize)(void*, size_t)                    = dlsym(handle, "retro_serialize");
	bool     (*unserialize)(const void*, size_t)            = dlsym(handle, "retro_unserialize");
	void     (*cheat_reset)(void)                           = dlsym(handle, "retro_cheat_reset");
	void     (*cheat_set)(unsigned, bool, const char*)      = dlsym(handle, "retro_cheat_set");
	bool     (*load_game)(const struct retro_game_info*)    = dlsym(handle, "retro_load_game");
	bool     (*load_game_special)(unsigned, const struct retro_game_info*, size_t) = dlsym(handle, "retro_load_game_special");
	void     (*unload_game)(void)                           = dlsym(handle, "retro_unload_game");
	unsigned (*get_region)(void)                            = dlsym(handle, "retro_get_region");
	void*    (*get_memory_data)(unsigned)                   = dlsym(handle, "retro_get_memory_data");
	size_t   (*get_memory_size)(unsigned)                   = dlsym(handle, "retro_get_memory_size");

	void (*set_environment)(retro_environment_t)               = dlsym(handle, "retro_set_environment");
	void (*set_video_refresh)(retro_video_refresh_t)           = dlsym(handle, "retro_set_video_refresh");
	void (*set_audio_sample)(retro_audio_sample_t)             = dlsym(handle, "retro_set_audio_sample");
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t) = dlsym(handle, "retro_set_audio_sample_batch");
	void (*set_input_poll)(retro_input_poll_t)                 = dlsym(handle, "retro_set_input_poll");
	void (*set_input_state)(retro_input_state_t)               = dlsym(handle, "retro_set_input_state");

	CHECK(init && deinit && get_system_info && get_system_av_info && run && load_game &&
	      set_environment && set_video_refresh && set_audio_sample &&
	      set_audio_sample_batch && set_input_poll && set_input_state,
	      "shim is missing entry points minarch requires");

	CHECK(set_controller_port_device && reset && serialize_size && serialize &&
	      unserialize && cheat_reset && cheat_set && load_game_special &&
	      unload_game && get_region && get_memory_data && get_memory_size,
	      "shim is missing entry points minarch resolves but does not hard-require");

	// --- minarch's order starts here -------------------------------------

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	get_system_info(&info);

	// These feed core.extensions / core.need_fullpath, and minarch strcpys
	// valid_extensions unchecked.
	CHECK(info.library_name && strcmp(info.library_name, "FakeCore") == 0,
	      "library_name not forwarded verbatim (got %s)", info.library_name ? info.library_name : "(null)");
	CHECK(info.valid_extensions && strcmp(info.valid_extensions, "fake|bin") == 0,
	      "valid_extensions not forwarded verbatim");
	CHECK(info.need_fullpath == true, "need_fullpath not forwarded");

	set_environment(fe_environment);
	set_video_refresh(fe_video_refresh);
	set_audio_sample(fe_audio_sample);
	set_audio_sample_batch(fe_audio_sample_batch);
	set_input_poll(fe_input_poll);
	set_input_state(fe_input_state);

	init();

	struct retro_game_info game;
	memset(&game, 0, sizeof(game));
	game.path = "/fake/rom.bin";
	CHECK(load_game(&game) == true, "load_game did not forward its return value");

	struct retro_system_av_info av;
	memset(&av, 0, sizeof(av));
	get_system_av_info(&av);
	CHECK(av.geometry.base_width == 240 && av.geometry.base_height == 160,
	      "av_info geometry not forwarded (got %ux%u)", av.geometry.base_width, av.geometry.base_height);
	CHECK(av.timing.sample_rate == 44100.0, "av_info sample_rate not forwarded");

	for (int i = 0; i < frames; i++) {
		run();
		fflush(stdout);
		if (frame_ms) usleep(frame_ms * 1000);
	}

	size_t sz = serialize_size();
	CHECK(sz == 16, "serialize_size not forwarded (got %zu)", sz);

	unsigned char buf[16];
	CHECK(serialize(buf, sizeof(buf)) == true, "serialize did not forward");
	CHECK(buf[0] == 0xAB, "serialize did not write the core's bytes through");
	CHECK(unserialize(buf, sizeof(buf)) == true, "unserialize did not forward");

	CHECK(get_region() == RETRO_REGION_NTSC, "get_region not forwarded");
	CHECK(get_memory_size(RETRO_MEMORY_SAVE_RAM) == 16, "get_memory_size not forwarded");
	CHECK(get_memory_data(RETRO_MEMORY_SAVE_RAM) != NULL, "get_memory_data not forwarded");

	unload_game();
	deinit();

	printf(failures ? "\nRESULT: %d failure(s)\n" : "\nRESULT: ok (%d failures)\n", failures);
	return failures ? 1 : 0;
}
