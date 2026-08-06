/*
 * Netplay.pak - session setup.
 *
 * Replaces hand-edited config files: pick host or join, discover the other
 * device, arm. Session setup lives here rather than in the shim because
 * minarch's GFX_/PAD_ are compiled into the executable and unreachable from a
 * loaded core (see docs/shim-architecture.md).
 */

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <msettings.h>
#include "defines.h"
#include "api.h"
#include "utils.h"

#include "netsetup.h"

typedef enum {
	SCREEN_MENU,
	SCREEN_CHECKS,
	SCREEN_HOSTING,
	SCREEN_JOINING,
	SCREEN_ARMED,
} Screen;

static int quit = 0;
static void on_signal(int s) { (void)s; quit = 1; }

static Screen screen = SCREEN_MENU;
static int    menu_sel = 0;
static char   status[128] = "";

static NS_Check checks[8];
static int      check_count = 0;
static NS_CheckResult check_worst = NS_CHECK_OK;

static NS_Peer peers[NS_MAX_PEERS];
static int     peer_count = 0;
static int     peer_sel = 0;

static char local_ip[NS_IP_LEN] = "?";

//////////////////////////////////////////////////////////////////////////////
// drawing helpers
//////////////////////////////////////////////////////////////////////////////

static void draw_title(SDL_Surface* screen_s, const char* title) {
	SDL_Surface* t = TTF_RenderUTF8_Blended(font.large, title, COLOR_WHITE);
	if (t) {
		SDL_BlitSurface(t, NULL, screen_s,
		                &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), SCALE1(PADDING + 4)});
		SDL_FreeSurface(t);
	}
}

static int draw_line(SDL_Surface* screen_s, const char* text, int y, SDL_Color color, bool small) {
	SDL_Surface* t = TTF_RenderUTF8_Blended(small ? font.small : font.large, text, color);
	if (t) {
		SDL_BlitSurface(t, NULL, screen_s,
		                &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y, 0, 0});
		y += t->h + SCALE1(4);
		SDL_FreeSurface(t);
	}
	return y;
}

static int draw_row(SDL_Surface* screen_s, const char* text, int y, bool selected) {
	SDL_Color color = selected ? COLOR_BLACK : COLOR_WHITE;
	if (selected) {
		int w, h;
		TTF_SizeUTF8(font.large, text, &w, &h);
		SDL_Rect pill = {
			SCALE1(PADDING), y - SCALE1(2),
			w + SCALE1(BUTTON_PADDING * 2), h + SCALE1(4)
		};
		GFX_blitPill(ASSET_WHITE_PILL, screen_s, &pill);
	}
	SDL_Surface* t = TTF_RenderUTF8_Blended(font.large, text, color);
	if (t) {
		SDL_BlitSurface(t, NULL, screen_s,
		                &(SDL_Rect){SCALE1(PADDING + BUTTON_PADDING), y, 0, 0});
		y += t->h + SCALE1(6);
		SDL_FreeSurface(t);
	}
	return y;
}

//////////////////////////////////////////////////////////////////////////////
// screens
//////////////////////////////////////////////////////////////////////////////

static const char* MENU_ITEMS[] = { "Host a game", "Join a game", "Turn off", "Run checks" };
#define MENU_COUNT 4

static void render(SDL_Surface* s) {
	GFX_clear(s);

	switch (screen) {
	case SCREEN_MENU: {
		draw_title(s, NS_isArmed() ? "Netplay - armed" : "Netplay");
		int y = SCALE1(PADDING + 44);
		for (int i = 0; i < MENU_COUNT; i++)
			y = draw_row(s, MENU_ITEMS[i], y, i == menu_sel);
		if (status[0]) draw_line(s, status, y + SCALE1(8), COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "EXIT", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_CHECKS: {
		draw_title(s, "Checks");
		int y = SCALE1(PADDING + 44);
		for (int i = 0; i < check_count; i++) {
			char line[160];
			const char* mark = checks[i].result == NS_CHECK_OK ? "OK  "
			                 : checks[i].result == NS_CHECK_WARN ? "WARN" : "FAIL";
			snprintf(line, sizeof(line), "%s  %s", mark, checks[i].label);
			y = draw_line(s, line, y,
			              checks[i].result == NS_CHECK_OK ? COLOR_WHITE : COLOR_GRAY, false);
			y = draw_line(s, checks[i].detail, y, COLOR_GRAY, true);
		}
		GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_HOSTING: {
		draw_title(s, "Hosting");
		int y = SCALE1(PADDING + 44);
		char line[128];
		snprintf(line, sizeof(line), "This device: %s", local_ip);
		y = draw_line(s, line, y, COLOR_WHITE, false);
		y += SCALE1(6);
		y = draw_line(s, "On the other device choose Join.", y, COLOR_GRAY, true);
		y = draw_line(s, "Then launch the same game on both.", y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_JOINING: {
		draw_title(s, "Join a game");
		int y = SCALE1(PADDING + 44);
		if (peer_count == 0) {
			y = draw_line(s, "Looking for a host...", y, COLOR_WHITE, false);
			y += SCALE1(4);
			draw_line(s, "The other device must be on its Hosting screen.", y, COLOR_GRAY, true);
		} else {
			for (int i = 0; i < peer_count; i++) {
				char line[128];
				snprintf(line, sizeof(line), "%s  (%s)", peers[i].ip, peers[i].platform);
				y = draw_row(s, line, y, i == peer_sel);
			}
		}
		GFX_blitButtonGroup((char*[]){"B", "BACK", "A", "SELECT", NULL}, 1, s, 1);
		break;
	}
	case SCREEN_ARMED: {
		draw_title(s, "Ready");
		int y = SCALE1(PADDING + 44);
		y = draw_line(s, status, y, COLOR_WHITE, false);
		y += SCALE1(6);
		y = draw_line(s, "Launch the same game on both devices.", y, COLOR_GRAY, true);
		draw_line(s, "Re-open this app to turn netplay off.", y, COLOR_GRAY, true);
		GFX_blitButtonGroup((char*[]){"B", "BACK", NULL}, 1, s, 1);
		break;
	}
	}

	GFX_flip(s);
}

//////////////////////////////////////////////////////////////////////////////

static void do_arm(NS_Role role, const char* peer) {
	char err[96] = "";

	check_count = NS_runChecks(checks, 8, &check_worst);
	if (check_worst == NS_CHECK_FAIL) {
		snprintf(status, sizeof(status), "Checks failed - not arming");
		screen = SCREEN_CHECKS;
		return;
	}

	if (!NS_arm(role, peer, err, sizeof(err))) {
		snprintf(status, sizeof(status), "%s", err[0] ? err : "could not arm");
		screen = SCREEN_MENU;
		return;
	}

	if (role == NS_ROLE_HOST) snprintf(status, sizeof(status), "Hosting on %s", local_ip);
	else                      snprintf(status, sizeof(status), "Joined %s", peer);
	screen = SCREEN_ARMED;
}

int main(int argc, char* argv[]) {
	(void)argc; (void)argv;

	SDL_Surface* s = GFX_init(MODE_MAIN);
	PAD_init();
	PWR_init();
	InitSettings();

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	NS_init();
	if (!NS_localIP(local_ip, sizeof(local_ip))) snprintf(local_ip, sizeof(local_ip), "no network");
	if (NS_isArmed()) snprintf(status, sizeof(status), "A session is already set up.");

	int dirty = 1;
	while (!quit) {
		PAD_poll();

		switch (screen) {
		case SCREEN_MENU:
			if (PAD_justPressed(BTN_UP))   { menu_sel = (menu_sel + MENU_COUNT - 1) % MENU_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN)) { menu_sel = (menu_sel + 1) % MENU_COUNT; dirty = 1; }
			if (PAD_justPressed(BTN_B))    quit = 1;
			if (PAD_justPressed(BTN_A)) {
				status[0] = '\0';
				if (menu_sel == 0) {
					NS_announceStart();
					screen = SCREEN_HOSTING;
					do_arm(NS_ROLE_HOST, NULL);
					if (screen == SCREEN_ARMED) screen = SCREEN_HOSTING;
				} else if (menu_sel == 1) {
					peer_count = 0; peer_sel = 0;
					NS_discoverTick(NULL, 0);
					NS_discoverStart();
					screen = SCREEN_JOINING;
				} else if (menu_sel == 2) {
					NS_disarm();
					snprintf(status, sizeof(status), "Netplay off. Launches are stock again.");
				} else {
					check_count = NS_runChecks(checks, 8, &check_worst);
					screen = SCREEN_CHECKS;
				}
				dirty = 1;
			}
			break;

		case SCREEN_CHECKS:
			if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_A)) { screen = SCREEN_MENU; dirty = 1; }
			break;

		case SCREEN_HOSTING:
			NS_announceTick();
			if (PAD_justPressed(BTN_B)) {
				NS_announceStop();
				screen = SCREEN_MENU;
				dirty = 1;
			}
			break;

		case SCREEN_JOINING: {
			int n = NS_discoverTick(peers, NS_MAX_PEERS);
			if (n != peer_count) { peer_count = n; dirty = 1; }
			if (PAD_justPressed(BTN_UP) && peer_count)   { peer_sel = (peer_sel + peer_count - 1) % peer_count; dirty = 1; }
			if (PAD_justPressed(BTN_DOWN) && peer_count) { peer_sel = (peer_sel + 1) % peer_count; dirty = 1; }
			if (PAD_justPressed(BTN_B)) { NS_discoverStop(); screen = SCREEN_MENU; dirty = 1; }
			if (PAD_justPressed(BTN_A) && peer_count) {
				NS_discoverStop();
				do_arm(NS_ROLE_CLIENT, peers[peer_sel].ip);
				dirty = 1;
			}
			break;
		}

		case SCREEN_ARMED:
			NS_announceTick(); /* harmless when not hosting */
			if (PAD_justPressed(BTN_B) || PAD_justPressed(BTN_A)) { screen = SCREEN_MENU; dirty = 1; }
			break;
		}

		PWR_update(&dirty, NULL, NULL, NULL);

		if (dirty) { render(s); dirty = 0; }
		else GFX_sync();
	}

	NS_announceStop();
	NS_discoverStop();

	QuitSettings();
	PWR_quit();
	PAD_quit();
	GFX_quit();
	return 0;
}
