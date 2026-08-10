#include "netsetup.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <gnu/libc-version.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DISCOVERY_PORT  55438
#define DISCOVERY_MAGIC 0x4E504C4BU /* 'NPLK' - same family as the link */
#define ANNOUNCE_MS     500

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t version;
	uint32_t mode;
	uint32_t hotspot;
	char     platform[16];
	char     ssid[NS_SSID_LEN];
	char     psk[NS_PSK_LEN];
} NS_Announce;

static char ns_platform[16] = "tg5040";
static char ns_pak[256];
static char ns_sd[128] = "/mnt/SDCARD";

static int announce_fd = -1;
static int discover_fd = -1;
static struct timeval last_announce;
static NS_Mode announce_mode = NS_MODE_NETPLAY;
static char announce_ssid[NS_SSID_LEN];
static char announce_psk[NS_PSK_LEN];
static bool hotspot_running;   /* we are serving the ad hoc network */
static bool joined_hotspot;    /* we moved our client stack onto someone's */
static char ns_ap_if[64];      /* interface hostapd was given, for teardown */
static char joined_ssid[NS_SSID_LEN];  /* recorded into the session so the */
static char joined_psk[NS_PSK_LEN];    /* launch stub can rejoin per game */
static NS_ProgressFn ns_progress;      /* lets the UI animate a blocking join */

//////////////////////////////////////////////////////////////////////////////
// environment
//////////////////////////////////////////////////////////////////////////////

void NS_init(void) {
	const char* p = getenv("PLATFORM");
	if (p && p[0]) snprintf(ns_platform, sizeof(ns_platform), "%s", p);

	const char* sd = getenv("SDCARD_PATH");
	if (sd && sd[0]) snprintf(ns_sd, sizeof(ns_sd), "%s", sd);

	snprintf(ns_pak, sizeof(ns_pak), "%s/Tools/%s/Netplay.pak", ns_sd, ns_platform);
}

const char* NS_platform(void) { return ns_platform; }
const char* NS_pakPath(void)  { return ns_pak; }

/* Read the first non-loopback IPv4 from `ip addr`. Cheaper and more portable
 * across these firmwares than getifaddrs, which busybox images vary on. */
bool NS_localIP(char* out, int len) {
	FILE* f = popen("ip -4 addr show 2>/dev/null | grep -oE 'inet [0-9.]+' | grep -v '127\\.' | head -1 | cut -d' ' -f2", "r");
	if (!f) return false;
	bool ok = false;
	if (fgets(out, len, f)) {
		char* nl = strpbrk(out, "\r\n");
		if (nl) *nl = '\0';
		ok = out[0] != '\0';
	}
	pclose(f);
	return ok;
}

static bool file_exists(const char* p) {
	struct stat st;
	return stat(p, &st) == 0;
}

//////////////////////////////////////////////////////////////////////////////
// wifi power save
//
// The radio parks itself between beacons, which costs tens of milliseconds on
// a link that is otherwise idle - exactly the shape of traffic netplay makes,
// a few bytes per frame. On the Brick this is on by default and shows up as
// input jitter. Off while a session is armed, restored to whatever it was on
// disarm, because leaving it off costs battery.
//////////////////////////////////////////////////////////////////////////////

/* Goes to stderr, which launch.sh redirects into the pak's log. The ad hoc path
 * used to report only through on-screen status, so a failure left nothing to
 * read afterwards and every diagnosis started from scratch. */
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
static void ns_stamp(char* out, int len) {
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

static void ns_log(const char* fmt, ...) {
	char ts[48];
	ns_stamp(ts, sizeof(ts));

	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[%s] [netplay-app] ", ts);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

/* popen with a deadline.
 *
 * `iw dev wlan0 scan` can block indefinitely on these radios - it wedged the
 * app on the Join screen with no buttons working, because the read happens on
 * the thread that draws the UI. Nothing shelled out to from a screen may be
 * unbounded. Returns the number of lines read; kills the child on timeout.
 *
 * timeout(1) is not on these images, so the bound is enforced here: the pipe is
 * non-blocking and we give up on the clock. */
/* Defined with the join code below; the scan helpers above need it. */
static bool platform_ctrl_dir(char* out, int len);

static int popen_bounded(const char* cmd, int timeout_ms,
                         void (*on_line)(const char*, void*), void* ctx) {
	FILE* f = popen(cmd, "r");
	if (!f) return -1;

	int fd = fileno(f);
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	struct timeval start;
	gettimeofday(&start, NULL);

	char line[512];
	size_t used = 0;
	int lines = 0;

	for (;;) {
		struct timeval now;
		gettimeofday(&now, NULL);
		long ms = (now.tv_sec - start.tv_sec) * 1000L
		        + (now.tv_usec - start.tv_usec) / 1000L;
		if (ms > timeout_ms) {
			ns_log("command timed out after %dms: %.60s\n", timeout_ms, cmd);
			break;
		}

		char c;
		ssize_t n = read(fd, &c, 1);
		if (n == 1) {
			if (c == '\n' || used + 1 >= sizeof(line)) {
				line[used] = '\0';
				if (used && on_line) on_line(line, ctx);
				if (used) lines++;
				used = 0;
			} else {
				line[used++] = c;
			}
			continue;
		}
		if (n == 0) break;                               /* clean EOF */
		if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(20 * 1000); continue; }
		break;
	}

	pclose(f);
	return lines;
}

static void ps_state_path(char* out, int len) {
	snprintf(out, len, "%s/state/wifi_powersave", ns_pak);
}

/* "on" / "off", as `iw` reports it. */
static bool wifi_powerSaveGet(char* out, int len) {
	FILE* f = popen("iw dev wlan0 get power_save 2>/dev/null "
	                "| sed -n 's/.*Power save: *//p' | head -1", "r");
	if (!f) return false;
	bool ok = false;
	if (fgets(out, len, f)) {
		char* nl = strpbrk(out, "\r\n");
		if (nl) *nl = '\0';
		ok = out[0] != '\0';
	}
	pclose(f);
	return ok;
}

void NS_wifiPowerSaveDisable(void) {
	char prior[16] = "";
	char path[512];
	ps_state_path(path, sizeof(path));

	/* Record the prior value once. Re-arming without disarming first must not
	 * overwrite it with the "off" we ourselves set. */
	if (!file_exists(path) && wifi_powerSaveGet(prior, sizeof(prior))) {
		FILE* f = fopen(path, "w");
		if (f) { fprintf(f, "%s\n", prior); fclose(f); }
	}
	system("iw dev wlan0 set power_save off >/dev/null 2>&1");
}

void NS_wifiPowerSaveRestore(void) {
	char path[512];
	ps_state_path(path, sizeof(path));

	FILE* f = fopen(path, "r");
	if (f) {
		char prior[16] = "";
		if (fgets(prior, sizeof(prior), f)) {
			char* nl = strpbrk(prior, "\r\n");
			if (nl) *nl = '\0';
			if (!strcmp(prior, "on")) system("iw dev wlan0 set power_save on >/dev/null 2>&1");
		}
		fclose(f);
		remove(path);
	}
}

//////////////////////////////////////////////////////////////////////////////
// settings
//////////////////////////////////////////////////////////////////////////////

const char* const NS_INST_CORE[NS_INST_CORES] = { "gambatte", "gpsp", "mgba" };

/* Executable sharing is frozen and therefore defaults off. Compatibility cores
 * are local, pinned artifacts and are the safe default fallback. */
static NS_Settings ns_set = {
	.share_cores   = false,
	.compatibility_cores = true,
	.simple_client = false,
	.instanced     = NS_INST_OFF,
	.inst_core     = { false, false, false },
};
static bool ns_set_loaded = false;

static void settings_path(char* out, int len) {
	snprintf(out, len, "%s/state/settings", ns_pak);
}

static const char* compatibility_arch(void) {
	return !strcmp(ns_platform, "my282") ? "armv7" : "aarch64";
}

NS_Settings* NS_settings(void) {
	if (!ns_set_loaded) NS_settingsLoad();
	return &ns_set;
}

void NS_settingsLoad(void) {
	ns_set_loaded = true;              /* set first: a missing file means defaults */

	char path[512];
	settings_path(path, sizeof(path));
	FILE* f = fopen(path, "r");
	if (!f) return;

	char line[160];
	while (fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		const char* k = line;
		int v = atoi(eq + 1);

		if      (!strcmp(k, "share_cores"))   ns_set.share_cores = v != 0;
		else if (!strcmp(k, "compatibility_cores")) ns_set.compatibility_cores = v != 0;
		else if (!strcmp(k, "simple_client")) ns_set.simple_client = v != 0;
		else if (!strcmp(k, "instanced"))
			ns_set.instanced = (v < 0 || v > NS_INST_SELECTED) ? NS_INST_OFF : (NS_InstMode)v;
		else {
			for (int i = 0; i < NS_INST_CORES; i++) {
				char key[64];
				snprintf(key, sizeof(key), "inst_%s", NS_INST_CORE[i]);
				if (!strcmp(k, key)) ns_set.inst_core[i] = v != 0;
			}
		}
	}
	fclose(f);
}

void NS_settingsSave(void) {
	char path[512], tmp[540];
	settings_path(path, sizeof(path));
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);

	/* Write-then-rename: a half-written settings file read at next launch would
	 * silently reset to defaults, and losing "simple client" mid-session is the
	 * kind of thing nobody would connect back to a power cut. */
	FILE* f = fopen(tmp, "w");
	if (!f) { ns_log("cannot write settings\n"); return; }
	fprintf(f, "share_cores=%d\n",   ns_set.share_cores ? 1 : 0);
	fprintf(f, "compatibility_cores=%d\n", ns_set.compatibility_cores ? 1 : 0);
	fprintf(f, "simple_client=%d\n", ns_set.simple_client ? 1 : 0);
	fprintf(f, "instanced=%d\n",     (int)ns_set.instanced);
	for (int i = 0; i < NS_INST_CORES; i++)
		fprintf(f, "inst_%s=%d\n", NS_INST_CORE[i], ns_set.inst_core[i] ? 1 : 0);
	fclose(f);
	if (rename(tmp, path) != 0) remove(tmp);
}

/* Three places a core can legitimately live: the platform's own directory, the
 * cores this pak ships to replace them, and an EXTRAS pak that bundles its own
 * (which is where mgba is - MGBA.pak, not the system cores dir). */
bool NS_coreInstalled(const char* core) {
	char p[640];

	snprintf(p, sizeof(p), "%s/.system/%s/cores/%s_libretro.so", ns_sd, ns_platform, core);
	if (file_exists(p)) return true;

	snprintf(p, sizeof(p), "%s/cores/compatibility/%s/%s_libretro.so",
	         ns_pak, compatibility_arch(), core);
	if (file_exists(p)) return true;

	char cmd[768];
	snprintf(cmd, sizeof(cmd),
	         "ls '%s/Emus/%s'/*.pak/%s_libretro.so >/dev/null 2>&1", ns_sd, ns_platform, core);
	return system(cmd) == 0;
}

//////////////////////////////////////////////////////////////////////////////
// core manifest
//////////////////////////////////////////////////////////////////////////////

/* Cores the shim can drive. Kept in step with NETPLAY_CORES in the launcher
 * scripts; a core absent here is simply never offered for sharing. */
static const char* MANIFEST_CORES[] = {
	"fceumm", "picodrive", "snes9x", "snes9x2005", "gambatte", "gpsp",
	"mgba", "mednafen_supafaust", "fbneo", "pcsx_rearmed",
};

uint32_t NS_runtimeGlibc(void) {
	const char* v = gnu_get_libc_version();
	unsigned maj = 0, min = 0;
	if (v && sscanf(v, "%u.%u", &maj, &min) == 2) return maj * 1000 + min;
	return 0;
}

/* CRC32, size, ELF machine and glibc floor in one pass - the file is a few MB
 * and reading it four times would be four times the SD card wear. */
static bool scan_core_file(const char* path, NS_CoreInfo* out) {
	FILE* f = fopen(path, "rb");
	if (!f) return false;

	static uint32_t table[256];
	static int built = 0;
	if (!built) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			table[i] = c;
		}
		built = 1;
	}

	uint32_t crc = 0xFFFFFFFFu, best_glibc = 0;
	size_t total = 0;
	uint16_t machine = 0;
	char buf[16384];
	char tail[32];
	size_t taillen = 0;
	size_t n;

	while ((n = fread(buf, 1, sizeof(buf) - 1, f)) > 0) {
		if (total == 0 && n >= 20 && !memcmp(buf, "\177ELF", 4))
			machine = (uint16_t)((unsigned char)buf[18] | ((unsigned char)buf[19] << 8));
		for (size_t i = 0; i < n; i++) crc = table[(crc ^ (unsigned char)buf[i]) & 0xFF] ^ (crc >> 8);

		/* GLIBC_ strings, searched across the seam between reads. */
		char win[16384 + 32];
		memcpy(win, tail, taillen);
		memcpy(win + taillen, buf, n);
		size_t have = taillen + n;
		win[have] = '\0';
		for (size_t i = 0; i + 10 < have; i++) {
			if (memcmp(win + i, "GLIBC_", 6)) continue;
			unsigned maj = 0, min = 0;
			if (sscanf(win + i + 6, "%u.%u", &maj, &min) == 2) {
				uint32_t v = maj * 1000 + min;
				if (v > best_glibc) best_glibc = v;
			}
		}
		taillen = have > 32 ? 32 : have;
		memcpy(tail, win + have - taillen, taillen);
		total += n;
	}
	fclose(f);
	if (!total) return false;

	out->crc = crc ^ 0xFFFFFFFFu;
	out->size = (uint32_t)total;
	out->machine = machine;
	out->glibc = best_glibc;
	return true;
}

/* library_version without a ROM. */
static void probe_version(const char* path, char* out, int len) {
	struct core_system_info {
		const char* name;
		const char* version;
		const char* extensions;
		bool need_fullpath;
		bool block_extract;
	};
	out[0] = '\0';
	void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!h) { ns_log("manifest: %s will not load here (%s)\n", path, dlerror()); return; }
	void (*gsi)(struct core_system_info*);
	*(void**)&gsi = dlsym(h, "retro_get_system_info");
	if (gsi) {
		struct core_system_info i;
		memset(&i, 0, sizeof(i));
		gsi(&i);
		if (i.version) snprintf(out, len, "%s", i.version);
	}
	dlclose(h);
}

/* The device's installed core is always considered first. Compatibility cores
 * are deliberately excluded here; otherwise merely bundling one would make it
 * look like the device was already using it. */
static bool installed_core_path(const char* core, char* out, int len) {
	snprintf(out, len, "%s/.system/%s/cores/%s_libretro.so", ns_sd, ns_platform, core);
	if (file_exists(out)) return true;

	/* EXTRAS paks bundle their own - mgba lives in MGBA.pak, not the cores dir. */
	char cmd[700];
	snprintf(cmd, sizeof(cmd),
	         "ls '%s/Emus/%s'/*.pak/%s_libretro.so 2>/dev/null | head -1", ns_sd, ns_platform, core);
	FILE* p = popen(cmd, "r");
	if (!p) return false;
	bool ok = false;
	if (fgets(out, len, p)) {
		char* nl = strpbrk(out, "\r\n");
		if (nl) *nl = '\0';
		ok = out[0] != '\0';
	}
	pclose(p);
	return ok;
}

static bool compatibility_core_path(const char* core, char* out, int len) {
	snprintf(out, len, "%s/cores/compatibility/%s/%s_libretro.so",
	         ns_pak, compatibility_arch(), core);
	return file_exists(out);
}

int NS_coreManifest(NS_CoreInfo* out, int max) {
	int n = 0;
	for (size_t i = 0; i < sizeof(MANIFEST_CORES)/sizeof(MANIFEST_CORES[0]) && n < max; i++) {
		memset(&out[n], 0, sizeof(out[n]));
		snprintf(out[n].core, sizeof(out[n].core), "%s", MANIFEST_CORES[i]);

		char path[256];
		if (installed_core_path(MANIFEST_CORES[i], path, sizeof(path))) {
			snprintf(out[n].path, sizeof(out[n].path), "%s", path);
			if (scan_core_file(path, &out[n])) {
				probe_version(path, out[n].version, sizeof(out[n].version));
				out[n].installed = true;
			}
		}

		if (compatibility_core_path(MANIFEST_CORES[i], path, sizeof(path))) {
			NS_CoreInfo compat;
			memset(&compat, 0, sizeof(compat));
			if (scan_core_file(path, &compat)) {
				probe_version(path, out[n].compat_version, sizeof(out[n].compat_version));
				out[n].compat_available = out[n].compat_version[0] != '\0';
			}
		}

		if (!out[n].installed && !out[n].compat_available) continue;
		n++;
	}
	ns_log("manifest: %d core(s), runtime glibc %u.%u\n",
	       n, NS_runtimeGlibc() / 1000, NS_runtimeGlibc() % 1000);
	return n;
}

//////////////////////////////////////////////////////////////////////////////
// core compatibility negotiation
//////////////////////////////////////////////////////////////////////////////

#define CORE_MAGIC 0x4E504332u   /* 'NPC2': metadata-only compatibility protocol */
#define CORE_IO_MS   30000

typedef struct __attribute__((packed)) {
	char     core[32];
	char     version[32];
	char     compat_version[32];
	uint32_t crc;
	uint32_t size;
	uint32_t glibc;
	uint16_t machine;
	uint8_t  installed;
	uint8_t  compat_available;
} CoreWire;

static int core_listen_fd = -1;

/* Bounded, so neither side can be parked forever by a peer that stops talking
 * mid-transfer - the failure that has bitten this codebase repeatedly. */
static bool io_all(int fd, void* buf, size_t len, bool writing) {
	uint8_t* p = buf;
	size_t done = 0;
	struct timeval start, now;
	gettimeofday(&start, NULL);

	while (done < len) {
		gettimeofday(&now, NULL);
		long ms = (now.tv_sec - start.tv_sec) * 1000L + (now.tv_usec - start.tv_usec) / 1000L;
		if (ms > CORE_IO_MS) return false;

		ssize_t n = writing ? send(fd, p + done, len - done, MSG_NOSIGNAL)
		                    : recv(fd, p + done, len - done, 0);
		if (n > 0) { done += (size_t)n; continue; }
		if (n == 0) return false;                       /* peer closed */
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(20 * 1000); continue; }
		return false;
	}
	return true;
}

static bool send_manifest(int fd, NS_CoreInfo* m, int n) {
	uint32_t hdr[3] = { htonl(CORE_MAGIC), htonl((uint32_t)n),
	                    htonl(NS_settings()->compatibility_cores ? 1u : 0u) };
	if (!io_all(fd, hdr, sizeof(hdr), true)) return false;
	for (int i = 0; i < n; i++) {
		CoreWire w;
		memset(&w, 0, sizeof(w));
		snprintf(w.core, sizeof(w.core), "%s", m[i].core);
		snprintf(w.version, sizeof(w.version), "%s", m[i].version);
		snprintf(w.compat_version, sizeof(w.compat_version), "%s", m[i].compat_version);
		w.crc = htonl(m[i].crc);
		w.size = htonl(m[i].size);
		w.glibc = htonl(m[i].glibc);
		w.machine = htons(m[i].machine);
		w.installed = m[i].installed ? 1 : 0;
		w.compat_available = m[i].compat_available ? 1 : 0;
		if (!io_all(fd, &w, sizeof(w), true)) return false;
	}
	return true;
}

static int recv_manifest(int fd, NS_CoreInfo* out, int max, bool* peer_compat_enabled) {
	uint32_t hdr[3];
	if (!io_all(fd, hdr, sizeof(hdr), false)) return -1;
	if (ntohl(hdr[0]) != CORE_MAGIC) return -1;
	int n = (int)ntohl(hdr[1]);
	*peer_compat_enabled = ntohl(hdr[2]) != 0;
	if (n < 0 || n > NS_MAX_MANIFEST) return -1;

	int kept = 0;
	for (int i = 0; i < n; i++) {
		CoreWire w;
		if (!io_all(fd, &w, sizeof(w), false)) return -1;
		if (kept >= max) continue;
		w.core[sizeof(w.core) - 1] = '\0';
		w.version[sizeof(w.version) - 1] = '\0';
		w.compat_version[sizeof(w.compat_version) - 1] = '\0';
		memset(&out[kept], 0, sizeof(out[kept]));
		snprintf(out[kept].core, sizeof(out[kept].core), "%s", w.core);
		snprintf(out[kept].version, sizeof(out[kept].version), "%s", w.version);
		snprintf(out[kept].compat_version, sizeof(out[kept].compat_version), "%s", w.compat_version);
		out[kept].crc = ntohl(w.crc);
		out[kept].size = ntohl(w.size);
		out[kept].glibc = ntohl(w.glibc);
		out[kept].machine = ntohs(w.machine);
		out[kept].installed = w.installed != 0;
		out[kept].compat_available = w.compat_available != 0;
		kept++;
	}
	return kept;
}

static NS_CoreInfo* find_core(NS_CoreInfo* m, int n, const char* name) {
	for (int i = 0; i < n; i++) if (!strcmp(m[i].core, name)) return &m[i];
	return NULL;
}

static bool installed_builds_match(const char* core, const NS_CoreInfo* a, const NS_CoreInfo* b) {
	if (!a || !b || !a->installed || !b->installed) return false;
	/* Link cores need the pak's networking fixes, not merely the same upstream
	 * revision on both devices. A system core carrying the same marked build is
	 * fine; an unmarked stock build is not feature-compatible. */
	if (!strcmp(core, "gambatte") || !strcmp(core, "gpsp")) {
		if (!a->compat_available || !b->compat_available ||
		    strcmp(a->version, a->compat_version) || strcmp(b->version, b->compat_version))
			return false;
	}
	if (a->version[0] && b->version[0]) return !strcmp(a->version, b->version);
	return a->machine == b->machine && a->size == b->size && a->crc == b->crc;
}

static bool compatibility_builds_match(const NS_CoreInfo* a, const NS_CoreInfo* b) {
	return a && b && a->compat_available && b->compat_available &&
	       a->compat_version[0] && !strcmp(a->compat_version, b->compat_version);
}

/* Rewrite only the generated compatibility selections, preserving role,
 * transport and hand-edited options. Both copies must agree because the
 * launcher reads state/session while session.conf is the durable template. */
static bool write_compat_file(const char* path, const char selected[][32], int count) {
	char tmp[560];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* in = fopen(path, "r");
	if (!in) return false;
	FILE* out = fopen(tmp, "w");
	if (!out) { fclose(in); return false; }

	char line[512];
	while (fgets(line, sizeof(line), in))
		if (strncmp(line, "compat_core.", 12)) fputs(line, out);
	for (int i = 0; i < count; i++) fprintf(out, "compat_core.%s=1\n", selected[i]);

	fclose(in);
	if (fclose(out) != 0 || rename(tmp, path) != 0) {
		remove(tmp);
		return false;
	}
	return true;
}

static int select_compatibility(NS_CoreInfo* mine, int mn, NS_CoreInfo* theirs, int tn,
								bool peer_enabled) {
	char selected[NS_MAX_MANIFEST][32];
	int count = 0;
	bool enabled = NS_settings()->compatibility_cores && peer_enabled;

	for (size_t i = 0; i < sizeof(MANIFEST_CORES) / sizeof(MANIFEST_CORES[0]); i++) {
		NS_CoreInfo* a = find_core(mine, mn, MANIFEST_CORES[i]);
		NS_CoreInfo* b = find_core(theirs, tn, MANIFEST_CORES[i]);
		if (installed_builds_match(MANIFEST_CORES[i], a, b)) {
			ns_log("%s: installed builds match; keeping them\n", MANIFEST_CORES[i]);
			continue;
		}
		if (enabled && compatibility_builds_match(a, b)) {
			snprintf(selected[count++], sizeof(selected[0]), "%s", MANIFEST_CORES[i]);
			ns_log("%s: installed builds differ; both will use compatibility cores\n",
			       MANIFEST_CORES[i]);
		} else if (a && b && (a->installed || b->installed)) {
			ns_log("%s: installed builds differ and no common compatibility core is enabled\n",
			       MANIFEST_CORES[i]);
		}
	}

	char path[512];
	snprintf(path, sizeof(path), "%s/session.conf", ns_pak);
	if (!write_compat_file(path, selected, count)) {
		ns_log("cannot update compatibility selections in session.conf\n");
		return -1;
	}
	snprintf(path, sizeof(path), "%s/state/session", ns_pak);
	if (!write_compat_file(path, selected, count)) {
		ns_log("cannot update compatibility selections in active session\n");
		/* Do not leave a durable selection that disagrees with the active file. */
		snprintf(path, sizeof(path), "%s/session.conf", ns_pak);
		write_compat_file(path, selected, 0);
		return -1;
	}
	return count;
}

//////////////////////////////////////////////////////////////////////////////
// host side

void NS_compatServeStart(void) {
	if (core_listen_fd >= 0) return;

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return;
	int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	int fl = fcntl(fd, F_GETFL, 0);
	if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = INADDR_ANY;
	a.sin_port = htons(NS_CORE_PORT);
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(fd, 2) != 0) {
		close(fd);
		return;
	}
	core_listen_fd = fd;
	ns_log("core compatibility negotiation listening on %d\n", NS_CORE_PORT);
}

void NS_compatServeStop(void) {
	if (core_listen_fd >= 0) { close(core_listen_fd); core_listen_fd = -1; }
}

void NS_compatServeTick(void) {
	if (core_listen_fd < 0) return;

	int fd = accept(core_listen_fd, NULL, NULL);
	if (fd < 0) return;                       /* nothing waiting - normal */

	NS_CoreInfo mine[NS_MAX_MANIFEST];
	int n = NS_coreManifest(mine, NS_MAX_MANIFEST);

	NS_CoreInfo theirs[NS_MAX_MANIFEST];
	bool peer_compat_enabled = false;
	int tn = recv_manifest(fd, theirs, NS_MAX_MANIFEST, &peer_compat_enabled);
	if (tn < 0 ||
	    !send_manifest(fd, mine, n)) {
		ns_log("core compatibility negotiation: manifest failed\n");
		close(fd);
		return;
	}
	int selected = select_compatibility(mine, n, theirs, tn, peer_compat_enabled);
	if (selected < 0)
		ns_log("core compatibility negotiation: could not activate selections\n");
	else
		ns_log("core compatibility negotiation: %d fallback(s) selected\n", selected);

	/* Frozen core-sharing implementation. Executable files must never be
	 * accepted from an unauthenticated peer. Kept here, excluded from the build,
	 * so the setting and its former implementation can be revisited together. */
#if 0
	/* Serve requests until the client says it is done. The client drives,
	 * because it is the side that knows what it could not load. */
	for (;;) {
		uint8_t op;
		char name[32];
		if (!io_all(fd, &op, 1, false)) break;
		if (op == CORE_OP_DONE) break;
		if (!io_all(fd, name, sizeof(name), false)) break;
		name[sizeof(name) - 1] = '\0';

		if (op == CORE_OP_GET) {
			NS_CoreInfo* c = find_core(mine, n, name);
			uint32_t len = 0;
			uint8_t* buf = NULL;
			if (c) {
				FILE* f = fopen(c->path, "rb");
				if (f) {
					buf = malloc(c->size);
					if (buf && fread(buf, 1, c->size, f) == c->size) len = c->size;
					else { free(buf); buf = NULL; }
					fclose(f);
				}
			}
			uint32_t nl_len = htonl(len);
			if (!io_all(fd, &nl_len, 4, true)) { free(buf); break; }
			if (len && !io_all(fd, buf, len, true)) { free(buf); break; }
			ns_log("core exchange: sent %s (%u bytes)\n", name, len);
			free(buf);
		} else if (op == CORE_OP_PUT) {
			uint32_t len;
			if (!io_all(fd, &len, 4, false)) break;
			len = ntohl(len);
			if (!len || len > CORE_MAX_BYTES) break;
			uint8_t* buf = malloc(len);
			if (!buf || !io_all(fd, buf, len, false)) { free(buf); break; }

			char dir[512], path[600], cmd[700];
			staged_dir(dir, sizeof(dir));
			snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
			system(cmd);
			snprintf(path, sizeof(path), "%s/%s_libretro.so", dir, name);
			FILE* out = fopen(path, "wb");
			if (out) {
				size_t w = fwrite(buf, 1, len, out);
				fclose(out);
				if (w == len) ns_log("core exchange: staged %s from peer (%u bytes)\n", name, len);
				else remove(path);
			}
			free(buf);
		} else break;
	}
#endif
	close(fd);
}

//////////////////////////////////////////////////////////////////////////////
// client side

/* Compare installed builds first, then select the pak fallback on both sides
 * only when both settings and both local compatibility artifacts permit it. */
int NS_compatSync(const char* host_ip, char* err, int errlen) {
	NS_CoreInfo mine[NS_MAX_MANIFEST];
	int n = NS_coreManifest(mine, NS_MAX_MANIFEST);
	if (n <= 0) { snprintf(err, errlen, "no cores found locally"); return -1; }

	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { snprintf(err, errlen, "no socket"); return -1; }

	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(NS_CORE_PORT);
	if (inet_pton(AF_INET, host_ip, &a.sin_addr) != 1) {
		close(fd);
		snprintf(err, errlen, "bad host address");
		return -1;
	}

	struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) {
		close(fd);
		snprintf(err, errlen, "host is not serving cores");
		return -1;
	}

	NS_CoreInfo theirs[NS_MAX_MANIFEST];
	bool peer_compat_enabled = false;
	if (!send_manifest(fd, mine, n)) {
		close(fd);
		snprintf(err, errlen, "manifest exchange failed");
		return -1;
	}
	int tn = recv_manifest(fd, theirs, NS_MAX_MANIFEST, &peer_compat_enabled);
	if (tn < 0) {
		close(fd);
		snprintf(err, errlen, "manifest exchange failed");
		return -1;
	}

	int selected = select_compatibility(mine, n, theirs, tn, peer_compat_enabled);
	if (selected < 0)
		snprintf(err, errlen, "cannot activate core selection");

	/* Frozen peer-to-peer executable transfer. Compatibility negotiation above
	 * exchanges metadata only and selects local, packaged artifacts. */
#if 0
	uint32_t my_runtime = NS_runtimeGlibc();
	int staged = 0, attempted = 0;
	for (int i = 0; i < n; i++) {
		NS_CoreInfo* t = find_core(theirs, tn, mine[i].core);
		if (!t || !t->version[0]) continue;                 /* peer lacks it */
		if (mine[i].version[0] && !strcmp(mine[i].version, t->version)) continue;
		if (t->machine != mine[i].machine) continue;        /* unbridgeable */

		attempted++;
		bool we_can_load_theirs = t->glibc <= my_runtime;
		bool they_can_load_ours = mine[i].glibc <= peer_runtime;

		if (we_can_load_theirs) {
			uint8_t op = CORE_OP_GET;
			char name[32];
			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name), "%s", mine[i].core);
			if (!io_all(fd, &op, 1, true) || !io_all(fd, name, sizeof(name), true)) break;

			uint32_t len;
			if (!io_all(fd, &len, 4, false)) break;
			len = ntohl(len);
			if (!len || len > CORE_MAX_BYTES) continue;
			uint8_t* buf = malloc(len);
			if (!buf) break;
			if (!io_all(fd, buf, len, false)) { free(buf); break; }

			char dir[512], path[600], cmd[700];
			staged_dir(dir, sizeof(dir));
			snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
			system(cmd);
			snprintf(path, sizeof(path), "%s/%s_libretro.so", dir, mine[i].core);
			FILE* out = fopen(path, "wb");
			bool ok = false;
			if (out) {
				ok = fwrite(buf, 1, len, out) == len;
				fclose(out);
			}
			free(buf);

			/* Prove it loads before leaving it where the shim will find it. */
			if (ok) {
				void* probe = dlopen(path, RTLD_NOW | RTLD_LOCAL);
				if (probe) { dlclose(probe); staged++;
					ns_log("staged %s from host (%u bytes)\n", mine[i].core, len); }
				else { ns_log("host's %s will not load here (%s)\n", mine[i].core, dlerror());
					remove(path); ok = false; }
			}
			if (!ok) remove(path);

		} else if (they_can_load_ours) {
			/* We cannot use theirs but they can use ours - push instead. */
			FILE* f = fopen(mine[i].path, "rb");
			if (!f) continue;
			uint8_t* buf = malloc(mine[i].size);
			if (!buf) { fclose(f); continue; }
			size_t got = fread(buf, 1, mine[i].size, f);
			fclose(f);

			uint8_t op = CORE_OP_PUT;
			char name[32];
			memset(name, 0, sizeof(name));
			snprintf(name, sizeof(name), "%s", mine[i].core);
			uint32_t len = htonl((uint32_t)got);
			if (got == mine[i].size &&
			    io_all(fd, &op, 1, true) && io_all(fd, name, sizeof(name), true) &&
			    io_all(fd, &len, 4, true) && io_all(fd, buf, got, true)) {
				ns_log("pushed our %s to the host (%zu bytes)\n", mine[i].core, got);
				staged++;
			}
			free(buf);
		} else {
			ns_log("%s: neither build is loadable on the other device\n", mine[i].core);
		}
	}

	uint8_t done = CORE_OP_DONE;
	io_all(fd, &done, 1, true);
	(void)my_runtime;
	(void)attempted;
#endif
	close(fd);

	if (selected >= 0)
		ns_log("core compatibility negotiation: %d fallback(s) selected\n", selected);
	return selected;
}

//////////////////////////////////////////////////////////////////////////////
// assumption checks
//////////////////////////////////////////////////////////////////////////////

static void add(NS_Check* out, int* n, int max, NS_CheckResult r,
                const char* label, const char* detail) {
	if (*n >= max) return;
	out[*n].result = r;
	snprintf(out[*n].label, sizeof(out[*n].label), "%s", label);
	snprintf(out[*n].detail, sizeof(out[*n].detail), "%s", detail);
	(*n)++;
}

int NS_runChecks(NS_Check* out, int max, NS_CheckResult* worst) {
	int n = 0;
	NS_CheckResult w = NS_CHECK_OK;
	char path[512], line[512];

	/* 1. Emu paks must invoke minarch.elf unqualified, or our PATH shim is
	 *    never consulted and netplay silently does nothing. */
	int checked = 0, unqualified = 0;
	snprintf(path, sizeof(path), "%s/.system/%s/paks/Emus", ns_sd, ns_platform);
	{
		char cmd[600];
		snprintf(cmd, sizeof(cmd),
		         "grep -l 'minarch.elf' %s/*.pak/launch.sh 2>/dev/null", path);
		FILE* f = popen(cmd, "r");
		if (f) {
			while (fgets(line, sizeof(line), f)) {
				char* nl = strpbrk(line, "\r\n"); if (nl) *nl = '\0';
				checked++;
				char cmd2[700];
				/* An absolute path before minarch.elf means PATH is bypassed. */
				snprintf(cmd2, sizeof(cmd2),
				         "grep -qE '(^|[^/[:alnum:]_])minarch\\.elf' '%s' && "
				         "! grep -qE '/[A-Za-z0-9_./-]*/minarch\\.elf' '%s'", line, line);
				if (system(cmd2) == 0) unqualified++;
			}
			pclose(f);
		}
	}
	if (checked == 0)
		add(out, &n, max, NS_CHECK_FAIL, "Emulator paks", "none found where expected");
	else if (unqualified == 0)
		add(out, &n, max, NS_CHECK_FAIL, "minarch on PATH", "paks call it by absolute path");
	else if (unqualified < checked)
		add(out, &n, max, NS_CHECK_WARN, "minarch on PATH", "some paks bypass PATH");
	else
		add(out, &n, max, NS_CHECK_OK, "minarch on PATH", "resolved via PATH");

	/* 2. An SD-card Emus pak must win over the system one, or stubs are inert. */
	{
		char probe[512];
		snprintf(probe, sizeof(probe), "%s/Emus/%s", ns_sd, ns_platform);
		mkdir(probe, 0755);
		bool writable = access(probe, W_OK) == 0;
		add(out, &n, max,
		    writable ? NS_CHECK_OK : NS_CHECK_FAIL,
		    "SD pak override", writable ? "path writable" : "cannot create or write");
	}

	/* 3. core.name is basename truncated at the last underscore, and it feeds
	 *    config_dir AND states_dir. If that ever changes, a staged shim would
	 *    quietly relocate the user's save states - the only check here whose
	 *    failure costs data rather than features. */
	{
		const char* sample = "gpsp_libretro.so";
		char derived[64];
		snprintf(derived, sizeof(derived), "%s", sample);
		char* us = strrchr(derived, '_');
		if (us) *us = '\0';

		char states[512];
		snprintf(states, sizeof(states), "%s/.userdata/shared/GBA-%s", ns_sd, derived);
		bool ok = (strcmp(derived, "gpsp") == 0);
		char detail[96];
		snprintf(detail, sizeof(detail), ok && file_exists(states)
		         ? "save states resolve to GBA-gpsp"
		         : (ok ? "naming ok (no existing GBA states)" : "naming rule changed"));
		add(out, &n, max, ok ? NS_CHECK_OK : NS_CHECK_FAIL, "Save state paths", detail);
	}

	/* 4. Our own pieces are present for this platform. */
	{
		char shim[512];
		snprintf(shim, sizeof(shim), "%s/bin/%s/netplay_shim.so", ns_pak, ns_platform);
		bool ok = file_exists(shim);
		add(out, &n, max, ok ? NS_CHECK_OK : NS_CHECK_FAIL, "Shim for this device",
		    ok ? ns_platform : "missing - unsupported platform");
	}

	for (int i = 0; i < n; i++) if (out[i].result > w) w = out[i].result;
	if (worst) *worst = w;
	return n;
}

//////////////////////////////////////////////////////////////////////////////
// session
//////////////////////////////////////////////////////////////////////////////

bool NS_isArmed(void) {
	char p[512];
	snprintf(p, sizeof(p), "%s/state/session", ns_pak);
	return file_exists(p);
}

bool NS_arm(NS_Role role, const char* peer_ip, char* err, int errlen) {
	char path[512];

	snprintf(path, sizeof(path), "%s/state", ns_pak);
	mkdir(path, 0755);

	snprintf(path, sizeof(path), "%s/session.conf", ns_pak);
	FILE* f = fopen(path, "w");
	if (!f) {
		snprintf(err, errlen, "cannot write session.conf");
		return false;
	}

	/* No mode= line. The shim reads the core's library_name and decides, so a
	 * single armed session covers every system. Writing one here would pin all
	 * of them to whichever mode was picked before a game was even chosen.
	 * Hand-editing mode=netplay or mode=link into this file still overrides. */

	/* A new arm must never inherit a checkpoint from an older play session of
	 * the same ROM. The identifier is local to each device; it only namespaces
	 * that device's /tmp checkpoint and therefore need not be negotiated. */
	unsigned char nonce[16];
	memset(nonce, 0, sizeof(nonce));
	FILE* random = fopen("/dev/urandom", "rb");
	size_t random_bytes = 0;
	if (random) { random_bytes = fread(nonce, 1, sizeof(nonce), random); fclose(random); }
	if (random_bytes != sizeof(nonce)) {
		struct timeval now;
		gettimeofday(&now, NULL);
		memcpy(nonce, &now, sizeof(now) < sizeof(nonce) ? sizeof(now) : sizeof(nonce));
		nonce[sizeof(nonce) - 1] ^= (unsigned char)getpid();
	}
	for (size_t i = 0; i < sizeof(nonce); i++) fprintf(f, i ? "%02x" : "session_id=%02x", nonce[i]);
	fputc('\n', f);

	/* Sized for the transport actually in use. Both sides reach the same answer
	 * because a client only holds an ad hoc address when it joined this host's
	 * network, and the host only serves one when it is hosting ad hoc. */
	bool adhoc = hotspot_running || joined_hotspot;
	fprintf(f, "input_delay=%d\n", adhoc ? NS_INPUT_DELAY_ADHOC : NS_INPUT_DELAY_WIFI);

	/* Kept in the session format while peer executable sharing is frozen. */
	fprintf(f, "share_cores=%d\n", NS_settings()->share_cores ? 1 : 0);
	fprintf(f, "compatibility_cores=%d\n", NS_settings()->compatibility_cores ? 1 : 0);

	/* Record the ad hoc network so the launch stub can put us back on it. The
	 * join done here does not survive the app exiting - the platform brings its
	 * own WiFi back up before the game ever starts. */
	if (joined_hotspot && joined_ssid[0]) {
		fprintf(f, "adhoc_ssid=%s\nadhoc_psk=%s\n", joined_ssid, joined_psk);
	}

	if (role == NS_ROLE_HOST) {
		fprintf(f, "role=host\nport=55437\n");
		/* Host side of both link cores. Written unconditionally: an override for
		 * an option the core does not have is ignored, and the only cores that
		 * read these are the ones the shim puts in link mode anyway. */
		fprintf(f, "option.gambatte_gb_link_mode=Network Server\n");
		fprintf(f, "option.gambatte_gb_link_network_port=56400\n");
	} else {
		fprintf(f, "role=client\nport=55437\npeer=%s\n", peer_ip);
		fprintf(f, "option.gambatte_gb_link_mode=Network Client\n");
		fprintf(f, "option.gambatte_gb_link_network_port=56400\n");
		/* gambatte encodes the peer address as twelve single-digit options,
		 * three per octet, each zero-padded then atoi'd. */
		unsigned o[4] = {0, 0, 0, 0};
		sscanf(peer_ip, "%u.%u.%u.%u", &o[0], &o[1], &o[2], &o[3]);
		int idx = 1;
		for (int i = 0; i < 4; i++) {
			char digits[4];
			snprintf(digits, sizeof(digits), "%03u", o[i] % 1000);
			for (int d = 0; d < 3; d++)
				fprintf(f, "option.gambatte_gb_link_network_server_ip_%d=%c\n",
				        idx++, digits[d]);
		}
	}
	fclose(f);

	char cmd[1200];
	snprintf(cmd, sizeof(cmd), "cp '%s/session.conf' '%s/state/session'", ns_pak, ns_pak);
	if (system(cmd) != 0) {
		snprintf(err, errlen, "cannot activate session");
		return false;
	}

	/* Bind mounts first, launch stubs only if that fails.
	 *
	 * The mount route writes nothing to the user's Emus tree - it stages a copy
	 * of each system pak and mounts it over the original in place. The stub
	 * route creates a directory per system under Emus/<PLATFORM>/ and is kept
	 * only for devices where mounting is unavailable. bind-mount.sh clears any
	 * stubs left by a previous version before mounting, so the two can never be
	 * active at once - an SD stub would shadow the mount and wrap twice. */
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
	         "USERDATA_PATH='%s/.userdata/%s' "
	         "sh '%s/launcher/bind-mount.sh' up >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
	if (system(cmd) != 0) {
		ns_log("bind mounts unavailable - falling back to launch stubs\n");
		snprintf(cmd, sizeof(cmd),
		         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
		         "sh '%s/launcher/install-stubs.sh' install >/dev/null 2>&1",
		         ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
		if (system(cmd) != 0) {
			snprintf(err, errlen, "could not install launch stubs");
			return false;
		}
	}

	/* The launch stub does this again per game - power save comes back when the
	 * interface reassociates - but doing it here means discovery and the
	 * handshake are not fighting the radio either. */
	NS_wifiPowerSaveDisable();
	return true;
}

void NS_disarm(void) {
	char cmd[1200];
	/* Takes both routes down - mounts, the boot hook, and any legacy stubs -
	 * so "off" means off regardless of how this device was covered. */
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
	         "USERDATA_PATH='%s/.userdata/%s' "
	         "sh '%s/launcher/bind-mount.sh' down >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
	system(cmd);

	snprintf(cmd, sizeof(cmd), "rm -f '%s/state/session' '%s/state/force-shim'",
	         ns_pak, ns_pak);
	system(cmd);

	NS_hotspotStop();
	NS_wifiPowerSaveRestore();
}

/* Drop the session but leave the launch stubs installed.
 *
 * With no session file the shim loads as a pure passthrough, so games launch
 * exactly as they would without us - the difference from NS_disarm is only
 * that the stubs stay in place, so the next session is one button press away
 * instead of a full reinstall across every Emus pak. */
void NS_endSession(void) {
	char cmd[1200];
	snprintf(cmd, sizeof(cmd), "rm -f '%s/state/session'", ns_pak);
	system(cmd);

	/* A session is a network arrangement as much as a file. Ending one has to
	 * put the radio back: stop serving if we were, and rejoin the normal
	 * network if we had left it. Callers used to have to remember both. */
	NS_hotspotStop();
	NS_wifiPowerSaveRestore();
}

/* Covered by either route: mounts (preferred) or legacy stubs. */
bool NS_stubsInstalled(void) {
	char p[512];
	snprintf(p, sizeof(p), "%s/state/mounts.list", ns_pak);
	if (file_exists(p)) return true;
	snprintf(p, sizeof(p), "%s/state/stubs.list", ns_pak);
	return file_exists(p);
}

//////////////////////////////////////////////////////////////////////////////
// discovery
//////////////////////////////////////////////////////////////////////////////

static long ms_since(const struct timeval* t) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - t->tv_sec) * 1000L + (now.tv_usec - t->tv_usec) / 1000L;
}

void NS_announceStart(NS_Mode mode) {
	announce_mode = mode;
	if (announce_fd >= 0) return;
	announce_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (announce_fd < 0) return;
	int on = 1;
	setsockopt(announce_fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
	memset(&last_announce, 0, sizeof(last_announce));
}

void NS_announceTick(void) {
	if (announce_fd < 0) return;
	if (last_announce.tv_sec && ms_since(&last_announce) < ANNOUNCE_MS) return;

	NS_Announce a;
	memset(&a, 0, sizeof(a));
	a.magic = htonl(DISCOVERY_MAGIC);
	a.version = htonl(1);
	a.mode = htonl((uint32_t)announce_mode);
	a.hotspot = htonl(announce_ssid[0] ? 1u : 0u);
	snprintf(a.ssid, sizeof(a.ssid), "%s", announce_ssid);
	snprintf(a.psk, sizeof(a.psk), "%s", announce_psk);
	snprintf(a.platform, sizeof(a.platform), "%s", ns_platform);

	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(DISCOVERY_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	sendto(announce_fd, &a, sizeof(a), 0, (struct sockaddr*)&to, sizeof(to));

	gettimeofday(&last_announce, NULL);
}

void NS_announceStop(void) {
	if (announce_fd >= 0) { close(announce_fd); announce_fd = -1; }
}

struct scan_ctx { NS_Peer* out; int max; int n; };

static void scan_line(const char* line, void* v) {
	struct scan_ctx* c = v;
	if (c->n >= c->max || !line[0]) return;
	memset(&c->out[c->n], 0, sizeof(c->out[c->n]));
	snprintf(c->out[c->n].ssid, sizeof(c->out[c->n].ssid), "%s", line);
	snprintf(c->out[c->n].psk,  sizeof(c->out[c->n].psk),  "%s", NS_ADHOC_PSK);
	/* The host's address on its own network is fixed, so this is known before
	 * we join - which is what makes joining by SSID alone work. */
	snprintf(c->out[c->n].ip, sizeof(c->out[c->n].ip), "%s", NS_HOTSPOT_HOST_IP);
	c->out[c->n].hotspot = true;
	c->out[c->n].scanned = true;
	c->n++;
}

int NS_scanAdhoc(NS_Peer* out, int max) {
	if (!out || max <= 0) return 0;

	/* Prefer the supplicant's cached results over `iw scan`.
	 *
	 * wpa_cli scan_results returns whatever the supplicant already knows and
	 * comes back immediately; `iw dev wlan0 scan` asks the radio to go and look,
	 * which takes seconds when it works and hangs outright when it does not.
	 * The frontend polls the same way, so the cache is kept warm for us. */
	char ctrl[256];
	char cmd[600];
	if (platform_ctrl_dir(ctrl, sizeof(ctrl))) {
		/* Ask for a refresh but do not wait on it - this returns at once. */
		snprintf(cmd, sizeof(cmd), "wpa_cli -p %s -i wlan0 scan >/dev/null 2>&1", ctrl);
		system(cmd);
		snprintf(cmd, sizeof(cmd),
		         "wpa_cli -p %s -i wlan0 scan_results 2>/dev/null "
		         "| awk -F'\t' 'NR>1 && NF>=5 {print $5}' "
		         "| grep -i '^%s-' | sort -u", ctrl, NS_ADHOC_PREFIX);
	} else {
		snprintf(cmd, sizeof(cmd),
		         "iw dev wlan0 scan 2>/dev/null | sed -n 's/^[[:space:]]*SSID: //p' "
		         "| grep -i '^%s-' | sort -u", NS_ADHOC_PREFIX);
	}

	struct scan_ctx c = { out, max, 0 };
	popen_bounded(cmd, 6000, scan_line, &c);
	ns_log("scan found %d ad hoc network(s)\n", c.n);
	return c.n;
}

void NS_discoverStart(void) {
	if (discover_fd >= 0) return;
	discover_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (discover_fd < 0) return;

	int on = 1;
	setsockopt(discover_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(DISCOVERY_PORT);
	if (bind(discover_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(discover_fd); discover_fd = -1; return;
	}
	int flags = fcntl(discover_fd, F_GETFL, 0);
	if (flags >= 0) fcntl(discover_fd, F_SETFL, flags | O_NONBLOCK);
}

int NS_discoverTick(NS_Peer* peers, int max) {
	static int count = 0;
	if (discover_fd < 0) return count;
	if (peers == NULL) { count = 0; return 0; }

	NS_Announce a;
	struct sockaddr_in from;
	socklen_t flen = sizeof(from);

	while (recvfrom(discover_fd, &a, sizeof(a), 0, (struct sockaddr*)&from, &flen) == (ssize_t)sizeof(a)) {
		if (ntohl(a.magic) != DISCOVERY_MAGIC) continue;

		char ip[NS_IP_LEN];
		inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));

		bool known = false;
		for (int i = 0; i < count; i++)
			if (!strcmp(peers[i].ip, ip)) { known = true; break; }
		if (known || count >= max) continue;

		snprintf(peers[count].ip, NS_IP_LEN, "%s", ip);
		a.platform[sizeof(a.platform) - 1] = '\0';
		snprintf(peers[count].platform, sizeof(peers[count].platform), "%s", a.platform);
		peers[count].mode = (ntohl(a.mode) == NS_MODE_LINK) ? NS_MODE_LINK : NS_MODE_NETPLAY;
		peers[count].hotspot = ntohl(a.hotspot) != 0;
		a.ssid[sizeof(a.ssid) - 1] = '\0';
		a.psk[sizeof(a.psk) - 1]   = '\0';
		snprintf(peers[count].ssid, NS_SSID_LEN, "%s", a.ssid);
		snprintf(peers[count].psk,  NS_PSK_LEN,  "%s", a.psk);
		count++;
	}
	return count;
}

void NS_discoverStop(void) {
	/* Reset before closing. NS_discoverTick returns early when the fd is gone,
	 * so asking it to clear the count afterwards did nothing - and the stale
	 * peers[] then reappeared on the next visit to Join, offering a host that
	 * may have gone away or moved to an address that no longer routes. */
	NS_discoverTick(NULL, 0);
	if (discover_fd >= 0) { close(discover_fd); discover_fd = -1; }
}

//////////////////////////////////////////////////////////////////////////////
// ad hoc network
//
// Both devices negotiate while still on their existing network, then move
// together. The host hands wlan0 from wpa_supplicant to hostapd - these
// handhelds have one usable radio, so hosting means leaving the old network
// rather than running both at once.
//////////////////////////////////////////////////////////////////////////////

void NS_announceHotspot(const char* ssid, const char* psk) {
	snprintf(announce_ssid, sizeof(announce_ssid), "%s", ssid ? ssid : "");
	snprintf(announce_psk, sizeof(announce_psk), "%s", psk ? psk : "");
}

/* Fixed rather than generated. Generated credentials had to be advertised
 * before either side moved, which coupled ad hoc to discovery working first.
 * A known SSID and passphrase decouples them: the client can join even if it
 * never saw an announcement. The trade is that two pairs in one room collide. */
void NS_hotspotCredentials(char* ssid, int ssid_len, char* psk, int psk_len) {
	/* A short code in the SSID, so two pairs in one room do not collide and the
	 * joining device can be told which network is theirs without typing
	 * anything. Kept for the life of a session: regenerating it between the
	 * announcement and the AP coming up would leave the client looking for a
	 * network that no longer exists. */
	static char cached[NS_SSID_LEN];
	if (!cached[0]) {
		static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
		unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
		char code[5];
		for (int i = 0; i < 4; i++) {
			seed = seed * 1103515245u + 12345u;
			code[i] = alphabet[(seed >> 16) % (sizeof(alphabet) - 1)];
		}
		code[4] = '\0';
		snprintf(cached, sizeof(cached), "%s-%s", NS_ADHOC_PREFIX, code);
	}
	snprintf(ssid, ssid_len, "%s", cached);
	snprintf(psk,  psk_len,  "%s", NS_ADHOC_PSK);
}

/* The AP interface, which is not the one carrying the client connection.
 *
 * This firmware exposes a second interface dedicated to AP mode, and the radio
 * advertises `#{ managed } <= 2, #{ AP } <= 1` - so the host can serve the ad
 * hoc network while staying associated to the existing one. Pointing hostapd at
 * wlan0, as this used to, could never work while wlan0 was an associated
 * station: the interface stayed type managed and no client could associate. */
static bool ap_interface(char* out, int len) {
	/* Name and type together. Taking the first interface that simply is not
	 * wlan0 picked up hostapd's own leftover monitor vif - iw lists mon.wlan1
	 * before wlan1 - and hostapd then failed with "Could not read interface
	 * mon.wlan1 flags", after the previous working AP had already been killed. */
	FILE* p = popen("iw dev 2>/dev/null | sed -n 's/^\\s*Interface /IF /p; s/^\\s*type /TYPE /p'", "r");
	if (!p) return false;

	char line[96];
	char name[64] = "";
	bool found = false;
	while (fgets(line, sizeof(line), p) && !found) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';

		if (!strncmp(line, "IF ", 3)) {
			snprintf(name, sizeof(name), "%s", line + 3);
		} else if (!strncmp(line, "TYPE ", 5) && name[0]) {
			const char* type = line + 5;
			if (strcmp(name, "wlan0") != 0 &&
			    strncmp(name, "mon.", 4) != 0 &&
			    strcmp(type, "monitor") != 0) {
				snprintf(out, len, "%s", name);
				found = true;
			}
			name[0] = '\0';
		}
	}
	pclose(p);
	return found;
}

/* hostapd leaves these behind when it is killed rather than stopped, and they
 * then shadow the real interface. */
static void drop_monitor_interfaces(void) {
	system("for i in $(iw dev 2>/dev/null | sed -n 's/^\\s*Interface //p'); do "
	       "  case \"$i\" in mon.*) iw dev \"$i\" del 2>/dev/null;; esac; "
	       "done");
}

/* Pick the least congested 2.4GHz channel.
 *
 * Only usable when we are not associated: with a station up, the single-channel
 * constraint pins the AP to its channel and there is no choice to make. A scan
 * takes about two seconds on these radios and sees ~20 networks in a busy
 * environment, so this is cheap enough to do at every independent start.
 *
 * Weighted by overlap, not just exact matches - a 20MHz AP splatters over the
 * two channels either side of it. */
struct chan_ctx { int seen[15]; };

static void chan_line(const char* line, void* v) {
	struct chan_ctx* c = v;
	int freq = atoi(line);
	if (freq < 2412 || freq > 2472) return;
	int ch = (freq - 2407) / 5;
	if (ch >= 1 && ch <= 14) c->seen[ch]++;
}

static int quiet_channel(void) {
	/* Same reasoning as NS_scanAdhoc: cached results, and bounded either way.
	 * This runs from the Host screen, so a hang here freezes the UI. */
	char ctrl[256];
	char cmd[600];
	if (platform_ctrl_dir(ctrl, sizeof(ctrl))) {
		snprintf(cmd, sizeof(cmd), "wpa_cli -p %s -i wlan0 scan >/dev/null 2>&1", ctrl);
		system(cmd);
		snprintf(cmd, sizeof(cmd),
		         "wpa_cli -p %s -i wlan0 scan_results 2>/dev/null | awk -F'\t' 'NR>1 {print $2}'",
		         ctrl);
	} else {
		snprintf(cmd, sizeof(cmd), "iw dev wlan0 scan 2>/dev/null | sed -n 's/.*freq: *//p'");
	}

	struct chan_ctx c;
	memset(&c, 0, sizeof(c));
	popen_bounded(cmd, 6000, chan_line, &c);

	int best = 0, best_cost = -1;
	for (int ch = 1; ch <= 11; ch++) {
		int cost = 0;
		for (int o = ch - 2; o <= ch + 2; o++)
			if (o >= 1 && o <= 14) cost += c.seen[o];
		if (best_cost < 0 || cost < best_cost) { best_cost = cost; best = ch; }
	}
	if (best) ns_log("channel survey: picked %d (overlap score %d)\n", best, best_cost);
	return best;
}

/* Both interfaces share one radio and the driver permits a single channel, so
 * the AP has to sit on whatever channel the station is already using. A
 * hardcoded channel is rejected outright whenever it disagrees. */
static int station_channel(void) {
	FILE* p = popen("iw dev wlan0 link 2>/dev/null | sed -n 's/.*freq: *//p' | head -1", "r");
	if (!p) return 0;
	char line[32] = "";
	int freq = 0;
	if (fgets(line, sizeof(line), p)) freq = atoi(line);
	pclose(p);
	if (freq < 2412 || freq > 2484) return 0;
	return freq == 2484 ? 14 : (freq - 2407) / 5;
}

/* How the platform started its own supplicant, captured before we kill it.
 *
 * Guessing this is what broke the A30: the restore path assumed
 * /etc/wifi/wifi_init.sh, which exists on tg5040 and does not exist on my282,
 * so tearing the stack down there left the device with no network and nothing
 * to bring it back. Replaying the exact command line works on both. */
static char saved_supplicant[512];

static void wifi_client_stack_down(void) {
	saved_supplicant[0] = '\0';
	/* Match on argv[0] only. Matching anywhere in the command line also matches
	 * the shell running this very search - its own arguments contain the string
	 * - and whichever /proc entry came first would win. */
	FILE* p = popen(
		"for d in /proc/[0-9]*; do "
		"  a0=$(tr '\\0' '\\n' < $d/cmdline 2>/dev/null | head -1); "
		"  case \"$a0\" in */wpa_supplicant|wpa_supplicant) "
		"    tr '\\0' ' ' < $d/cmdline 2>/dev/null; echo; break;; "
		"  esac; "
		"done 2>/dev/null", "r");
	if (p) {
		if (fgets(saved_supplicant, sizeof(saved_supplicant), p)) {
			char* nl = strpbrk(saved_supplicant, "\r\n");
			if (nl) *nl = '\0';
		}
		pclose(p);
	}

	/* Persist it. Held only in memory, this is lost the moment the app exits or
	 * dies - which is exactly when a device is stranded on a network that is no
	 * longer there and has nothing left that knows how to get back. */
	if (saved_supplicant[0]) {
		char path[512];
		snprintf(path, sizeof(path), "%s/state/wifi_restore", ns_pak);
		FILE* w = fopen(path, "w");
		if (w) { fprintf(w, "%s\n", saved_supplicant); fclose(w); }
	}

	system("killall -q wpa_supplicant 2>/dev/null");
	system("killall -q udhcpc 2>/dev/null");
	system("ip addr flush dev wlan0 2>/dev/null");
	system("ip route flush dev wlan0 2>/dev/null");
}

/* The persisted copy, for when this process never captured one itself. */
static bool load_saved_supplicant(void) {
	if (saved_supplicant[0]) return true;

	char path[512];
	snprintf(path, sizeof(path), "%s/state/wifi_restore", ns_pak);
	FILE* f = fopen(path, "r");
	if (!f) return false;
	if (fgets(saved_supplicant, sizeof(saved_supplicant), f)) {
		char* nl = strpbrk(saved_supplicant, "\r\n");
		if (nl) *nl = '\0';
	}
	fclose(f);
	return saved_supplicant[0] != '\0';
}

/* How long to wait for the client stack to come back, in wall-clock seconds.
 *
 * One deadline, not one per step. Splitting it into an association budget and a
 * DHCP budget was wrong twice over: if association used its whole 8s the DHCP
 * phase then ran anyway against a link that did not exist, burning its budget to
 * reach a foregone conclusion - and a device that associated at 9s was declared
 * failed with 8 unused seconds sitting in the next phase.
 *
 * 25s because that is what this hardware needs. Measured on the my282: after a
 * supplicant restart the watchdog still saw "not associated" at its 20s check
 * and was associated by the next one. The original code appeared to work only
 * because a blocking `udhcpc -t 8` sat there for ~24s and gave association that
 * long to finish by accident; shortening it to 8s removed the accident and
 * exposed the real requirement. Spent animating, not frozen. */
#define RESTORE_TOTAL_S 25

/* Re-ask for a lease if one has not arrived this long after associating. The
 * first request can go out during the 4-way handshake and simply be dropped. */
#define RESTORE_DHCP_RETRY_S 8

static bool wifi_link_up(void) {
	return system("iw dev wlan0 link 2>/dev/null | grep -q 'Connected to'") == 0;
}

static bool wifi_has_ip(void) {
	return system("ip -4 addr show wlan0 2>/dev/null | grep -q 'inet '") == 0;
}

/* Put back whatever was running before, preferring the platform's own script
 * when it has one and falling back to the captured command line.
 *
 * Returns whether wlan0 ended up with an address. Callers used to get no signal
 * at all and reported success unconditionally, so the UI could claim it was
 * connected while the radio was still associating. */
static bool wifi_client_stack_up(void) {
	system("killall -q wpa_supplicant 2>/dev/null");
	system("ip link set wlan0 up 2>/dev/null");

	if (file_exists("/etc/wifi/wifi_init.sh")) {
		system("/etc/wifi/wifi_init.sh stop  >/dev/null 2>&1");
		system("/etc/wifi/wifi_init.sh start >/dev/null 2>&1");
		ns_log("restored wifi via wifi_init.sh\n");
		return wifi_has_ip();
	}

	if (!load_saved_supplicant()) {
		ns_log("WARNING: nothing recorded to restore wifi with\n");
		return false;
	}

	char cmd[600];
	/* The captured line already carries -B; it daemonises itself. */
	snprintf(cmd, sizeof(cmd), "%s >/dev/null 2>&1", saved_supplicant);
	system(cmd);

	/* One loop to the deadline. What we are waiting for is an address; whether
	 * the time goes on associating or on DHCP is not something to budget for
	 * separately.
	 *
	 * DHCP runs backgrounded and is polled, because the answer is observable
	 * independently of udhcpc's exit. Blocking on `udhcpc -t 8` cost up to ~24s
	 * of frozen UI: busybox pauses -T seconds between discover packets and
	 * defaults to 3, so the retry count multiplies straight into wall clock. */
	bool assoc = false, got_ip = false;
	int dhcp_at = -1;

	for (int i = 0; i < RESTORE_TOTAL_S && !got_ip; i++) {
		if (ns_progress)
			ns_progress(assoc ? "Getting an address" : "Reconnecting", i + 1, RESTORE_TOTAL_S);
		sleep(1);

		if (!assoc) {
			assoc = wifi_link_up();
			if (!assoc) continue;
			ns_log("re-associated after %ds\n", i + 1);
		}

		/* Ask on the first second we are associated, and again if nothing has
		 * arrived - never leaving a request unretried inside the deadline. */
		if (dhcp_at < 0 || i - dhcp_at >= RESTORE_DHCP_RETRY_S) {
			system("killall -q udhcpc 2>/dev/null");
			system("udhcpc -i wlan0 -n -q -t 3 -T 2 >/dev/null 2>&1 &");
			dhcp_at = i;
		}

		got_ip = wifi_has_ip();
	}

	if (got_ip) {
		ns_log("restored wifi via saved supplicant command\n");
		return true;
	}

	/* Out of time, but not necessarily failed - association may still land a few
	 * seconds from now. Leave udhcpc running rather than killing the one process
	 * that would finish the job, and report honestly so the caller does not draw
	 * a connected state that is not there yet. The restore record stays put, so
	 * the watchdog and the next app launch both still know to finish this. */
	ns_log("wifi not back after %ds (associated=%d) - leaving dhcp running\n",
	       RESTORE_TOTAL_S, assoc);
	return false;
}

/* Is a watchdog holding this session? Its pidfile is the shared token. */
static bool watchdog_running(void) {
	FILE* f = fopen("/tmp/netplay_watchdog.pid", "r");
	if (!f) return false;
	int pid = 0;
	bool ok = fscanf(f, "%d", &pid) == 1 && pid > 0;
	fclose(f);
	if (!ok) return false;

	char proc[64];
	snprintf(proc, sizeof(proc), "/proc/%d", pid);
	return file_exists(proc);
}

/* Recover a device left on a network that is gone.
 *
 * Two ways in: a join that failed partway, and a join that worked whose host
 * then tore its AP down. Both end with the radio associated to nothing, and
 * neither used to leave anything behind that knew how to undo it. */
bool NS_wifiRecoverIfStranded(void) {
	char path[512];
	snprintf(path, sizeof(path), "%s/state/wifi_restore", ns_pak);
	if (!file_exists(path)) return false;   /* never moved the client stack */

	bool connected = system("iw dev wlan0 link 2>/dev/null | grep -q 'Connected to'") == 0;
	bool has_ip    = system("ip -4 addr show wlan0 2>/dev/null | grep -q 'inet '") == 0;

	char cmd[256];
	snprintf(cmd, sizeof(cmd),
	         "iw dev wlan0 link 2>/dev/null | grep -q 'SSID: %s'", NS_ADHOC_SSID);
	bool on_adhoc = system(cmd) == 0;

	/* Healthy on some other network: nothing to do, and the record is stale.
	 *
	 * Unless a watchdog is mid-count. It is the record's other reader, and
	 * removing it underneath makes it stand down believing the session ended -
	 * which is exactly what happened in testing, aborting a recovery at 2 of 3
	 * strikes. Whoever finishes gets to clean up. */
	if (connected && has_ip && !on_adhoc) {
		if (!watchdog_running()) remove(path);
		return false;
	}

	/* Still usefully on the ad hoc network with a session armed - leave it. */
	if (on_adhoc && has_ip && NS_isArmed()) return false;

	ns_log("stranded (connected=%d ip=%d adhoc=%d) - restoring wifi\n",
	       connected, has_ip, on_adhoc);
	joined_hotspot = false;
	/* Only drop the breadcrumb once we are actually back. Removing it on a
	 * restore that timed out deletes the one record the watchdog and the next
	 * app launch use to know a restore is still owed - stranding the device for
	 * good, which is the exact failure this whole mechanism exists to stop. */
	bool back = wifi_client_stack_up();
	if (back) remove(path);
	return true;
}

/* Are we currently sitting on one of our own ad hoc networks?
 *
 * Observable, unlike joined_hotspot, which is process-local. The normal flow is
 * join -> leave the app to play -> come back later to end the session, and by
 * then joined_hotspot is false in a fresh process - so gating the client-side
 * restore on it meant ending a session left the device on a network that was
 * about to be torn down. */
static bool on_own_adhoc(void) {
	char cmd[256];
	snprintf(cmd, sizeof(cmd),
	         "iw dev wlan0 link 2>/dev/null | grep -q 'SSID: %s-'", NS_ADHOC_PREFIX);
	return system(cmd) == 0;
}

/* An in-process flag is not enough: the app can be reopened after a crash or a
 * quit that skipped teardown, and a hotspot left running still has to be
 * cleanable. */
static bool hotspot_processes_alive(void) {
	return system("pidof hostapd >/dev/null 2>&1") == 0
	    || system("pidof udhcpd  >/dev/null 2>&1") == 0;
}

bool NS_hotspotStart(const char* ssid, const char* psk, char* err, int errlen) {
	/* Already serving this network? Leave it alone.
	 *
	 * Tearing it down and rebuilding it is what stranded a client that had
	 * already joined: the old AP was killed, the restart then failed, and the
	 * peer was left associated to a network that no longer existed with no way
	 * back. Restarting is only worth the risk if something is actually wrong. */
	char cur[128];
	if (hotspot_running && ns_ap_if[0]
	    && system("pidof hostapd >/dev/null 2>&1") == 0) {
		snprintf(cur, sizeof(cur),
		         "iw dev %s info 2>/dev/null | grep -q 'ssid %s'", ns_ap_if, ssid);
		if (system(cur) == 0) {
			ns_log("AP already up on %s as %s - keeping it\n", ns_ap_if, ssid);
			return true;
		}
	}

	drop_monitor_interfaces();

	char apif[64];
	if (!ap_interface(apif, sizeof(apif))) {
		snprintf(err, errlen, "no AP interface on this device - it cannot host ad hoc");
		return false;
	}
	if (system("command -v udhcpd >/dev/null 2>&1") != 0) {
		/* my282 ships no udhcpd. Without it a client associates and then sits
		 * with no address, which used to read as a hang. Say so instead. */
		snprintf(err, errlen, "no udhcpd on this device - it cannot host ad hoc");
		return false;
	}

	/* Follow the station if there is one - the radio permits a single channel.
	 * Free of that constraint, survey and take the quietest. */
	int ch = station_channel();
	if (!ch) ch = quiet_channel();
	if (!ch) ch = 1;

	FILE* f = fopen("/tmp/netplay_hostapd.conf", "w");
	if (!f) { snprintf(err, errlen, "cannot write hostapd config"); return false; }
	fprintf(f,
		"interface=%s\ndriver=nl80211\nssid=%s\nhw_mode=g\nchannel=%d\n"
		"auth_algs=1\nwpa=2\nwpa_passphrase=%s\nwpa_key_mgmt=WPA-PSK\n"
		"rsn_pairwise=CCMP\nctrl_interface=/var/run/hostapd\n",
		apif, ssid, ch, psk);
	fclose(f);

	f = fopen("/tmp/netplay_udhcpd.conf", "w");
	if (!f) { snprintf(err, errlen, "cannot write dhcp config"); return false; }
	fprintf(f,
		"start 10.0.0.20\nend 10.0.0.40\ninterface %s\n"
		"lease_file /tmp/netplay_udhcpd.leases\npidfile /tmp/netplay_udhcpd.pid\n"
		"option subnet 255.255.255.0\noption lease 3600\n", apif);
	fclose(f);
	system("touch /tmp/netplay_udhcpd.leases");

	/* Same graceful shutdown as the teardown path - see NS_hotspotStop. A
	 * SIGKILL here with a client still associated can wedge the radio. */
	system("killall -q hostapd 2>/dev/null");
	for (int i = 0; i < 10; i++) {
		if (system("pidof hostapd >/dev/null 2>&1") != 0) break;
		usleep(500 * 1000);
	}
	system("killall -q -9 hostapd 2>/dev/null");
	system("killall -q udhcpd 2>/dev/null");
	system("killall -q -9 udhcpd 2>/dev/null");
	sleep(1);

	/* Reset the interface before handing it to hostapd.
	 *
	 * Killing hostapd does not undo what it did: the interface is left type AP
	 * with the SSID still set and the channel still claimed, and a later hostapd
	 * then fails with "Could not set channel for kernel driver". This only shows
	 * up on the second session - the first works because the interface is clean
	 * - which is exactly how it looked like ad hoc had regressed.
	 *
	 * Note what is *not* here: the client stack is left alone. The host stays on
	 * the existing network, so discovery keeps working and only one device has
	 * to move. */
	char cmd[512];
	drop_monitor_interfaces();
	snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", apif);      system(cmd);
	snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", apif);     system(cmd);
	snprintf(cmd, sizeof(cmd), "iw dev %s set type managed 2>/dev/null", apif); system(cmd);
	sleep(1);

	/* Not -B. Its exit status only says it forked, which it does even when the
	 * channel is rejected - the old "hostapd would not start" branch could never
	 * fire. Run it in the background and wait for it to say AP-ENABLED. */
	ns_log("starting AP on %s channel %d (station is on %d)\n", apif, ch, station_channel());
	system("hostapd /tmp/netplay_hostapd.conf > /tmp/netplay_hostapd.log 2>&1 &");

	bool up = false;
	for (int i = 0; i < 12 && !up; i++) {
		sleep(1);
		if (system("pidof hostapd >/dev/null 2>&1") != 0) break;
		up = system("grep -q AP-ENABLED /tmp/netplay_hostapd.log 2>/dev/null") == 0;
	}
	if (!up) {
		snprintf(err, errlen, "AP would not start on %s ch%d - see netplay_hostapd.log", apif, ch);
		ns_log("%s\n", err);
		system("sed 's/^/[netplay-app]   hostapd: /' /tmp/netplay_hostapd.log 1>&2 2>/dev/null");
		system("killall -q hostapd 2>/dev/null");
		return false;
	}

	/* hostapd owns the interface now, so the address goes on after it is up. */
	snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", apif); system(cmd);
	snprintf(cmd, sizeof(cmd), "ip addr add " NS_HOTSPOT_HOST_IP "/24 dev %s 2>/dev/null", apif);
	system(cmd);

	system("udhcpd /tmp/netplay_udhcpd.conf >/dev/null 2>&1");
	if (system("pidof udhcpd >/dev/null 2>&1") != 0) {
		snprintf(err, errlen, "AP up but udhcpd would not run - clients get no address");
		ns_log("%s\n", err);
		system("killall -q hostapd 2>/dev/null");
		return false;
	}
	ns_log("AP up on %s as %s, serving %s\n", apif, ssid, NS_HOTSPOT_HOST_IP);

	snprintf(ns_ap_if, sizeof(ns_ap_if), "%s", apif);
	hotspot_running = true;
	return true;
}

/* Where the platform's own supplicant listens.
 *
 * The frontend's WiFi status - the signal icon, the SSID, everything
 * PLAT_wifiConnection reports - is read with `wpa_cli -p <dir>`. Replacing the
 * supplicant with one on a different control socket leaves that layer blind: it
 * reports no connection and the icon disappears, while the radio is in fact
 * associated and passing traffic. Reusing the platform's directory keeps the
 * frontend able to see us, in game as well as in this app.
 *
 * Taken from -O on the running command line if present (tg5040), otherwise from
 * ctrl_interface= in the config it was given (my282). */
static bool platform_ctrl_dir(char* out, int len) {
	out[0] = '\0';

	FILE* p = popen(
		"for d in /proc/[0-9]*; do "
		"  a0=$(tr '\\0' '\\n' < $d/cmdline 2>/dev/null | head -1); "
		"  case \"$a0\" in */wpa_supplicant|wpa_supplicant) "
		"    tr '\\0' '\\n' < $d/cmdline 2>/dev/null; break;; "
		"  esac; "
		"done 2>/dev/null", "r");
	if (!p) return false;

	char line[512];
	char conf[512] = "";
	int want = 0;   /* 'O' or 'c' when the value is the next argument */
	while (fgets(line, sizeof(line), p)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';

		/* Both spellings occur: tg5040 runs `-O/etc/wifi/sockets` joined, my282
		 * runs `-c /path` as two arguments. Handling only the joined form found
		 * nothing on my282 and silently fell back to a directory the frontend
		 * does not watch - which is the whole bug this function exists to fix. */
		if (want) {
			if (want == 'O') snprintf(out, len, "%s", line);
			else             snprintf(conf, sizeof(conf), "%s", line);
			want = 0;
			continue;
		}
		if (!strcmp(line, "-O")) { want = 'O'; continue; }
		if (!strcmp(line, "-c")) { want = 'c'; continue; }
		if (!strncmp(line, "-O", 2) && line[2]) snprintf(out, len, "%s", line + 2);
		if (!strncmp(line, "-c", 2) && line[2]) snprintf(conf, sizeof(conf), "%s", line + 2);
	}
	pclose(p);

	if (!out[0] && conf[0]) {
		char cmd[600];
		snprintf(cmd, sizeof(cmd),
		         "sed -n 's/^ctrl_interface=\\(DIR=\\)\\?//p' '%s' 2>/dev/null "
		         "| sed 's/ .*//' | head -1", conf);
		FILE* c = popen(cmd, "r");
		if (c) {
			if (fgets(out, len, c)) {
				char* nl = strpbrk(out, "\r\n");
				if (nl) *nl = '\0';
			}
			pclose(c);
		}
	}
	return out[0] != '\0';
}

bool NS_hotspotJoin(const char* ssid, const char* psk, char* err, int errlen) {
	char ctrl[256];
	if (!platform_ctrl_dir(ctrl, sizeof(ctrl))) {
		snprintf(ctrl, sizeof(ctrl), "/var/run/wpa_supplicant");
		ns_log("no platform ctrl_interface found - frontend wifi status will be blank\n");
	} else {
		ns_log("joining %s, reusing ctrl_interface %s\n", ssid, ctrl);
	}

	FILE* f = fopen("/tmp/netplay_wpa.conf", "w");
	if (!f) { snprintf(err, errlen, "cannot write supplicant config"); return false; }
	fprintf(f,
		"ctrl_interface=%s\nupdate_config=1\n"
		"network={\n\tssid=\"%s\"\n\tpsk=\"%s\"\n\tkey_mgmt=WPA-PSK\n"
		"\tpriority=99\n}\n", ctrl, ssid, psk);
	fclose(f);

	/* Capture how the platform runs its supplicant before killing it - that is
	 * what puts the existing network back afterwards. */
	wifi_client_stack_down();
	joined_hotspot = true;   /* set before the first failure path can return */

	char cmd[256];
	for (int attempt = 1; attempt <= NS_JOIN_ATTEMPTS; attempt++) {
		system("ip link set wlan0 up 2>/dev/null");
		system("wpa_supplicant -B -D nl80211 -i wlan0 -c /tmp/netplay_wpa.conf >/dev/null 2>&1");

		/* Bounded per attempt: five unbounded tries is minutes of black screen. */
		bool assoc = false;
		snprintf(cmd, sizeof(cmd), "iw dev wlan0 link 2>/dev/null | grep -q '%s'", ssid);
		for (int i = 0; i < NS_JOIN_ASSOC_S && !assoc; i++) {
			if (ns_progress) ns_progress("Connecting", attempt, NS_JOIN_ATTEMPTS);
			sleep(1);
			assoc = system(cmd) == 0;
		}

		if (assoc) {
			/* iw reports the SSID as soon as it associates, which is before the
			 * 4-way handshake finishes - a DHCP request sent in that gap is
			 * simply dropped. */
			if (ns_progress) ns_progress("Getting an address", attempt, NS_JOIN_ATTEMPTS);
			sleep(1);
			for (int j = 0; j < 2; j++) {
				if (system("udhcpc -i wlan0 -n -q -t 6 >/dev/null 2>&1") == 0
				    && system("ip -4 addr show wlan0 2>/dev/null | grep -q 'inet '") == 0) {
					snprintf(joined_ssid, sizeof(joined_ssid), "%s", ssid);
					snprintf(joined_psk,  sizeof(joined_psk),  "%s", psk);

					/* Guard the window where stranding is possible: from here
					 * until the session ends. Detached, so it outlives this app
					 * - which is the point, since the stranding shows up while
					 * you are somewhere else entirely. */
					char wd[700];
					snprintf(wd, sizeof(wd),
					         "if command -v start-stop-daemon >/dev/null 2>&1; then "
					         "  start-stop-daemon -S -b -m -p /tmp/netplay_watchdog_ssd.pid "
					         "    -x /bin/sh -- "
					         "    '%s/launcher/wifi-watchdog.sh' '%s'; "
					         "else ( sh '%s/launcher/wifi-watchdog.sh' '%s' </dev/null & ); fi",
					         ns_pak, ns_pak, ns_pak, ns_pak);
					system(wd);
					ns_log("started wifi watchdog\n");
					return true;
				}
			}
			snprintf(err, errlen, "joined %s but no address (attempt %d)", ssid, attempt);
		} else {
			snprintf(err, errlen, "could not find %s (attempt %d)", ssid, attempt);
		}

		system("killall -q wpa_supplicant 2>/dev/null");
		if (attempt < NS_JOIN_ATTEMPTS) {
			for (int i = 0; i < NS_JOIN_GAP_S; i++) {
				if (ns_progress) ns_progress("Retrying", attempt + 1, NS_JOIN_ATTEMPTS);
				sleep(1);
			}
		}
	}

	/* Never leave the radio stranded on a network that did not work. */
	NS_hotspotStop();
	return false;
}

void NS_hotspotStop(void) {
	announce_ssid[0] = '\0';
	announce_psk[0]  = '\0';

	/* Host and client undo different things, and doing the wrong one is how
	 * Turn off used to take a device off WiFi with no way back. */
	if (hotspot_running || hotspot_processes_alive()) {
		/* Let hostapd shut down properly. This is not politeness.
		 *
		 * SIGTERM makes hostapd deauthenticate its stations and hand the
		 * interface back in an orderly way. Sending SIGKILL straight after it -
		 * which is what this used to do, with no wait in between - kills it
		 * before any of that happens, leaving the driver holding an AP with a
		 * station still associated. On a single-radio device that also carries
		 * the station interface, that wedged the whole radio: the Brick froze
		 * hard mid-session and took several reboots and manual WiFi toggling to
		 * recover. Ending a session must never be able to do that.
		 *
		 * udhcpd genuinely does ignore SIGTERM on these images, so it still
		 * needs escalating - but separately, and only it. */
		system("killall -q hostapd 2>/dev/null");
		for (int i = 0; i < 10; i++) {          /* up to ~5s for a clean exit */
			if (system("pidof hostapd >/dev/null 2>&1") != 0) break;
			usleep(500 * 1000);
		}
		if (system("pidof hostapd >/dev/null 2>&1") == 0) {
			ns_log("hostapd did not exit on SIGTERM - escalating\n");
			system("killall -q -9 hostapd 2>/dev/null");
			sleep(1);
		}

		system("killall -q udhcpd 2>/dev/null");
		system("killall -q -9 udhcpd 2>/dev/null");
		/* Put the interface back to a state the next hostapd can configure.
		 * Leaving it type AP with a channel claimed is what made the second
		 * session fail where the first succeeded. */
		drop_monitor_interfaces();
		if (ns_ap_if[0]) {
			char cmd[256];
			snprintf(cmd, sizeof(cmd), "ip addr flush dev %s 2>/dev/null", ns_ap_if);   system(cmd);
			snprintf(cmd, sizeof(cmd), "ip link set %s down 2>/dev/null", ns_ap_if);    system(cmd);
			snprintf(cmd, sizeof(cmd), "iw dev %s set type managed 2>/dev/null", ns_ap_if); system(cmd);
		}
		ns_log("AP stopped\n");
		hotspot_running = false;
		ns_ap_if[0] = '\0';
		/* The host never took the client stack down, so there is nothing to
		 * restore here. */
	}

	/* Restore the client stack if we moved it - judged by what the radio is
	 * actually associated to, not by a flag this process may not have set. */
	if (joined_hotspot || on_own_adhoc()) {
		ns_log("leaving ad hoc network, restoring wifi\n");
		wifi_client_stack_up();
		joined_hotspot = false;
	}
}

bool NS_hotspotActive(void) { return hotspot_running || joined_hotspot; }

/* Can this device serve a network at all?
 *
 * Asked before offering the choice, so a device that cannot do it never shows
 * an option that would fail. Platforms differ here and the answer is not
 * knowable in advance - probe rather than assume. */
bool NS_canHostAdhoc(void) {
	/* Cached: this is called from render, several times per frame, and the
	 * uncached version spawns a shell running `iw dev` each time. The set of
	 * wireless interfaces does not change under us during a session - hostapd
	 * adds and removes a monitor vif, which ap_interface already ignores. */
	static int cached = -1;
	if (cached < 0) {
		char apif[64];
		cached = ap_interface(apif, sizeof(apif)) ? 1 : 0;
		ns_log("ad hoc hosting: %s\n", cached ? "supported" : "no AP-capable interface");
	}
	return cached != 0;
}

void NS_setProgressCallback(NS_ProgressFn fn) { ns_progress = fn; }

void NS_wifiRestore(void) {
	char path[512];
	snprintf(path, sizeof(path), "%s/state/wifi_restore", ns_pak);

	/* Stop serving too - on one radio, an AP left running is part of why the
	 * client stack cannot settle. */
	if (hotspot_running || hotspot_processes_alive()) NS_hotspotStop();

	joined_hotspot = false;
	if (wifi_client_stack_up()) remove(path);   /* see NS_wifiRecoverIfStranded */
}

/* Who is actually on our network.
 *
 * Associated does not mean reachable - a station can hold the radio link and
 * still have no address - so this reports the address where one is known and
 * says so plainly when it is not, rather than implying a working peer. */
int NS_hotspotClients(NS_Client* out, int max) {
	if (!hotspot_running || !ns_ap_if[0] || !out || max <= 0) return 0;

	char cmd[256];
	snprintf(cmd, sizeof(cmd),
	         "iw dev %s station dump 2>/dev/null | sed -n 's/^Station \\([0-9a-f:]*\\).*/\\1/p'",
	         ns_ap_if);
	FILE* p = popen(cmd, "r");
	if (!p) return 0;

	int n = 0;
	char mac[32];
	while (n < max && fgets(mac, sizeof(mac), p)) {
		char* nl = strpbrk(mac, "\r\n");
		if (nl) *nl = '\0';
		if (!mac[0]) continue;

		snprintf(out[n].mac, sizeof(out[n].mac), "%s", mac);
		out[n].ip[0] = '\0';

		/* The lease lands in the kernel's neighbour table once the client
		 * actually talks to us, which is the thing worth showing. */
		char look[256];
		snprintf(look, sizeof(look),
		         "awk 'tolower($4)==\"%s\" && $6==\"%s\" {print $1; exit}' /proc/net/arp 2>/dev/null",
		         mac, ns_ap_if);
		FILE* a = popen(look, "r");
		if (a) {
			if (fgets(out[n].ip, sizeof(out[n].ip), a)) {
				char* n2 = strpbrk(out[n].ip, "\r\n");
				if (n2) *n2 = '\0';
			}
			pclose(a);
		}
		n++;
	}
	pclose(p);
	return n;
}
