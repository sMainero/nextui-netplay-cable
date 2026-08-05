/*
 * A minimal libretro core, used only to verify that the shim forwards
 * correctly in both directions. Every entry point records that it was reached;
 * retro_run exercises the frontend callbacks so the reverse path is covered too.
 */

#include <stdio.h>
#include <string.h>

#include "libretro.h"

#define HIT(name) printf("core:%s\n", name)

static retro_environment_t        cb_environment;
static retro_video_refresh_t      cb_video_refresh;
static retro_audio_sample_t       cb_audio_sample;
static retro_audio_sample_batch_t cb_audio_sample_batch;
static retro_input_poll_t         cb_input_poll;
static retro_input_state_t        cb_input_state;

static unsigned char state_blob[16];

//////////////////////////////////////////////////////////////////////////////
// netpacket - what gpsp/gambatte use for link play
//////////////////////////////////////////////////////////////////////////////

static retro_netpacket_send_t         np_send;
static uint16_t                       np_local_id;
static int                            np_started;
static unsigned                       np_seq;

static void np_start(uint16_t client_id, retro_netpacket_send_t send_fn,
                     retro_netpacket_poll_receive_t poll_fn) {
	printf("core:np_start id=%u\n", client_id);
	np_send = send_fn;
	np_local_id = client_id;
	np_started = 1;
}

static void np_receive(const void* buf, size_t len, uint16_t client_id) {
	printf("core:np_recv from=%u len=%zu data=%.*s\n",
	       client_id, len, (int)len, (const char*)buf);
}

static void np_stop(void) { printf("core:np_stop\n"); np_started = 0; np_send = NULL; }
static void np_poll(void) { }
static bool np_connected(uint16_t id)    { printf("core:np_connected id=%u\n", id); return true; }
static void np_disconnected(uint16_t id) { printf("core:np_disconnected id=%u\n", id); }

static const struct retro_netpacket_callback np_callback = {
	np_start, np_receive, np_stop, np_poll, np_connected, np_disconnected, "faketest1"
};

void retro_set_environment(retro_environment_t cb) {
	HIT("set_environment");
	cb_environment = cb;
	// Real cores probe the frontend from inside set_environment; do the same so
	// the environment path is exercised.
	unsigned quirk = 0;
	cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &quirk);

	// Link-capable cores register here. A stock frontend returns false and the
	// core disables link; the shim answers true instead.
	if (cb(RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE, (void*)&np_callback))
		printf("core:netpacket_accepted\n");
	else
		printf("core:netpacket_refused\n");
}

void retro_set_video_refresh(retro_video_refresh_t cb)           { HIT("set_video_refresh");      cb_video_refresh = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb)             { HIT("set_audio_sample");       cb_audio_sample = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { HIT("set_audio_sample_batch"); cb_audio_sample_batch = cb; }
void retro_set_input_poll(retro_input_poll_t cb)                 { HIT("set_input_poll");         cb_input_poll = cb; }
void retro_set_input_state(retro_input_state_t cb)               { HIT("set_input_state");        cb_input_state = cb; }

void retro_init(void)   { HIT("init"); }
void retro_deinit(void) { HIT("deinit"); }

unsigned retro_api_version(void) { HIT("api_version"); return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info* info) {
	HIT("get_system_info");
	memset(info, 0, sizeof(*info));
	info->library_name     = "FakeCore";
	info->library_version  = "9.9";
	info->valid_extensions = "fake|bin";
	info->need_fullpath    = true;
	info->block_extract    = false;
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
	HIT("get_system_av_info");
	memset(info, 0, sizeof(*info));
	info->geometry.base_width   = 240;
	info->geometry.base_height  = 160;
	info->geometry.max_width    = 240;
	info->geometry.max_height   = 160;
	info->geometry.aspect_ratio = 1.5f;
	info->timing.fps            = 59.73;
	info->timing.sample_rate    = 44100.0;
}

void retro_set_controller_port_device(unsigned port, unsigned device) { HIT("set_controller_port_device"); }
void retro_reset(void) { HIT("reset"); }

void retro_run(void) {
	HIT("run");

	// Once linked, emit one identifiable packet per frame so the peer's log
	// proves data crossed the wire.
	if (np_started && np_send) {
		char msg[32];
		int n = snprintf(msg, sizeof(msg), "P%u-%u", np_local_id, np_seq++);
		np_send(RETRO_NETPACKET_RELIABLE, msg, (size_t)n, RETRO_NETPACKET_BROADCAST);
	}
	cb_input_poll();
	// The value the frontend returns has to survive the trip back through the
	// shim's input_state wrapper.
	printf("core:input_state=%d\n", (int)cb_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A));

	static const int16_t frame[240 * 160];
	cb_video_refresh(frame, 240, 160, 240 * sizeof(int16_t));

	static const int16_t audio[64];
	cb_audio_sample_batch(audio, 32);
	cb_audio_sample(0, 0);
}

size_t retro_serialize_size(void) { HIT("serialize_size"); return sizeof(state_blob); }

bool retro_serialize(void* data, size_t size) {
	HIT("serialize");
	if (size < sizeof(state_blob)) return false;
	memset(state_blob, 0xAB, sizeof(state_blob));
	memcpy(data, state_blob, sizeof(state_blob));
	return true;
}

bool retro_unserialize(const void* data, size_t size) {
	HIT("unserialize");
	if (size < sizeof(state_blob)) return false;
	memcpy(state_blob, data, sizeof(state_blob));
	return state_blob[0] == 0xAB;
}

void retro_cheat_reset(void) { HIT("cheat_reset"); }
void retro_cheat_set(unsigned index, bool enabled, const char* code) { HIT("cheat_set"); }

bool retro_load_game(const struct retro_game_info* game) { HIT("load_game"); return true; }
bool retro_load_game_special(unsigned t, const struct retro_game_info* i, size_t n) { HIT("load_game_special"); return false; }
void retro_unload_game(void) { HIT("unload_game"); }

unsigned retro_get_region(void) { HIT("get_region"); return RETRO_REGION_NTSC; }
void* retro_get_memory_data(unsigned id) { HIT("get_memory_data"); return state_blob; }
size_t retro_get_memory_size(unsigned id) { HIT("get_memory_size"); return sizeof(state_blob); }
