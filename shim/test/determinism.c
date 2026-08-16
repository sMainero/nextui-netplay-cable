/*
 * Does this core compute identically on different devices?
 *
 * Dual-instance play has both devices simulating both consoles, so every device
 * must reach bit-identical state from identical inputs. That is the assumption
 * that just sank cross-architecture shared-screen netplay: picodrive emulates
 * the 68000 with a C interpreter on aarch64 and a hand-written ARM recompiler on
 * armv7, so the two never agreed.
 *
 * Run this with the same core, ROM and frame count on two devices and compare
 * the checkpoint hashes. Inputs are derived from the frame number so the
 * sequence is identical everywhere without needing to be recorded.
 *
 *   determinism <core.so> <rom> [frames]
 */

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

#define CHECKPOINT 500

static void *(*p_dl)(void);
static unsigned cur_frame;

static bool env_cb(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data) *(bool*)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (data) *(const char**)data = "/tmp";
		return true;
	default: return false;
	}
}
static void vid_cb(const void* d, unsigned w, unsigned h, size_t p) { (void)d;(void)w;(void)h;(void)p; }
static void aud_cb(int16_t l, int16_t r) { (void)l;(void)r; }
static size_t aud_batch_cb(const int16_t* d, size_t f) { (void)d; return f; }
static void poll_cb(void) {}

/* Inputs from the frame number: identical on every device, no recording needed,
 * and varied enough that the game actually advances rather than idling on a
 * title screen where divergence would never show. */
static int16_t input_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
	(void)dev; (void)idx;
	if (port != 0) return 0;

	unsigned f = cur_frame;
	unsigned mask = 0;
	if (f > 60 && f < 90) mask |= 1 << RETRO_DEVICE_ID_JOYPAD_START;
	if ((f / 17) % 4 == 0) mask |= 1 << RETRO_DEVICE_ID_JOYPAD_LEFT;
	if ((f / 17) % 4 == 1) mask |= 1 << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	if ((f / 13) % 5 == 0) mask |= 1 << RETRO_DEVICE_ID_JOYPAD_A;
	if ((f / 29) % 7 == 0) mask |= 1 << RETRO_DEVICE_ID_JOYPAD_DOWN;

	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)mask;
	return (mask >> id) & 1;
}

int main(int argc, char** argv) {
	if (argc < 3) { printf("usage: %s <core.so> <rom> [frames]\n", argv[0]); return 2; }
	unsigned frames = (argc > 3) ? (unsigned)atoi(argv[3]) : 3000;

	void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }

	void (*set_env)(retro_environment_t)               = dlsym(h, "retro_set_environment");
	void (*set_vid)(retro_video_refresh_t)             = dlsym(h, "retro_set_video_refresh");
	void (*set_aud)(retro_audio_sample_t)              = dlsym(h, "retro_set_audio_sample");
	void (*set_audb)(retro_audio_sample_batch_t)       = dlsym(h, "retro_set_audio_sample_batch");
	void (*set_poll)(retro_input_poll_t)               = dlsym(h, "retro_set_input_poll");
	void (*set_inp)(retro_input_state_t)               = dlsym(h, "retro_set_input_state");
	void   (*core_init)(void)                          = dlsym(h, "retro_init");
	bool   (*load_game)(const struct retro_game_info*) = dlsym(h, "retro_load_game");
	void   (*run)(void)                                = dlsym(h, "retro_run");
	size_t (*ser_size)(void)                           = dlsym(h, "retro_serialize_size");
	bool   (*ser)(void*, size_t)                       = dlsym(h, "retro_serialize");
	void   (*get_info)(struct retro_system_info*)      = dlsym(h, "retro_get_system_info");

	if (!set_env || !run || !ser || !load_game) { printf("core missing entry points\n"); return 1; }

	set_env(env_cb); set_vid(vid_cb); set_aud(aud_cb);
	set_audb(aud_batch_cb); set_poll(poll_cb); set_inp(input_cb);

	struct retro_system_info info; memset(&info, 0, sizeof(info));
	if (get_info) get_info(&info);
	printf("core: %s %s\n", info.library_name ? info.library_name : "?",
	       info.library_version ? info.library_version : "?");

	FILE* rf = fopen(argv[2], "rb");
	if (!rf) { printf("cannot open rom\n"); return 1; }
	fseek(rf, 0, SEEK_END); long len = ftell(rf); fseek(rf, 0, SEEK_SET);
	void* rom = malloc(len);
	if (fread(rom, 1, len, rf) != (size_t)len) { printf("short read\n"); return 1; }
	fclose(rf);

	core_init();
	struct retro_game_info game = { argv[2], rom, (size_t)len, NULL };
	if (!load_game(&game)) { printf("load_game failed\n"); return 1; }

	size_t sz = ser_size();
	void* buf = malloc(sz);
	printf("state size: %zu\n", sz);

	for (cur_frame = 0; cur_frame < frames; cur_frame++) {
		run();
		if ((cur_frame + 1) % CHECKPOINT == 0) {
			/* Zero first: cores do not necessarily write every byte of the
			 * buffer (struct padding), so hashing malloc'd memory makes the
			 * result vary run to run even on one device. */
			memset(buf, 0, sz);
			unsigned hash = 2166136261u;
			if (ser(buf, sz)) {
				const unsigned char* p = buf;
				for (size_t i = 0; i < sz; i++) { hash ^= p[i]; hash *= 16777619u; }
			}
			printf("frame %6u  %08x\n", cur_frame + 1, hash);
		}
	}
	/* Optional dump, so two runs can be compared byte for byte to find where
	 * they actually differ rather than only that they do. */
	if (argc > 4) {
		memset(buf, 0, sz);
		if (ser(buf, sz)) {
			FILE* df = fopen(argv[4], "wb");
			if (df) { fwrite(buf, 1, sz, df); fclose(df); printf("dumped %zu bytes\n", sz); }
		}
	}
	(void)p_dl;
	return 0;
}
