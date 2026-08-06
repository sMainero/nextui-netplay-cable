/*
 * Session setup: discovery, session files, and the pre-arm safety checks.
 *
 * Deliberately separate from the UI so it can be exercised without a screen.
 */

#ifndef NETSETUP_H
#define NETSETUP_H

#include <stdbool.h>

#define NS_MAX_PEERS 8
#define NS_IP_LEN    24

typedef struct {
	char ip[NS_IP_LEN];
	char platform[16];
} NS_Peer;

/* --- environment ------------------------------------------------------- */

void NS_init(void);                  /* resolve pak path, platform, sd root */
const char* NS_platform(void);
const char* NS_pakPath(void);
bool NS_localIP(char* out, int len);

/* --- assumption checks -------------------------------------------------
 *
 * The shim leans on a few NextUI behaviours that are internal details rather
 * than promises. Three of them fail silently if they ever change - the launcher
 * simply never runs and games launch as stock - and one of those silently moves
 * save states. Checking at arm time turns all of that into a message.
 */

typedef enum {
	NS_CHECK_OK = 0,
	NS_CHECK_WARN,
	NS_CHECK_FAIL,
} NS_CheckResult;

typedef struct {
	NS_CheckResult result;
	char label[48];
	char detail[96];
} NS_Check;

/* Returns the number of checks run; worst result via *worst. */
int NS_runChecks(NS_Check* out, int max, NS_CheckResult* worst);

/* --- session ------------------------------------------------------------ */

typedef enum { NS_ROLE_HOST, NS_ROLE_CLIENT } NS_Role;

/* Write session.conf + state/session, and install launch stubs. */
bool NS_arm(NS_Role role, const char* peer_ip, char* err, int errlen);

/* Remove session, force flag and stubs. */
void NS_disarm(void);

bool NS_isArmed(void);

/* --- discovery ---------------------------------------------------------- */

/* Host: announce presence so clients can find us. Safe to call repeatedly. */
void NS_announceStart(void);
void NS_announceTick(void);
void NS_announceStop(void);

/* Client: listen for announcements. Call repeatedly; accumulates into peers.
 * Returns the number known so far. */
void NS_discoverStart(void);
int  NS_discoverTick(NS_Peer* peers, int max);
void NS_discoverStop(void);

#endif
