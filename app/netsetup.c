#include "netsetup.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DISCOVERY_PORT  55438
#define DISCOVERY_MAGIC 0x4E504C4BU /* 'NPLK' - same family as the link */
#define ANNOUNCE_MS     500

typedef struct __attribute__((packed)) {
	uint32_t magic;
	uint32_t version;
	char     platform[16];
} NS_Announce;

static char ns_platform[16] = "tg5040";
static char ns_pak[256];
static char ns_sd[128] = "/mnt/SDCARD";

static int announce_fd = -1;
static int discover_fd = -1;
static struct timeval last_announce;

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
		add(out, &n, max,
		    file_exists(probe) ? NS_CHECK_OK : NS_CHECK_FAIL,
		    "SD pak override", file_exists(probe) ? "path writable" : "cannot create");
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

	if (role == NS_ROLE_HOST) {
		fprintf(f, "role=host\nport=55437\n");
		/* Host side of both link cores. */
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

	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
	         "sh '%s/launcher/install-stubs.sh' install >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
	if (system(cmd) != 0) {
		snprintf(err, errlen, "could not install launch stubs");
		return false;
	}
	return true;
}

void NS_disarm(void) {
	char cmd[1200];
	snprintf(cmd, sizeof(cmd),
	         "SDCARD_PATH='%s' PLATFORM='%s' SYSTEM_PATH='%s/.system/%s' "
	         "sh '%s/launcher/install-stubs.sh' uninstall >/dev/null 2>&1",
	         ns_sd, ns_platform, ns_sd, ns_platform, ns_pak);
	system(cmd);

	snprintf(cmd, sizeof(cmd), "rm -f '%s/state/session' '%s/state/force-shim'; rm -rf '%s/cores/staged'",
	         ns_pak, ns_pak, ns_pak);
	system(cmd);
}

//////////////////////////////////////////////////////////////////////////////
// discovery
//////////////////////////////////////////////////////////////////////////////

static long ms_since(const struct timeval* t) {
	struct timeval now;
	gettimeofday(&now, NULL);
	return (now.tv_sec - t->tv_sec) * 1000L + (now.tv_usec - t->tv_usec) / 1000L;
}

void NS_announceStart(void) {
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
		count++;
	}
	return count;
}

void NS_discoverStop(void) {
	if (discover_fd >= 0) { close(discover_fd); discover_fd = -1; }
	NS_discoverTick(NULL, 0);
}
