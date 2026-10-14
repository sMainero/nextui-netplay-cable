/* A host build of the cable daemon (/testing/test-cable.sh) needs two symbols
 * from netsetup.c - the state directory and whether a session is armed - and
 * nothing else. Linking the real netsetup.c is not an option off-device: it is
 * the setup app, and it carries SDL and the NextUI API. These stand in, with the
 * same meaning, so the daemon's lifecycle (role parsing, the pid lock, the status
 * file, the restore record, teardown) can be exercised on any host. The device
 * build still links the real thing - app/makefile picks one or the other.
 *
 * Not part of the pak: nothing here runs on a device.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "netsetup.h"

static char stub_state[4096];

void NS_init(void) {
	/* No pak path or platform to resolve here: NS_statePath() below reads what
	 * it needs from SDCARD_PATH itself. */
}

const char* NS_statePath(void) {
	if (!stub_state[0]) {
		const char* sd = getenv("SDCARD_PATH");
		snprintf(stub_state, sizeof(stub_state), "%s/.userdata/shared/Netplay",
		         (sd && *sd) ? sd : "/tmp");
	}
	return stub_state;
}

bool NS_isArmed(void) {
	char path[4096];
	snprintf(path, sizeof(path), "%s/session", NS_statePath());
	struct stat st;
	return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
