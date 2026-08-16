/*
 * Can two independent instances of one libretro core coexist in a process?
 *
 * This is the question that decides whether dual-instance play is possible at
 * all. Cores keep their state in globals, and dlopen deduplicates by inode - so
 * loading the same file twice normally hands back the *same* instance, with both
 * "copies" mutating one set of globals.
 *
 * Two escapes are worth testing:
 *   - two physical copies of the .so, which are distinct inodes
 *   - dlmopen(LM_ID_NEWLM), a separate link namespace for the same file
 *
 * The test drives both instances with different inputs and checks their states
 * diverge. If they share globals the states stay identical - which looks like
 * success unless you check for it.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

typedef struct {
	void* handle;
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void   (*init)(void);
	void   (*deinit)(void);
	bool   (*load_game)(const struct retro_game_info*);
	void   (*run)(void);
	size_t (*serialize_size)(void);
	bool   (*serialize)(void*, size_t);
	int     id;
} Core;

static int buttons_for[2];   /* what each instance's "player" holds */
static int active;           /* which instance is currently running */

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
static int16_t input_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
	(void)port;(void)dev;(void)idx;
	/* Drive each instance differently so identical states prove sharing. */
	if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return buttons_for[active];
	return (buttons_for[active] >> id) & 1;
}

#define SYM(c, f, n) do { (c)->f = dlsym((c)->handle, n); if (!(c)->f) { \
	printf("  missing %s\n", n); return 0; } } while (0)

static int wire(Core* c) {
	SYM(c, set_environment,        "retro_set_environment");
	SYM(c, set_video_refresh,      "retro_set_video_refresh");
	SYM(c, set_audio_sample,       "retro_set_audio_sample");
	SYM(c, set_audio_sample_batch, "retro_set_audio_sample_batch");
	SYM(c, set_input_poll,         "retro_set_input_poll");
	SYM(c, set_input_state,        "retro_set_input_state");
	SYM(c, init,                   "retro_init");
	SYM(c, deinit,                 "retro_deinit");
	SYM(c, load_game,              "retro_load_game");
	SYM(c, run,                    "retro_run");
	SYM(c, serialize_size,         "retro_serialize_size");
	SYM(c, serialize,              "retro_serialize");

	c->set_environment(env_cb);
	c->set_video_refresh(vid_cb);
	c->set_audio_sample(aud_cb);
	c->set_audio_sample_batch(aud_batch_cb);
	c->set_input_poll(poll_cb);
	c->set_input_state(input_cb);
	return 1;
}

static unsigned hash_state(Core* c) {
	size_t sz = c->serialize_size();
	if (!sz) return 0;
	void* buf = malloc(sz);
	if (!buf) return 0;
	unsigned h = 2166136261u;
	if (c->serialize(buf, sz)) {
		const unsigned char* p = buf;
		for (size_t i = 0; i < sz; i++) { h ^= p[i]; h *= 16777619u; }
	}
	free(buf);
	return h;
}

int main(int argc, char** argv) {
	if (argc < 4) {
		printf("usage: %s <core.so> <copy.so> <rom>\n", argv[0]);
		return 2;
	}
	const char* rom_path = argv[3];

	FILE* rf = fopen(rom_path, "rb");
	if (!rf) { printf("cannot open rom\n"); return 1; }
	fseek(rf, 0, SEEK_END); long rom_len = ftell(rf); fseek(rf, 0, SEEK_SET);
	void* rom = malloc(rom_len);
	if (fread(rom, 1, rom_len, rf) != (size_t)rom_len) { printf("short read\n"); return 1; }
	fclose(rf);

	struct retro_game_info game = { rom_path, rom, (size_t)rom_len, NULL };

	for (int mode = 0; mode < 2; mode++) {
		Core a = {0}, b = {0};
		a.id = 0; b.id = 1;

		if (mode == 0) {
			printf("== two physical copies of the .so\n");
			a.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
			b.handle = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
		} else {
			printf("== dlmopen(LM_ID_NEWLM), same file\n");
			a.handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
			b.handle = dlmopen(LM_ID_NEWLM, argv[1], RTLD_NOW | RTLD_LOCAL);
		}

		if (!a.handle || !b.handle) {
			printf("  dlopen failed: %s\n", dlerror());
			continue;
		}
		printf("  handles %s\n", a.handle == b.handle ? "IDENTICAL (shared!)" : "distinct");

		if (!wire(&a) || !wire(&b)) continue;

		active = 0; a.init(); a.load_game(&game);
		active = 1; b.init(); b.load_game(&game);

		/* Hold different buttons and run both. Independent instances must end
		 * in different states. */
		buttons_for[0] = 0;
		buttons_for[1] = (1 << RETRO_DEVICE_ID_JOYPAD_START) | (1 << RETRO_DEVICE_ID_JOYPAD_A);

		for (int i = 0; i < 600; i++) {
			active = 0; a.run();
			active = 1; b.run();
		}

		active = 0; unsigned ha = hash_state(&a);
		active = 1; unsigned hb = hash_state(&b);

		printf("  state A=%08x  state B=%08x  -> %s\n", ha, hb,
		       (ha && hb && ha != hb) ? "INDEPENDENT" : "shared or broken");

		active = 0; a.deinit();
		active = 1; b.deinit();
		dlclose(a.handle); dlclose(b.handle);
	}

	free(rom);
	return 0;
}
