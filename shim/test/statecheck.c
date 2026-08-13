/*
 * statecheck - is a libretro core's save state comparable between two builds?
 *
 * Shared-screen netplay compares a hash of the serialized state across two
 * devices. That only means anything if the state is (a) a fixpoint under
 * serialize/unserialize on each device and (b) produced identically by both
 * builds from the same starting point. A core can fail either while emulating
 * perfectly - a pointer or some host-specific padding inside the blob is enough
 * - and the frontend cannot tell that apart from real divergence.
 *
 * Runs headless with stub callbacks, so it needs no frontend and can be driven
 * over ssh.
 *
 *   statecheck <core.so> <rom> [frames] [--save out] [--load in]
 *
 *   --save  after running <frames>, write the state to a file
 *   --load  load a state before running, so two devices can start identically
 *   --preframes N  run N frames before loading, to test whether unserialize is
 *                  a pure function of the blob
 *   --serprobe N  is retro_serialize side-effect free? Run N frames twice from
 *                 one state, once with an extra serialize call in between, and
 *                 compare. Netplay hashes one side more often than the other
 *                 whenever a check is missed, so a serialize that perturbs
 *                 state desyncs a session that is otherwise in lockstep.
 *   --amplify N  run N more frames down both the host and client paths and
 *                report whether a non-fixpoint state stays put or diverges
 *
 * Prints a round-trip verdict and an FNV-1a of the final state. Run it on both
 * devices with the same --load file and compare the hashes: equal means the two
 * builds emulate this ROM identically and netplay's own comparison is sound.
 */

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"

static void* handle;
#define SYM(t, n) ((t)dlsym(handle, n))

static char sysdir[512] = ".";

/* Core options, given as NAME=VALUE pairs in $STATECHECK_VARS separated by ';'.
 *
 * Answering false to every GET_VARIABLE leaves a core on its compiled-in
 * defaults, and those are not what a frontend produces. pcsx_rearmed defaults
 * to a threaded SPU and 'auto' threaded rendering - auto meaning "on if the
 * machine has two cores" - so it spawns a worker that races the emulation
 * thread, and the serialized state then differs between two runs of the same
 * binary on the same input. That looks exactly like a non-deterministic core
 * while actually being a harness that never configured it. A frontend always
 * answers this callback; a harness that does not is not testing the core as it
 * is really run. */
static char vars_buf[1024];
static struct { const char* key; const char* value; } vars[32];
static int var_count = -1;

static void vars_init(void) {
	var_count = 0;
	const char* src = getenv("STATECHECK_VARS");
	if (!src) return;
	snprintf(vars_buf, sizeof(vars_buf), "%s", src);
	char* p = vars_buf;
	while (p && *p && var_count < 32) {
		char* semi = strchr(p, ';');
		if (semi) *semi++ = '\0';
		char* eq = strchr(p, '=');
		if (eq) {
			*eq = '\0';
			vars[var_count].key = p;
			vars[var_count].value = eq + 1;
			var_count++;
		}
		p = semi;
	}
}

static const char* var_lookup(const char* key) {
	if (var_count < 0) vars_init();
	for (int i = 0; i < var_count; i++)
		if (!strcmp(vars[i].key, key)) return vars[i].value;
	return NULL;
}

static bool env_cb(unsigned cmd, void* data) {
	switch (cmd) {
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:  return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:     return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable* v = (struct retro_variable*)data;
		if (!v || !v->key) return false;
		const char* val = var_lookup(v->key);
		if (!val) return false;            /* unset - core default */
		v->value = val;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		if (data) *(bool*)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		if (data) *(const char**)data = sysdir;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		if (data) *(bool*)data = true;
		return true;
	default: return false;
	}
}

static void  video_cb(const void* d, unsigned w, unsigned h, size_t p) { (void)d;(void)w;(void)h;(void)p; }
static void  audio_cb(int16_t l, int16_t r) { (void)l;(void)r; }
static size_t batch_cb(const int16_t* d, size_t f) { (void)d; return f; }
static void  poll_cb(void) {}
static int16_t input_cb(unsigned p, unsigned d, unsigned i, unsigned id) {
	(void)p;(void)d;(void)i;(void)id; return 0;   /* neutral, so runs are comparable */
}

static uint32_t fnv(const void* p, size_t n) {
	const uint8_t* b = p;
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
	return h;
}

int main(int argc, char** argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: %s <core.so> <rom> [frames] [--save f] [--load f]\n", argv[0]);
		return 2;
	}
	const char* corepath = argv[1];
	const char* rompath  = argv[2];
	unsigned frames = (argc > 3 && argv[3][0] != '-') ? (unsigned)atoi(argv[3]) : 600;
	const char* savepath = NULL, *loadpath = NULL;
	unsigned amplify = 0;
	unsigned preframes = 0;
	unsigned serprobe = 0;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--save") && i + 1 < argc) savepath = argv[++i];
		if (!strcmp(argv[i], "--load") && i + 1 < argc) loadpath = argv[++i];
		if (!strcmp(argv[i], "--amplify") && i + 1 < argc) amplify = (unsigned)atoi(argv[++i]);
		if (!strcmp(argv[i], "--preframes") && i + 1 < argc) preframes = (unsigned)atoi(argv[++i]);
		if (!strcmp(argv[i], "--serprobe") && i + 1 < argc) serprobe = (unsigned)atoi(argv[++i]);
	}
	snprintf(sysdir, sizeof(sysdir), "%s", getenv("SYSDIR") ? getenv("SYSDIR") : "/tmp");

	handle = dlopen(corepath, RTLD_NOW | RTLD_LOCAL);
	if (!handle) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

	void (*set_env)(retro_environment_t)        = SYM(void(*)(retro_environment_t), "retro_set_environment");
	void (*set_video)(retro_video_refresh_t)    = SYM(void(*)(retro_video_refresh_t), "retro_set_video_refresh");
	void (*set_audio)(retro_audio_sample_t)     = SYM(void(*)(retro_audio_sample_t), "retro_set_audio_sample");
	void (*set_batch)(retro_audio_sample_batch_t) = SYM(void(*)(retro_audio_sample_batch_t), "retro_set_audio_sample_batch");
	void (*set_poll)(retro_input_poll_t)        = SYM(void(*)(retro_input_poll_t), "retro_set_input_poll");
	void (*set_input)(retro_input_state_t)      = SYM(void(*)(retro_input_state_t), "retro_set_input_state");
	void (*init)(void)                          = SYM(void(*)(void), "retro_init");
	bool (*load_game)(const struct retro_game_info*) = SYM(bool(*)(const struct retro_game_info*), "retro_load_game");
	void (*run)(void)                           = SYM(void(*)(void), "retro_run");
	size_t (*ser_size)(void)                    = SYM(size_t(*)(void), "retro_serialize_size");
	bool (*ser)(void*, size_t)                  = SYM(bool(*)(void*, size_t), "retro_serialize");
	bool (*unser)(const void*, size_t)          = SYM(bool(*)(const void*, size_t), "retro_unserialize");
	void (*get_info)(struct retro_system_info*) = SYM(void(*)(struct retro_system_info*), "retro_get_system_info");

	if (!set_env || !init || !load_game || !run || !ser_size || !ser || !unser) {
		fprintf(stderr, "core is missing required entry points\n");
		return 1;
	}

	struct retro_system_info info;
	memset(&info, 0, sizeof(info));
	if (get_info) get_info(&info);
	printf("core     : %s %s\n", info.library_name ? info.library_name : "?",
	       info.library_version ? info.library_version : "?");

	set_env(env_cb);
	init();
	if (set_video) set_video(video_cb);
	if (set_audio) set_audio(audio_cb);
	if (set_batch) set_batch(batch_cb);
	if (set_poll)  set_poll(poll_cb);
	if (set_input) set_input(input_cb);

	struct retro_game_info game;
	memset(&game, 0, sizeof(game));
	game.path = rompath;
	void* rom_data = NULL;
	if (get_info && !info.need_fullpath) {
		FILE* rom = fopen(rompath, "rb");
		if (!rom || fseek(rom, 0, SEEK_END) != 0) {
			fprintf(stderr, "cannot read %s\n", rompath);
			if (rom) fclose(rom);
			return 1;
		}
		long length = ftell(rom);
		if (length <= 0 || fseek(rom, 0, SEEK_SET) != 0 ||
		    !(rom_data = malloc((size_t)length)) ||
		    fread(rom_data, 1, (size_t)length, rom) != (size_t)length) {
			fprintf(stderr, "cannot load %s into memory\n", rompath);
			fclose(rom);
			free(rom_data);
			return 1;
		}
		fclose(rom);
		game.data = rom_data;
		game.size = (size_t)length;
	}
	if (!load_game(&game)) { fprintf(stderr, "retro_load_game failed for %s\n", rompath); return 1; }

	size_t sz = ser_size();
	printf("state    : %zu bytes\n", sz);
	if (!sz) return 1;

	/* Run before loading, to vary the state a load lands on top of. If
	 * unserialize is a pure function of its bytes the result is unaffected;
	 * if it leaves anything of the previous state behind, it is not - and the
	 * host and client cannot be equalised just by both loading the same blob. */
	for (unsigned i = 0; i < preframes; i++) run();
	if (preframes) printf("preframes: %u\n", preframes);

	/* Optional identical starting point, so two devices can be compared. */
	if (loadpath) {
		FILE* f = fopen(loadpath, "rb");
		if (!f) { fprintf(stderr, "cannot open %s\n", loadpath); return 1; }
		void* in = malloc(sz);
		size_t got = fread(in, 1, sz, f);
		fclose(f);
		if (got != sz) { fprintf(stderr, "state file is %zu bytes, core wants %zu\n", got, sz); return 1; }
		if (!unser(in, sz)) { fprintf(stderr, "retro_unserialize failed\n"); return 1; }
		printf("loaded   : %s (%08x)\n", loadpath, fnv(in, sz));
		free(in);
	}

	for (unsigned i = 0; i < frames; i++) run();

	void* a = malloc(sz);
	void* b = malloc(sz);
	if (!a || !b) return 1;
	memset(a, 0, sz);
	if (!ser(a, sz)) { fprintf(stderr, "retro_serialize failed\n"); return 1; }
	printf("after %-4u: %08x\n", frames, fnv(a, sz));

	/* Round trip: load what we just wrote, write it again, compare. */
	if (!unser(a, sz)) { fprintf(stderr, "retro_unserialize failed\n"); return 1; }
	memset(b, 0, sz);
	if (!ser(b, sz)) { fprintf(stderr, "retro_serialize failed\n"); return 1; }

	size_t diff = 0, first = (size_t)-1;
	for (size_t i = 0; i < sz; i++) {
		if (((uint8_t*)a)[i] != ((uint8_t*)b)[i]) { if (first == (size_t)-1) first = i; diff++; }
	}
	if (diff) printf("roundtrip: DIFFERS %zu/%zu bytes, first at %zu\n", diff, sz, first);
	else      printf("roundtrip: exact\n");

	/* Does that round-trip difference matter, or is it inert?
	 *
	 * This is the netplay handshake in one process. The host serializes its
	 * state and keeps running from what it already had; the client runs from
	 * the same bytes loaded back in. If a round trip is not a fixpoint those are
	 * not the same starting state, and the question is whether the difference
	 * stays put or grows into real divergence.
	 *
	 * Both paths run the same number of frames with the same (neutral) input, so
	 * any difference in the result is the state difference amplifying. */
	if (serprobe) {
		void* snap = malloc(sz);
		void* r1 = malloc(sz);
		void* r2 = malloc(sz);
		if (snap && r1 && r2) {
			memset(snap, 0, sz);
			ser(snap, sz);

			/* clean: no extra serialize between the snapshot and the run */
			unser(snap, sz);
			for (unsigned i = 0; i < serprobe; i++) run();
			memset(r1, 0, sz); ser(r1, sz);

			/* probed: one extra serialize, exactly as a missed sync check
			 * leaves the host having done and the client not */
			unser(snap, sz);
			void* throwaway = malloc(sz);
			if (throwaway) { memset(throwaway, 0, sz); ser(throwaway, sz); free(throwaway); }
			for (unsigned i = 0; i < serprobe; i++) run();
			memset(r2, 0, sz); ser(r2, sz);

			size_t d = 0, f = (size_t)-1;
			for (size_t i = 0; i < sz; i++)
				if (((uint8_t*)r1)[i] != ((uint8_t*)r2)[i]) { if (f == (size_t)-1) f = i; d++; }
			printf("serprobe : after %u frames, clean=%08x probed=%08x\n",
			       serprobe, fnv(r1, sz), fnv(r2, sz));
			if (d) printf("serprobe : DIFFERS %zu/%zu bytes, first at %zu "
			              "- retro_serialize is NOT side-effect free\n", d, sz, f);
			else   printf("serprobe : identical - retro_serialize is side-effect free\n");
		}
		free(snap); free(r1); free(r2);
	}

	if (amplify) {
		void* snap = malloc(sz);
		if (snap) {
			memset(snap, 0, sz);
			if (ser(snap, sz)) {
				/* host path: carry on from the state we already hold */
				for (unsigned i = 0; i < amplify; i++) run();
				void* h1 = malloc(sz);
				memset(h1, 0, sz);
				ser(h1, sz);
				uint32_t hash_host = fnv(h1, sz);

				/* client path: same bytes, loaded back first */
				unser(snap, sz);
				for (unsigned i = 0; i < amplify; i++) run();
				void* h2 = malloc(sz);
				memset(h2, 0, sz);
				ser(h2, sz);
				uint32_t hash_client = fnv(h2, sz);

				size_t d2 = 0, f2 = (size_t)-1;
				for (size_t i = 0; i < sz; i++) {
					if (((uint8_t*)h1)[i] != ((uint8_t*)h2)[i]) {
						if (f2 == (size_t)-1) f2 = i;
						d2++;
					}
				}
				printf("amplify  : after %u more frames, host=%08x client=%08x\n",
				       amplify, hash_host, hash_client);
				if (d2) printf("amplify  : DIVERGED %zu/%zu bytes, first at %zu "
				               "- the round-trip difference is real state\n", d2, sz, f2);
				else    printf("amplify  : identical - the round-trip difference is inert\n");
				free(h1); free(h2);
			}
			free(snap);
		}
	}

	if (savepath) {
		FILE* f = fopen(savepath, "wb");
		if (f) { fwrite(a, 1, sz, f); fclose(f); printf("saved    : %s\n", savepath); }
	}

	free(a); free(b);
	return 0;
}
