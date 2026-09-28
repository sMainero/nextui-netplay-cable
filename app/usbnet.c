/*
 * The USB cable link daemon.
 *
 * One binary, two roles, and a lifetime that belongs to the session rather than
 * to a launch. `--role gadget` is the device end of the cable: it builds a
 * configfs FunctionFS gadget with the shape the firmware's own adbd already
 * runs, takes the controller from whoever holds it, and bridges the two bulk
 * endpoints to a TUN interface. `--role host` is the enumerating end: it
 * switches the port to host, finds the peer's gadget on the usbfs bus, claims
 * its vendor interface and drives its two bulk endpoints with asynchronous
 * URBs. Both roles end up with a TUN interface carrying the same
 * length-prefixed records, which is why nothing above them can tell which end
 * of the cable it is on.
 *
 * Why a daemon rather than part of the setup app: the link has to be up before
 * the session file is written, and has to outlive the app that started it.
 * app/broker.c is the precedent, and its lifecycle is copied deliberately -
 * pidfile as the single-instance lock, status published by rename, a signal
 * flag instead of a handler that does work, artifacts removed on every exit
 * path. The starter ritual (fork, setsid, log on both descriptors, fd sweep,
 * execl) is NS_brokerStart's and is not repeated here.
 *
 * Why the repair record exists: this process takes the UDC away from the
 * firmware's gadget, which is the thing that makes `adb` work. What it owes the
 * device is written to <state>/usb_restore *before* it is taken, so a hard kill
 * leaves a record something can repair from, rather than a device with no adb
 * and nothing that knows how to put it back.
 * launcher/session-cleanup.sh runs that repair at boot; NS_cableRecoverIfStranded
 * is the app's pass. Neither is in this file, and both are why the record is
 * keyed rather than positional.
 *
 * No kernel headers. The three constants this file needs from <linux/if_tun.h>,
 * the event vocabulary from <linux/usb/functionfs.h>, and the ioctls and two
 * structures the host role needs from <linux/usbdevice_fs.h> are transcribed
 * below with their source noted, exactly as app/cableproto.c transcribes the
 * descriptor blob, so the daemon builds wherever the app builds. The usbfs
 * structures are transcribed with their layout asserted underneath them,
 * because the kernel reads them by offset: a struct that is one field out is
 * not a compile error, it is a transfer that lands on the wrong endpoint.
 */
#include "usbnet.h"
#include "netsetup.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

//////////////////////////////////////////////////////////////////////////////
// transcribed kernel constants
//////////////////////////////////////////////////////////////////////////////

/* <linux/if_tun.h> and <linux/if.h> in 4.9: TUNSETIFF is _IOW('T', 202, int)
 * and the two flags say "layer 3" and "no packet information header". The
 * device build's include path is the platform's, and a uapi header that is not
 * there would be a build failure for a feature that does not need it. */
#define CP_TUNSETIFF _IOW('T', 202, int)
#define CP_IFF_TUN   0x0001
#define CP_IFF_NO_PI 0x1000

/* <linux/usb/functionfs.h> in 4.9: the events the kernel leaves on ep0 and the
 * 12-byte structure each arrives in - an 8-byte control request, a type byte,
 * three of padding. The order is the enum's, so an off-by-one would be read as
 * a different event's type rather than as an error. */
enum {
	CP_FFS_EV_BIND = 0,
	CP_FFS_EV_UNBIND,
	CP_FFS_EV_ENABLE,
	CP_FFS_EV_DISABLE,
	CP_FFS_EV_SETUP,
	CP_FFS_EV_SUSPEND,
	CP_FFS_EV_RESUME,
};

typedef struct {
	uint8_t  bRequestType;
	uint8_t  bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
} __attribute__((packed)) CP_CtrlRequest;

typedef struct {
	CP_CtrlRequest setup;
	uint8_t        type;
	uint8_t        pad[3];
} __attribute__((packed)) CP_FFS_Event;

/* <linux/usbdevice_fs.h> in 4.9: the host role's only way to reach the peer.
 *
 * The ioctl numbers are the _IOC-encoded values for a 64-bit target, which is
 * the only ABI this daemon is built for. Three of them encode a size - two carry
 * sizeof(void *) and one carries sizeof(struct usbdevfs_urb) - so a 32-bit
 * build would need the ...32 variants and a different urb layout. Writing them
 * as numbers rather than as _IOR/_IOW on a hand-made struct is deliberate: the
 * numbers are what the kernel compares against, and a macro that quietly
 * encoded a different size would look correct and fail at run time.
 *
 *   USBDEVFS_CONTROL        _IOWR('U',  0, struct usbdevfs_ctrltransfer)  24 bytes
 *   USBDEVFS_SUBMITURB      _IOR ('U', 10, struct usbdevfs_urb)           56 bytes
 *   USBDEVFS_REAPURBNDELAY  _IOW ('U', 13, void *)                         8 bytes
 *   USBDEVFS_CLAIMINTERFACE _IOR ('U', 15, unsigned int)                   4 bytes
 *   USBDEVFS_CLEAR_HALT     _IOR ('U', 21, unsigned int)                   4 bytes */
#define CP_USBDEVFS_CONTROL        0xc0185500u
#define CP_USBDEVFS_SUBMITURB      0x8038550au
#define CP_USBDEVFS_REAPURBNDELAY  0x4008550du
#define CP_USBDEVFS_CLAIMINTERFACE 0x8004550fu
#define CP_USBDEVFS_CLEAR_HALT     0x80045515u

/* The one urb type the host role submits, and the one flag it sets. */
#define CP_URB_TYPE_BULK   3u      /* USBDEVFS_URB_TYPE_BULK */
#define CP_URB_ZERO_PACKET 0x40u   /* USBDEVFS_URB_ZERO_PACKET */

/* The kernel reads both of these by offset and writes the completion back into
 * the same memory, so the layout is the ABI rather than a detail. Neither is
 * packed: the padding after `endpoint` and the padding before `buffer` are part
 * of what the kernel expects, and a packed struct would be two bytes short.
 * The assertions below are the transcription's own test - the sizes they pin
 * are the ones the ioctl numbers above encode. */
typedef struct {
	uint8_t  bRequestType;
	uint8_t  bRequest;
	uint16_t wValue;
	uint16_t wIndex;
	uint16_t wLength;
	uint32_t timeout;
	void*    data;
} CP_CtrlTransfer;

typedef struct {
	uint8_t  type;
	uint8_t  endpoint;
	int32_t  status;
	uint32_t flags;
	void*    buffer;
	int32_t  buffer_length;
	int32_t  actual_length;
	int32_t  start_frame;
	union {
		int32_t  number_of_packets;
		uint32_t stream_id;
	} u;
	int32_t  error_count;
	uint32_t signr;
	void*    usercontext;
} CP_Urb;

typedef char cp_ctrltransfer_is_24_bytes[sizeof(CP_CtrlTransfer) == 24 ? 1 : -1];
typedef char cp_urb_is_56_bytes[sizeof(CP_Urb) == 56 ? 1 : -1];

/* <linux/usb/ch9.h>, the handful of values needed to read a descriptor out of a
 * device and decide whether it is ours. */
#define CP_DT_DEVICE              1u
#define CP_DT_CONFIG              2u
#define CP_DT_INTERFACE           4u
#define CP_DT_ENDPOINT            5u
#define CP_USB_DIR_IN             0x80u
#define CP_USB_RECIP_DEVICE       0x00u
#define CP_USB_REQ_GET_DESCRIPTOR 6u
#define CP_MAXPACKET_MASK         0x07ffu
#define CP_USB_CTRL_TIMEOUT_MS    1000

//////////////////////////////////////////////////////////////////////////////
// cadence and limits
//////////////////////////////////////////////////////////////////////////////

/* Two cadences, for the two kinds of work in one loop.
 *
 * CP_TICK_MS is how long the poll behind the data path waits, and it is the
 * broker's tick (app/broker.c:98-103) for the same reason: it is a cadence, not
 * a latency. The status file, the peer question and the lifetime question are
 * not per-packet work, so they run on every tenth tick, exactly as the broker
 * throttles its own side effect (app/broker.c:102, `status_tick++ % 10`) - the
 * app reads the status file itself, and once a second is what that reader
 * needs.
 *
 * A record waiting for the transmit endpoint shortens the poll, because a held
 * packet is latency and a status refresh is not. */
#define CP_TICK_MS         100
#define CP_SIDE_EFFECT_MS  (CP_TICK_MS * 10)
#define CP_TX_RETRY_MS     5

/* How long a kernel step may take before it is reported as not having
 * happened. Both are deadline-bounded on purpose: nothing this daemon does may
 * be the reason a screen sits on "presenting USB gadget..." forever. */
#define CP_EP_WAIT_TICKS  20   /* ~2 s for the endpoint files after the strings blob */
#define CP_ADDR_WAIT_TICKS 10  /* ~1 s for the address to land on the interface */
#define CP_EP0_ATTEMPTS   10   /* ep0 writes that come back EAGAIN while the mutex is held */

/* How long the port's role attribute may take to answer before the attempt is
 * abandoned, and how many completions are reaped before the interface is looked
 * at again.
 *
 * The first is a deadline on a marker file rather than on a process, and that is
 * the whole point: the write behind it is the one thing in this file that can
 * wait forever, so nothing here ever waits on the writer.
 *
 * The second keeps one busy direction from starving the other. */
#define CP_ROLE_WAIT_TICKS 20   /* ~2 s to write one sysfs attribute and leave a marker */
#define CP_REAP_MAX        8

/* How often the host role looks for the peer on the bus. A gadget that has just
 * been plugged or powered takes a moment to appear, and a directory scan plus a
 * control transfer is not work to do ten times a second. */
#define CP_ATTACH_TICKS 5

/* The usb_restore record's line cap: one keyed line per thing that has to be put
 * back, and the two roles need two each - the controller's previous owner and
 * the controller itself for the gadget role, the port's attribute name and its
 * previous mode for the host role. Four is exactly that, and the cap is
 * deliberate: a record that has outgrown its reason is not one this daemon
 * should be writing. */
#define CP_RESTORE_MAX_LINES 4

#define CP_RESTORE_KEY_GADGET     "gadget"
#define CP_RESTORE_KEY_UDC        "udc"
#define CP_RESTORE_KEY_ROLE_NODE  "role_node"
#define CP_RESTORE_KEY_ROLE_VALUE "role_value"

/* The vendor OTG port, as three names and two values.
 *
 * The attribute name is ours to choose: cp_role_node takes it, so the
 * transcribed directory in CP_Facts is the only platform-specific half. The two
 * values are what the attribute is compared against and written with, and both
 * are the conventional spellings. What the vendor stack actually does with
 * either is the research's Open Question 2 and is verified on hardware, not
 * here - which is why a write that does not take effect is reported as "role not
 * confirmed" rather than assumed to have worked. */
#define CP_PORT_NODE_OTG "otg_role"
#define CP_PORT_ROLE_HOST     "host"
#define CP_PORT_ROLE_DEVICE   "device"

/* Every path this daemon drives is a fact of the platform rather than of the
 * device it runs on, so each one has an environment override. That is what lets
 * testing/test-cable.sh exercise the lifecycle - the pidfile, the status
 * grammar, the exit conditions - against a fake configfs tree and a fake TUN on
 * a machine that has neither. The convention (NP_<thing>) and the reason are
 * launcher/wifi-platform.sh:41-44's. SDCARD_PATH already selects the state
 * directory, because NS_init resolves it from there. NP_CABLE_ROLE_DIR=none is
 * how a caller says "this platform has no port-role nodes". */
#define CP_ENV_CONFIGFS "NP_CABLE_CONFIGFS"
#define CP_ENV_FFS_DIR  "NP_CABLE_FFS_DIR"
#define CP_ENV_UDC_DIR  "NP_CABLE_UDC_DIR"
#define CP_ENV_TUN      "NP_CABLE_TUN"
#define CP_ENV_ROLE_DIR "NP_CABLE_ROLE_DIR"
#define CP_ENV_USB_DIR  "NP_CABLE_USB_DIR"
#define CP_ROLE_NONE    "none"

/* Where the host role looks for the peer. A constant rather than a CP_Facts
 * entry: only this role needs it, it is the same path on every platform, and
 * the override below is what makes it testable off the device. */
#define CP_USB_DIR "/dev/bus/usb"

//////////////////////////////////////////////////////////////////////////////
// state this process owns
//////////////////////////////////////////////////////////////////////////////

static volatile sig_atomic_t cp_stopping;

static char cp_state_dir[CP_LINE_MAX];
static char cp_pid_path[CP_LINE_MAX];
static char cp_status_path[CP_LINE_MAX];
static char cp_log_path[CP_LINE_MAX];

static CP_Status cp_status;

/* The descriptors the daemon drives, so that teardown is one function that
 * unwinds whatever exists rather than a set of paths each bring-up step has to
 * remember to undo. -1 is "never opened". */
static int  cp_tun_fd  = -1;
static int  cp_ep_in   = -1;
static int  cp_ep_out  = -1;
static int  cp_ep0     = -1;
static int  cp_evfd    = -1;
static bool cp_gadget_bound;
static bool cp_ffs_mounted;
static bool cp_session_seen;

/* The receive thread talks to the main thread through these two words rather
 * than through the status file: a lock around every packet is a cost this link
 * does not need to pay, and the status file is published by the main thread
 * alone. */
static volatile sig_atomic_t cp_rx_records;
static volatile sig_atomic_t cp_rx_failed;
static volatile sig_atomic_t cp_rx_errno;

/* The one packet waiting for the transmit endpoint. One slot, not a queue -
 * while it is occupied the interface is simply not read, which is backpressure
 * rather than a buffer that can overflow. */
static uint8_t cp_tx_record[CP_RECORD_MAX];

/* The host role's device, its two endpoints, and the two URBs it keeps in the
 * kernel. The URBs are file scope because the kernel holds a pointer to them for
 * the whole life of the transfer and writes the completion back into the same
 * memory: they are the kernel's handle on the transfer, not this process's
 * scratch space.
 *
 * The read buffer is a whole number of packets at both speeds (4 x 512, 32 x 64)
 * for the same reason the gadget role's is: the controller fills what it is
 * given, and a request that is not a multiple of the packet size can only be one
 * that ends in the middle of what the peer is sending. */
static int      cp_usb_fd = -1;
static uint8_t  cp_usb_in_addr;
static uint8_t  cp_usb_out_addr;
static unsigned cp_usb_maxpacket = CP_MAXPACKET_HS;
/* Only the transmit side needs a flag: its record is only read from the
 * interface while no transfer is outstanding, which is the backpressure. The
 * receive URB is in flight for exactly as long as a peer is attached, and
 * saying so twice - once in a variable and once in the attachment - would be
 * two answers to one question. */
static bool     cp_urb_out_in_flight;
static CP_Urb   cp_urb_in;
static CP_Urb   cp_urb_out;
static uint8_t  cp_urb_in_buffer[CP_READ_MAX];
static const char* cp_usb_dir = CP_USB_DIR;

/* The role helper's result file. Named after this process so that two daemons
 * that somehow run at once cannot read each other's answer, and removed on the
 * way out of every attempt. */
static char cp_role_marker[CP_LINE_MAX];

//////////////////////////////////////////////////////////////////////////////
// logging
//
// The same shape as the app's and the watchdog's: wall-clock with milliseconds
// and a tag of our own, so one session's three logs read side by side.
//////////////////////////////////////////////////////////////////////////////

static void cp_stamp(char* out, size_t len) {
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

static void cp_log(const char* fmt, ...) {
	char ts[48];
	cp_stamp(ts, sizeof(ts));

	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "[%s] " CP_LOG_TAG, ts);
	vfprintf(stderr, fmt, args);
	va_end(args);
	fflush(stderr);
}

static long cp_ms_between(const struct timeval* then, const struct timeval* now) {
	return (now->tv_sec - then->tv_sec) * 1000L + (now->tv_usec - then->tv_usec) / 1000L;
}

//////////////////////////////////////////////////////////////////////////////
// small files
//////////////////////////////////////////////////////////////////////////////

static bool cp_file_exists(const char* path) {
	return path && path[0] && access(path, F_OK) == 0;
}

static bool cp_dir_exists(const char* path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* A sysfs or configfs attribute read as a value: one read, then trimmed of the
 * newline it is written with by the pure layer's rule. False means "not there,
 * or nothing but blanks in it" - an unbound UDC attribute, for instance. */
static bool cp_read_attr(const char* path, char* out, size_t cap) {
	if (!out || !cap) return false;
	out[0] = '\0';
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;
	ssize_t n = read(fd, out, cap - 1);
	close(fd);
	if (n <= 0) { out[0] = '\0'; return false; }
	out[n] = '\0';
	return cp_attr_trim(out);
}

/* One write of the whole value: sysfs and configfs parse what they are given in
 * a single call, so a short write is a different value rather than a partial
 * one. */
static bool cp_write_attr(const char* path, const char* value) {
	if (!path || !value) return false;
	int fd = open(path, O_WRONLY);
	if (fd < 0) return false;
	size_t want = strlen(value);
	ssize_t n = write(fd, value, want);
	close(fd);
	return n == (ssize_t)want;
}

static bool cp_mkdir_p(const char* path) {
	char buffer[CP_LINE_MAX];
	if (!path || !path[0]) return false;
	if (snprintf(buffer, sizeof(buffer), "%s", path) >= (int)sizeof(buffer)) return false;

	for (char* p = buffer + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return false;
		*p = '/';
	}
	return mkdir(buffer, 0755) == 0 || errno == EEXIST;
}

//////////////////////////////////////////////////////////////////////////////
// the platform's facts
//////////////////////////////////////////////////////////////////////////////

static CP_Facts cp_facts = {
	.configfs = "/sys/kernel/config",
	.ffs_dir  = "/dev/usb-ffs/cable",
	.udc_dir  = "/sys/class/udc",
	.tun_dev  = "/dev/net/tun",
	.role_dir = NULL,
	.gadget   = "netplay",
	.function = "cable",
	.iface    = CP_IFACE_NAME,
};

/* The one entry that is not the same on every platform. The vendor OTG role
 * nodes exist on the Allwinner parts and nowhere else. PLATFORM is a
 * compile-time string (app/makefile:51), so this stays a fact of the build
 * rather than something to probe, and a platform without them records no role
 * directory at all - which the pure layer answers with "no such node" and the
 * caller reports as "this device cannot force the port" instead of inventing a
 * path.) */
static const char* cp_role_dir_default(void) {
#ifdef PLATFORM
	if (!strcmp(PLATFORM, "tg5040") || !strcmp(PLATFORM, "tg5050"))
		return "/sys/devices/platform/soc/usbc0";
#endif
	return NULL;
}

static void cp_facts_init(void) {
	const char* env;

	if ((env = getenv(CP_ENV_CONFIGFS)) && env[0]) cp_facts.configfs = env;
	if ((env = getenv(CP_ENV_FFS_DIR))  && env[0]) cp_facts.ffs_dir  = env;
	if ((env = getenv(CP_ENV_UDC_DIR))  && env[0]) cp_facts.udc_dir  = env;
	if ((env = getenv(CP_ENV_TUN))      && env[0]) cp_facts.tun_dev  = env;

	cp_facts.role_dir = cp_role_dir_default();
	if ((env = getenv(CP_ENV_ROLE_DIR)) && env[0])
		cp_facts.role_dir = strcmp(env, CP_ROLE_NONE) ? env : NULL;

	if ((env = getenv(CP_ENV_USB_DIR)) && env[0]) cp_usb_dir = env;
}

//////////////////////////////////////////////////////////////////////////////
// the usb_restore record
//
// A record of a repair this device owes the firmware, kept until the repair
// succeeds. wifi_restore is the same idea for the radio (app/netsetup.c:2444-
// 2483) and the policy is copied deliberately: the record's existence *is* the
// recovery state, so only a repair that worked may remove it.
//
// Keyed rather than positional because the two roles write it for different
// reasons - the gadget role owes a controller rebind, the host role owes a port
// role - and neither may erase the other's line.
//////////////////////////////////////////////////////////////////////////////

typedef struct {
	char key[16];
	char value[CP_NAME_MAX];
} CP_RestoreLine;

static bool cp_restore_path(char* out, size_t cap) {
	int n = snprintf(out, cap, "%s/%s", NS_statePath(), CP_RESTORE_FILE);
	return n > 0 && (size_t)n < cap;
}

/* Unreadable lines are dropped rather than made to fail the file: the record's
 * job is to survive a crash, and half of it is worth more than none. */
static int cp_restore_load(CP_RestoreLine* lines, int max) {
	char path[CP_LINE_MAX];
	if (!cp_restore_path(path, sizeof(path))) return 0;

	FILE* f = fopen(path, "r");
	if (!f) return 0;

	int count = 0;
	char line[CP_LINE_MAX];
	while (count < max && fgets(line, sizeof(line), f)) {
		char* nl = strpbrk(line, "\r\n");
		if (nl) *nl = '\0';
		char* value = strchr(line, '=');
		if (!value) continue;
		*value++ = '\0';
		if (!line[0] || !cp_value_ok(value)) continue;
		snprintf(lines[count].key, sizeof(lines[count].key), "%s", line);
		snprintf(lines[count].value, sizeof(lines[count].value), "%s", value);
		count++;
	}
	fclose(f);
	return count;
}

static void cp_restore_save(const CP_RestoreLine* lines, int count) {
	char path[CP_LINE_MAX], temporary[CP_LINE_MAX + 32];
	if (!cp_restore_path(path, sizeof(path))) return;
	if (count <= 0) { remove(path); return; }
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%d", path, getpid()) >= (int)sizeof(temporary)) return;

	FILE* f = fopen(temporary, "w");
	if (!f) return;
	for (int i = 0; i < count; i++) fprintf(f, "%s=%s\n", lines[i].key, lines[i].value);
	if (fclose(f) == 0) rename(temporary, path);
	else remove(temporary);
}

/* Sets one key. An empty value removes it, and a record whose last key is
 * removed goes away: the file exists only while a repair is owed. */
static bool cp_restore_put(const char* key, const char* value) {
	CP_RestoreLine lines[CP_RESTORE_MAX_LINES];
	int count = cp_restore_load(lines, CP_RESTORE_MAX_LINES);

	int found = -1;
	for (int i = 0; i < count; i++)
		if (!strcmp(lines[i].key, key)) { found = i; break; }

	if (value && value[0]) {
		if (found < 0) {
			if (count >= CP_RESTORE_MAX_LINES) return false;
			found = count++;
			snprintf(lines[found].key, sizeof(lines[found].key), "%s", key);
		}
		snprintf(lines[found].value, sizeof(lines[found].value), "%s", value);
	} else if (found >= 0) {
		lines[found] = lines[--count];
	}

	cp_restore_save(lines, count);
	return true;
}

static bool cp_restore_get(const char* key, char* out, size_t cap) {
	CP_RestoreLine lines[CP_RESTORE_MAX_LINES];
	int count = cp_restore_load(lines, CP_RESTORE_MAX_LINES);

	if (out && cap) out[0] = '\0';
	for (int i = 0; i < count; i++)
		if (!strcmp(lines[i].key, key)) {
			if (out && cap) snprintf(out, cap, "%s", lines[i].value);
			return true;
		}
	return false;
}

//////////////////////////////////////////////////////////////////////////////
// the port's role
//
// Which end of a C-to-C cable enumerates is a decision, not a consequence: two
// dual-role ports negotiate CC with no way to prefer one, and two device-only
// sinks produce nothing at all. So the host role has to say "host" out loud.
//
// The saying is the problem. The vendor stack is reported to put a process that
// writes this attribute into uninterruptible sleep until reboot, and SIGKILL
// does not clear that state - so a helper that is waited on can hang the daemon
// forever, and a popen()/pclose() that bounds only the read hangs the same way.
// This process therefore never writes the attribute. A child writes it and
// leaves a marker; the parent polls for the marker against a deadline, reaps
// with WNOHANG, and treats a missing marker as "the port did not switch" -
// which is a sentence, not a hang and not a crash.
//
// The record is what makes the write defensible. Writing the attribute takes the
// port away from the firmware, which is the thing adb runs on, so the mode it
// was in goes to usb_restore *before* the switch - the same bargain the
// controller takeover makes, with the same keep-on-failure policy.
//////////////////////////////////////////////////////////////////////////////

static bool cp_role_marker_path(void) {
	int n = snprintf(cp_role_marker, sizeof(cp_role_marker), "%s/cable.role.%d",
	                 cp_state_dir, (int)getpid());
	return n > 0 && (size_t)n < sizeof(cp_role_marker);
}

/* The child's half of the marker protocol. Raw descriptors and _exit, no stdio:
 * this runs in a forked copy of a process that has been writing to its log, and
 * an exit that flushed an inherited buffer would duplicate the parent's output.
 * A marker that cannot be written leaves the parent to time out, which is the
 * same answer as a write that never returns - there is nothing better to say
 * from here. */
static void cp_marker_put(const char* text) {
	if (!cp_role_marker[0]) return;

	int fd = open(cp_role_marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return;
	ssize_t n = write(fd, text, strlen(text));
	close(fd);
	(void)n;
}

/* The half that can wedge, and it does nothing but this. The value is written
 * with a newline in one call, because sysfs attributes are parsed per write and
 * a two-call version could be read as a different value. */
static void cp_role_child(const char* node, const char* value) {
	char text[CP_NAME_MAX + 2];
	snprintf(text, sizeof(text), "%s\n", value);

	int fd = open(node, O_WRONLY);
	if (fd < 0) {
		char message[CP_LINE_MAX];
		snprintf(message, sizeof(message), "cannot open %s: %s\n", node, strerror(errno));
		cp_marker_put(message);
		_exit(0);
	}

	ssize_t n = write(fd, text, strlen(text));
	int saved = errno;
	close(fd);

	char message[CP_LINE_MAX];
	if (n == (ssize_t)strlen(text)) snprintf(message, sizeof(message), "ok\n");
	else snprintf(message, sizeof(message), "%s\n", n < 0 ? strerror(saved) : "short write");
	cp_marker_put(message);
	_exit(0);
}

static bool cp_marker_read(void) {
	char text[CP_LINE_MAX];

	int fd = open(cp_role_marker, O_RDONLY);
	if (fd < 0) return false;
	ssize_t n = read(fd, text, sizeof(text) - 1);
	close(fd);
	if (n <= 0) return false;
	text[n] = '\0';

	if (!cp_attr_trim(text)) return false;
	if (!strcmp(text, "ok")) return true;
	cp_log("the port-role helper: %s\n", text);
	return false;
}

/* The parent's half. Every exit removes the marker, so the next attempt starts
 * from nothing rather than from the last attempt's answer.
 *
 * The attribute is named rather than assumed, because the recorded name is what
 * the restore writes back to: a key that is written but never used would be a
 * record of a decision nobody is bound by. The name is checked for a path
 * separator here rather than at the restore, so every caller is covered by one
 * test - it can come out of a file, and a name carrying a '/' would address
 * something outside the port directory. */
static bool cp_role_write(const char* name, const char* value, char* err, int errlen) {
	char path[CP_LINE_MAX];

	if (!name || strchr(name, '/') || !cp_role_node(&cp_facts, name, path, sizeof(path))) {
		snprintf(err, errlen, "the port's role path does not fit");
		return false;
	}
	if (!cp_role_marker_path()) {
		snprintf(err, errlen, "the port-role marker path does not fit");
		return false;
	}
	remove(cp_role_marker);

	pid_t child = fork();
	if (child < 0) {
		snprintf(err, errlen, "cannot start the port-role helper: %s", strerror(errno));
		return false;
	}
	if (child == 0) cp_role_child(path, value);

	/* The child is reaped with WNOHANG and abandoned if it never finishes: a
	 * process in uninterruptible sleep cannot be reaped, and waiting for it
	 * would be the hang this whole arrangement exists to avoid. */
	bool ok = false;
	for (int tick = 0; tick < CP_ROLE_WAIT_TICKS && !ok; tick++) {
		(void)waitpid(child, NULL, WNOHANG);
		if (cp_file_exists(cp_role_marker)) ok = cp_marker_read();
		else usleep(CP_TICK_MS * 1000);
	}
	remove(cp_role_marker);

	if (!ok) {
		snprintf(err, errlen, "the port did not switch to %s (role not confirmed)", value);
		return false;
	}
	return true;
}

/* Switch the port, with the record written first.
 *
 * An attribute that cannot be read is refused rather than written: without the
 * mode it is in there is nothing to record, and an unrecorded write is exactly
 * the case the record exists to prevent - a device left in host mode with adb
 * dead and nothing that knows how to put it back. Refusing is reported in the
 * same words as a failed write, because from the outside they are the same
 * answer.
 *
 * A platform with no role attribute is not a failure: the link then depends on
 * the port's own negotiation, which is what the firmware does by default, and
 * "no cable peer" is what the app will report. */
static bool cp_role_force(char* err, int errlen) {
	char path[CP_LINE_MAX], current[CP_NAME_MAX];

	if (!cp_facts.role_dir) {
		cp_log("no port-role attribute on this platform; relying on the port's own negotiation\n");
		return true;
	}
	if (!cp_role_node(&cp_facts, CP_PORT_NODE_OTG, path, sizeof(path))) {
		snprintf(err, errlen, "the port's role path does not fit");
		return false;
	}
	if (!cp_read_attr(path, current, sizeof(current))) {
		snprintf(err, errlen, "cannot read %s, so the port cannot be switched safely", path);
		return false;
	}
	if (!strcmp(current, CP_PORT_ROLE_HOST)) {
		cp_log("the port is already in host mode\n");
		return true;
	}

	if (!cp_restore_put(CP_RESTORE_KEY_ROLE_NODE, CP_PORT_NODE_OTG) ||
	    !cp_restore_put(CP_RESTORE_KEY_ROLE_VALUE, current)) {
		snprintf(err, errlen, "cannot record the port's %s mode before changing it", current);
		return false;
	}
	cp_log("the port is in %s mode; switching it to %s\n", current, CP_PORT_ROLE_HOST);

	if (!cp_role_write(CP_PORT_NODE_OTG, CP_PORT_ROLE_HOST, err, errlen)) return false;
	cp_log("the port is in host mode\n");
	return true;
}

/* Put the port back, and forget the record only once that has worked - the same
 * policy as the controller's. The attribute written is the one the record names,
 * not the one this build prefers, because the record describes the device as it
 * was rather than as this build assumes it is. */
static void cp_role_restore(void) {
	char node[CP_NAME_MAX], value[CP_NAME_MAX], err[CP_ERROR_MAX];

	if (!cp_restore_get(CP_RESTORE_KEY_ROLE_NODE, node, sizeof(node)) ||
	    !cp_restore_get(CP_RESTORE_KEY_ROLE_VALUE, value, sizeof(value))) return;

	if (!cp_role_write(node, value, err, sizeof(err))) {
		cp_log("could not put the port back to %s: %s\n", value, err);
		return;
	}
	cp_log("the port is back in %s mode\n", value);
	cp_restore_put(CP_RESTORE_KEY_ROLE_NODE, "");
	cp_restore_put(CP_RESTORE_KEY_ROLE_VALUE, "");
}

/* The gadget role's use of the same mechanism, and the only reason it is here:
 * this device boots into device/OTG mode, and something may have left the port
 * in host mode - an earlier session killed before its restore, another tool. A
 * gadget bound to a port that is not presenting as a device is never enumerated,
 * so the check has to come first.
 *
 * This is the one port write that owes no record: device mode is what the
 * firmware already assumes, and it is where adb lives, so leaving it there after
 * a crash is leaving it where it belongs.
 *
 * An unreadable attribute is passed over rather than treated as host mode: not
 * knowing the current mode is not evidence of the wrong one, and this step's job
 * is to remove a specific obstacle, not to assert a state. */
static bool cp_port_device(char* err, int errlen) {
	char path[CP_LINE_MAX], current[CP_NAME_MAX];

	if (!cp_facts.role_dir) return true;
	if (!cp_role_node(&cp_facts, CP_PORT_NODE_OTG, path, sizeof(path))) return true;
	if (!cp_read_attr(path, current, sizeof(current))) return true;
	if (strcmp(current, CP_PORT_ROLE_HOST)) return true;

	cp_log("the port is in host mode; returning it to device mode\n");
	return cp_role_write(CP_PORT_NODE_OTG, CP_PORT_ROLE_DEVICE, err, errlen);
}

//////////////////////////////////////////////////////////////////////////////
// paths, pidfile, status
//////////////////////////////////////////////////////////////////////////////

static bool cp_paths_init(void) {
	const char* state = NS_statePath();
	if (!state || !state[0]) return false;
	if (snprintf(cp_state_dir, sizeof(cp_state_dir), "%s", state) >= (int)sizeof(cp_state_dir)) return false;
	if (snprintf(cp_pid_path, sizeof(cp_pid_path), "%s/%s", state, CP_PID_FILE) >= (int)sizeof(cp_pid_path)) return false;
	if (snprintf(cp_status_path, sizeof(cp_status_path), "%s/%s", state, CP_STATUS_FILE) >= (int)sizeof(cp_status_path)) return false;
	if (snprintf(cp_log_path, sizeof(cp_log_path), "%s/%s", state, CP_LOG_FILE) >= (int)sizeof(cp_log_path)) return false;
	return true;
}

/* One generation, by size, before the daemon writes its first line.
 *
 * The rename has to happen here rather than in whatever started us: the log is
 * opened for append before this process exists, so a rotation done by someone
 * else would leave this process's descriptors on the rotated inode.
 * Re-opening after the rename and putting the result back on both descriptors
 * keeps the two ends of the log the same file.
 * NETPLAY_CABLE_LOG_MAX overrides the cap, as NETPLAY_WATCHDOG_LOG_MAX does for
 * launcher/wifi-watchdog.sh:56-62. */
static void cp_log_open(void) {
	long max = 65536;
	const char* env = getenv(CP_LOG_MAX_ENV);
	if (env && env[0]) {
		char* end = NULL;
		long value = strtol(env, &end, 10);
		if (end && !*end && value > 0) max = value;
	}

	struct stat st;
	if (stat(cp_log_path, &st) == 0 && st.st_size > max) {
		char backup[CP_LINE_MAX + 4];
		if (snprintf(backup, sizeof(backup), "%s.1", cp_log_path) < (int)sizeof(backup)) {
			remove(backup);
			rename(cp_log_path, backup);   /* a failure here only means the log keeps growing */
		}
	}

	int fd = open(cp_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) return;
	dup2(fd, STDOUT_FILENO);
	dup2(fd, STDERR_FILENO);
	if (fd > STDERR_FILENO) close(fd);
}

/* The pidfile is the single-instance lock and the app's handle on us
 * (app/broker.c:27-36). O_EXCL is what makes it one. */
static bool cp_pid_write(void) {
	int fd = open(cp_pid_path, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0) return false;

	char pid[32];
	int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
	bool ok = write(fd, pid, (size_t)n) == n;
	close(fd);
	if (!ok) remove(cp_pid_path);
	return ok;
}

static void cp_publish(void) {
	char text[CP_STATUS_MAX], temporary[CP_LINE_MAX + 32];
	int n = cp_status_write(&cp_status, text, sizeof(text));
	if (n <= 0) return;
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%d", cp_status_path, getpid()) >= (int)sizeof(temporary)) return;

	FILE* f = fopen(temporary, "w");
	if (!f) return;
	bool written = fwrite(text, 1, (size_t)n, f) == (size_t)n;
	if (fclose(f) == 0 && written) rename(temporary, cp_status_path);
	else remove(temporary);
}

/* Every status field goes through the checked setter, so a key that does not
 * exist or a value the reader would have to reject fails at the writer instead
 * of becoming a line nobody can read. A refusal here is a bug in this file, and
 * it is worth a line in the log rather than a silently missing field. */
static bool cp_status_put(const char* key, const char* value) {
	if (cp_status_set(&cp_status, key, value)) return true;
	cp_log("internal: the status grammar refused %s\n", key);
	return false;
}

/* Every failure goes through here: the state, the message the app will show and
 * the log line are one action rather than three that can disagree. */
static void cp_fail(const char* fmt, ...) {
	char message[CP_ERROR_MAX];

	va_list args;
	va_start(args, fmt);
	vsnprintf(message, sizeof(message), fmt, args);
	va_end(args);

	/* The status grammar refuses a value carrying a line break, because such a
	 * value would forge a line. Our own messages are formatted, not quoted, so
	 * the defence is here rather than at the caller. */
	for (char* p = message; *p; p++)
		if ((unsigned char)*p < 0x20 || (unsigned char)*p == 0x7f) *p = ' ';

	cp_status_put("error", message);
	cp_status_set_state(&cp_status, CP_STATE_FAILED);
	cp_publish();
	cp_log("%s\n", message);
}

static void cp_stop_signal(int sig) {
	(void)sig;
	cp_stopping = 1;
}

//////////////////////////////////////////////////////////////////////////////
// the point-to-point interface
//
// Layer 3 with no packet information header, so a read gives one IP packet and
// there is nothing to strip. The interface belongs to this process: closing the
// descriptor destroys it, which is why nothing here removes it afterwards.
//////////////////////////////////////////////////////////////////////////////

static int cp_tun_open(char* err, int errlen) {
	int fd = open(cp_facts.tun_dev, O_RDWR);
	if (fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s", cp_facts.tun_dev, strerror(errno));
		return -1;
	}

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = CP_IFF_TUN | CP_IFF_NO_PI;
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", cp_facts.iface);

	if (ioctl(fd, CP_TUNSETIFF, &ifr) < 0) {
		snprintf(err, errlen, "cannot create %s: %s", cp_facts.iface, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

/* The address the kernel actually put on the interface, read back rather than
 * assumed: the status file's ip= is handed to the peer's side as the address to
 * reach, so it has to be what is, not what was asked for. */
static bool cp_iface_address(char* out, size_t cap) {
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	if (s < 0) return false;

	struct ifreq ifr;
	memset(&ifr, 0, sizeof(ifr));
	snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", cp_facts.iface);

	bool found = false;
	if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
		const struct sockaddr_in* sin = (const struct sockaddr_in*)&ifr.ifr_addr;
		char text[INET_ADDRSTRLEN];
		if (inet_ntop(AF_INET, &sin->sin_addr, text, sizeof(text))) {
			snprintf(out, cap, "%s", text);
			found = true;
		}
	}
	close(s);
	return found;
}

/* Shelled out for the same reason the hotspot path does it (app/netsetup.c:
 * 2640-2642): `ip` is what this device has, and the address has to be on the
 * interface before the peer's first packet arrives. The address is then
 * confirmed by reading it back, because a command that returned zero is not
 * evidence that the address landed. */
static bool cp_iface_up(const char* ip, char* err, int errlen) {
	char command[512];
	snprintf(command, sizeof(command),
	         "ip addr add %s/%s dev %s 2>/dev/null; "
	         "ip link set dev %s mtu %d 2>/dev/null; "
	         "ip link set dev %s up 2>/dev/null",
	         ip, NS_CABLE_PREFIX, cp_facts.iface, cp_facts.iface, CP_MTU, cp_facts.iface);
	if (system(command) != 0) {
		snprintf(err, errlen, "cannot bring %s up", cp_facts.iface);
		return false;
	}

	for (int i = 0; i < CP_ADDR_WAIT_TICKS; i++) {
		char address[CP_ADDR_MAX];
		if (cp_iface_address(address, sizeof(address)) && !strcmp(address, ip)) return true;
		usleep(CP_TICK_MS * 1000);
	}

	snprintf(err, errlen, "%s came up without an address", cp_facts.iface);
	return false;
}

//////////////////////////////////////////////////////////////////////////////
// the gadget
//
// The order is the kernel's, not ours: the gadget directory, its identity, a
// function instance, a configuration with that function linked into it, and
// only then the controller. The identity pair is deliberately unassigned
// (app/usbnet.h): nothing binds it automatically, so the other end's host role
// reaches the interface through usbfs without detaching a driver, which is what
// a class the kernel knows would cost.
//////////////////////////////////////////////////////////////////////////////

/* <configfs>/usb_gadget/<name>/<attr> - the one path this file builds itself,
 * because the gadget it names is not ours: the controller's current owner is
 * discovered at run time and only its name is known. It follows the pure
 * layer's rule - refuse a path that does not fit rather than return a
 * truncated one, which would open a different file and report it as a missing
 * attribute. */
static bool cp_gadget_path(const char* name, const char* attr, char* out, size_t cap) {
	if (!name || !name[0] || !attr || !out || !cap) return false;

	char dir[CP_LINE_MAX];
	int n = snprintf(dir, sizeof(dir), "%s/usb_gadget/%s", cp_facts.configfs, name);
	if (n <= 0 || (size_t)n >= sizeof(dir)) return false;

	n = snprintf(out, cap, "%s/%s", dir, attr);
	return n > 0 && (size_t)n < cap;
}

/* configfs is where a gadget is built, and on this firmware it is already
 * mounted because the adbd gadget lives in it. Mounting is therefore the repair
 * for the case where it is not, rather than part of the happy path. */
static bool cp_configfs_ensure(char* err, int errlen) {
	char group[CP_LINE_MAX];
	if (snprintf(group, sizeof(group), "%s/usb_gadget", cp_facts.configfs) >= (int)sizeof(group)) {
		snprintf(err, errlen, "the configfs path does not fit");
		return false;
	}
	if (cp_dir_exists(group)) return true;

	if (mkdir(cp_facts.configfs, 0755) != 0 && errno != EEXIST) {
		snprintf(err, errlen, "cannot use %s: %s", cp_facts.configfs, strerror(errno));
		return false;
	}
	if (mount("none", cp_facts.configfs, "configfs", 0, NULL) != 0 && errno != EBUSY) {
		snprintf(err, errlen, "cannot mount configfs on %s: %s", cp_facts.configfs, strerror(errno));
		return false;
	}
	if (!cp_dir_exists(group)) {
		snprintf(err, errlen, "%s has no usb_gadget group - is the gadget stack built in?", cp_facts.configfs);
		return false;
	}
	return true;
}

static bool cp_gadget_set(const char* attr, const char* value, char* err, int errlen) {
	char path[CP_LINE_MAX];
	if (!cp_gadget_attr(&cp_facts, attr, path, sizeof(path))) {
		snprintf(err, errlen, "the path to %s does not fit", attr);
		return false;
	}
	if (!cp_write_attr(path, value)) {
		snprintf(err, errlen, "cannot write %s: %s", attr, strerror(errno));
		return false;
	}
	return true;
}

/* Identity, function, configuration, in that order. Only the parts the kernel
 * requires are written - no product string, no serial number: a host that shows
 * nothing next to the device is the correct answer for a link that only ever
 * has our own daemon at the other end. */
static bool cp_gadget_build(char* err, int errlen) {
	char dir[CP_LINE_MAX], path[CP_LINE_MAX], function[CP_LINE_MAX], value[32];

	if (!cp_gadget_dir(&cp_facts, dir, sizeof(dir))) {
		snprintf(err, errlen, "the gadget path does not fit");
		return false;
	}
	/* EEXIST is a gadget a previous run left behind; the boot-time pass removes
	 * it, and until it does, reusing it is what the kernel allows. */
	if (!cp_mkdir_p(dir)) {
		snprintf(err, errlen, "cannot create %s: %s", dir, strerror(errno));
		return false;
	}

	snprintf(value, sizeof(value), "0x%04x", CP_VID);
	if (!cp_gadget_set("idVendor", value, err, errlen)) return false;
	snprintf(value, sizeof(value), "0x%04x", CP_PID);
	if (!cp_gadget_set("idProduct", value, err, errlen)) return false;
	snprintf(value, sizeof(value), "0x%04x", CP_BCD_DEVICE);
	if (!cp_gadget_set("bcdDevice", value, err, errlen)) return false;

	if (!cp_gadget_function_dir(&cp_facts, function, sizeof(function))) {
		snprintf(err, errlen, "the function path does not fit");
		return false;
	}
	if (!cp_mkdir_p(function)) {
		snprintf(err, errlen, "cannot create %s: %s", function, strerror(errno));
		return false;
	}

	if (snprintf(path, sizeof(path), "%s/configs/c.1", dir) >= (int)sizeof(path)) {
		snprintf(err, errlen, "the configuration path does not fit");
		return false;
	}
	if (!cp_mkdir_p(path)) {
		snprintf(err, errlen, "cannot create %s: %s", path, strerror(errno));
		return false;
	}

	/* What we ask the other end for. 100 mA is the amount any USB 2.0 host must
	 * offer before anything is negotiated, and the host here is another
	 * battery-powered handheld rather than a powered hub: asking for more than
	 * it can give is how a peer ends up unable to configure us at all. */
	if (snprintf(path, sizeof(path), "%s/configs/c.1/MaxPower", dir) >= (int)sizeof(path)) {
		snprintf(err, errlen, "the MaxPower path does not fit");
		return false;
	}
	if (!cp_write_attr(path, "100")) {
		snprintf(err, errlen, "cannot set MaxPower: %s", strerror(errno));
		return false;
	}

	/* The link inside the configuration has to carry the function directory's
	 * own name, so the name is taken from that path rather than spelled again
	 * here. */
	const char* instance = strrchr(function, '/');
	if (!instance || !instance[1]) {
		snprintf(err, errlen, "the function path has no instance name");
		return false;
	}
	if (snprintf(path, sizeof(path), "%s/configs/c.1/%s", dir, instance + 1) >= (int)sizeof(path)) {
		snprintf(err, errlen, "the function link path does not fit");
		return false;
	}
	if (symlink(function, path) != 0 && errno != EEXIST) {
		snprintf(err, errlen, "cannot link %s into the configuration: %s", instance + 1, strerror(errno));
		return false;
	}
	return true;
}

/* Which gadget holds the controller, and to which controller. Discovered rather
 * than assumed: the firmware's gadget has whatever name its build gave it, and
 * adb works today precisely because nothing here guesses. */
static bool cp_udc_owner(char* gadget, size_t gadget_cap, char* udc, size_t udc_cap) {
	char root[CP_LINE_MAX];
	if (snprintf(root, sizeof(root), "%s/usb_gadget", cp_facts.configfs) >= (int)sizeof(root)) return false;

	DIR* dir = opendir(root);
	if (!dir) return false;

	bool found = false;
	struct dirent* entry;
	while (!found && (entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') continue;
		if (!strcmp(entry->d_name, cp_facts.gadget)) continue;   /* ours, if a previous run left it */

		char path[CP_LINE_MAX], value[CP_NAME_MAX];
		if (!cp_gadget_path(entry->d_name, "UDC", path, sizeof(path))) continue;
		if (!cp_read_attr(path, value, sizeof(value)) || !value[0]) continue;

		snprintf(gadget, gadget_cap, "%s", entry->d_name);
		snprintf(udc, udc_cap, "%s", value);
		found = true;
	}
	closedir(dir);
	return found;
}

/* The controller's name, as its own attribute wants it: the directory name
 * under the UDC list. Read rather than constant, because it is the kernel's
 * name for the controller and it is what the attribute is matched against. */
static bool cp_udc_name(char* out, size_t cap) {
	DIR* dir = opendir(cp_facts.udc_dir);
	if (!dir) return false;

	bool found = false;
	struct dirent* entry;
	while (!found && (entry = readdir(dir)) != NULL) {
		if (entry->d_name[0] == '.') continue;
		snprintf(out, cap, "%s", entry->d_name);
		found = true;
	}
	closedir(dir);
	return found;
}

/* Whether a host has enumerated and configured us - the same question
 * PLAT_isUSBConnected answers for the stock UI
 * (NextUI/workspace/tg5040/platform/platform.c:192-215), asked of the
 * controller we bound rather than of every controller on the device. */
static bool cp_udc_configured(const char* udc) {
	char path[CP_LINE_MAX], state[32];
	if (!udc || !udc[0]) return false;
	if (snprintf(path, sizeof(path), "%s/%s/state", cp_facts.udc_dir, udc) >= (int)sizeof(path)) return false;
	return cp_read_attr(path, state, sizeof(state)) && !strncmp(state, "configured", 10);
}

/* The packet size a record is delimited against, at the speed the controller
 * actually negotiated. The aligned case - the record whose length is an exact
 * multiple of the packet size, the one that needs an explicit terminating
 * packet - is the shape of 512 at high speed and of 64 at full speed, so the
 * answer has to come from the controller rather than from a constant. Anything
 * that is not high speed is treated as full speed: the descriptor blob declares
 * no super-speed list, because this SoC is USB 2.0. */
static unsigned cp_maxpacket_for(const char* udc) {
	char path[CP_LINE_MAX], speed[32];
	if (!udc || !udc[0]) return CP_MAXPACKET_FS;
	if (snprintf(path, sizeof(path), "%s/%s/current_speed", cp_facts.udc_dir, udc) >= (int)sizeof(path)) return CP_MAXPACKET_FS;
	if (!cp_read_attr(path, speed, sizeof(speed))) return CP_MAXPACKET_FS;
	return !strcmp(speed, "high-speed") ? CP_MAXPACKET_HS : CP_MAXPACKET_FS;
}

/* The mount source is the instance name *without* its ffs. prefix: configfs
 * names the directory ffs.cable and the mount has to name "cable"
 * (Documentation/usb/functionfs.txt, 4.9). Getting that wrong is an ENOENT that
 * reads like the filesystem is missing, which is why both spellings come from
 * one fact. */
static bool cp_ffs_mount(char* err, int errlen) {
	char source[CP_NAME_MAX];

	if (!cp_ffs_source(&cp_facts, source, sizeof(source))) {
		snprintf(err, errlen, "the functionfs source does not fit");
		return false;
	}
	if (!cp_dir_exists(cp_facts.ffs_dir) && !cp_mkdir_p(cp_facts.ffs_dir)) {
		snprintf(err, errlen, "cannot create %s: %s", cp_facts.ffs_dir, strerror(errno));
		return false;
	}
	if (mount(source, cp_facts.ffs_dir, "functionfs", 0, NULL) != 0) {
		/* A mount left by a previous run is a state we can use rather than a
		 * failure: the files in it belong to this instance name, and the
		 * kernel would refuse to mount a second one over the same directory. */
		if (errno != EBUSY) {
			snprintf(err, errlen, "cannot mount functionfs on %s: %s", cp_facts.ffs_dir, strerror(errno));
			return false;
		}
		cp_log("reusing the functionfs mount left on %s\n", cp_facts.ffs_dir);
	}
	cp_ffs_mounted = true;
	return true;
}

/* One write, the whole blob, or nothing. The kernel checks the blob's own
 * length field against the number of bytes written (drivers/usb/gadget/
 * function/f_fs.c: get_unaligned_le32(data + 4) != len), so a short write is a
 * rejected gadget rather than a smaller one. */
static bool cp_ep0_write(const uint8_t* blob, size_t len, const char* what, char* err, int errlen) {
	for (int attempt = 0; attempt < CP_EP0_ATTEMPTS; attempt++) {
		ssize_t n = write(cp_ep0, blob, len);
		if (n == (ssize_t)len) return true;
		if (n < 0 && errno == EAGAIN) { usleep(CP_TICK_MS * 1000); continue; }
		snprintf(err, errlen, "the kernel refused the %s blob: %s", what,
		         n < 0 ? strerror(errno) : "short write");
		return false;
	}
	snprintf(err, errlen, "the kernel would not take the %s blob", what);
	return false;
}

/* Open ep0, then hand the kernel the two blobs, in the only order it accepts.
 * Both count and layout are the pure layer's (app/cableproto.c), which is where
 * they are pinned byte for byte. */
static bool cp_ffs_open_ep0(char* err, int errlen) {
	char path[CP_LINE_MAX];
	uint8_t blob[CP_DESCS_MAX];

	if (!cp_ffs_ep_file(&cp_facts, "ep0", path, sizeof(path))) {
		snprintf(err, errlen, "the ep0 path does not fit");
		return false;
	}
	cp_ep0 = open(path, O_RDWR | O_NONBLOCK);
	if (cp_ep0 < 0) {
		snprintf(err, errlen, "cannot open %s: %s", path, strerror(errno));
		return false;
	}

	/* The eventfd has to exist before the blob is written: the kernel takes it
	 * out of the descriptor header, so it cannot be attached afterwards. */
	cp_evfd = eventfd(0, EFD_NONBLOCK);
	if (cp_evfd < 0) {
		snprintf(err, errlen, "cannot create an eventfd: %s", strerror(errno));
		return false;
	}

	int n = cp_build_descriptors(blob, sizeof(blob), cp_evfd);
	if (n <= 0) {
		snprintf(err, errlen, "cannot build the descriptor blob");
		return false;
	}
	if (!cp_ep0_write(blob, (size_t)n, "descriptors", err, errlen)) return false;

	n = cp_build_strings(blob, sizeof(blob));
	if (n <= 0) {
		snprintf(err, errlen, "cannot build the strings blob");
		return false;
	}
	if (!cp_ep0_write(blob, (size_t)n, "strings", err, errlen)) return false;
	return true;
}

/* The endpoint files appear when the strings blob is accepted: that write is
 * what makes the function active in 4.9 (f_fs.c: the strings branch of
 * ffs_ep0_write calls ffs_epfiles_create), and the kernel creates every
 * declared endpoint's file with it. There is no `ready` attribute to wait on in
 * this kernel - it arrives years later - so the files themselves are the
 * readiness signal, they are waited for with a deadline, and the controller is
 * only written after they are open.
 *
 * The transmit side is opened non-blocking so that a stalled host cannot park
 * the loop that also has to service ep0; the receive side is opened blocking,
 * because it is read from a thread of its own (see cp_rx_worker) and there is
 * nothing else for that thread to do. */
static bool cp_eps_open(char* err, int errlen) {
	char path[CP_LINE_MAX];

	for (int tick = 0; tick < CP_EP_WAIT_TICKS; tick++) {
		if (!cp_ffs_ep_file(&cp_facts, CP_EP_FILE_IN, path, sizeof(path))) {
			snprintf(err, errlen, "the %s path does not fit", CP_EP_FILE_IN);
			return false;
		}
		if (cp_ep_in < 0) cp_ep_in = open(path, O_RDWR | O_NONBLOCK);

		if (!cp_ffs_ep_file(&cp_facts, CP_EP_FILE_OUT, path, sizeof(path))) {
			snprintf(err, errlen, "the %s path does not fit", CP_EP_FILE_OUT);
			return false;
		}
		if (cp_ep_out < 0) cp_ep_out = open(path, O_RDWR);

		if (cp_ep_in >= 0 && cp_ep_out >= 0) {
			cp_log("endpoints ready: %s -> host, %s -> us\n", CP_EP_FILE_IN, CP_EP_FILE_OUT);
			return true;
		}
		usleep(CP_TICK_MS * 1000);
	}

	snprintf(err, errlen, "the gadget never presented its endpoints");
	return false;
}

/* The order inside this function is the whole point of it: the record is
 * written first, then the current owner is released, then ours is bound. A
 * crash between any two of those steps leaves a record that says what to put
 * back, which is the only thing that makes taking the controller defensible -
 * the firmware's adb lives on the other side of it. */
static bool cp_udc_take(const char* udc, char* err, int errlen) {
	char owner_gadget[CP_NAME_MAX], owner_udc[CP_NAME_MAX], path[CP_LINE_MAX];

	if (cp_udc_owner(owner_gadget, sizeof(owner_gadget), owner_udc, sizeof(owner_udc))) {
		if (!cp_restore_put(CP_RESTORE_KEY_GADGET, owner_gadget) ||
		    !cp_restore_put(CP_RESTORE_KEY_UDC, owner_udc)) {
			snprintf(err, errlen, "cannot record who holds %s before taking it", udc);
			return false;
		}
		cp_log("%s is held by %s; taking it\n", udc, owner_gadget);

		/* Releasing is the empty string: the attribute is unbound by writing a
		 * line with nothing on it. A refusal here is not fatal - the bind below
		 * will fail with EBUSY and say so - but it is worth knowing which step
		 * said no. */
		if (cp_gadget_path(owner_gadget, "UDC", path, sizeof(path)) && !cp_write_attr(path, "\n"))
			cp_log("could not release %s cleanly: %s\n", owner_gadget, strerror(errno));
	}

	if (!cp_gadget_set("UDC", udc, err, errlen)) {
		char inside[CP_ERROR_MAX];
		snprintf(inside, sizeof(inside), "%s", err);
		snprintf(err, errlen, "cannot bind the gadget to %s (%s)", udc, inside);
		return false;
	}
	cp_gadget_bound = true;
	cp_log("bound to %s\n", udc);
	return true;
}

/* Hands the controller back to the gadget that had it, and forgets the record
 * only once that has worked - the same keep-on-failure policy as the radio's
 * breadcrumb. */
static void cp_udc_restore(void) {
	char gadget[CP_NAME_MAX], udc[CP_NAME_MAX], path[CP_LINE_MAX];

	if (!cp_restore_get(CP_RESTORE_KEY_GADGET, gadget, sizeof(gadget)) ||
	    !cp_restore_get(CP_RESTORE_KEY_UDC, udc, sizeof(udc))) return;
	if (!cp_gadget_path(gadget, "UDC", path, sizeof(path))) return;
	if (!cp_write_attr(path, udc)) {
		cp_log("could not hand %s back to %s: %s\n", udc, gadget, strerror(errno));
		return;
	}
	cp_log("handed %s back to %s\n", udc, gadget);
	cp_restore_put(CP_RESTORE_KEY_UDC, "");
	cp_restore_put(CP_RESTORE_KEY_GADGET, "");
}

/* Removes what was built, leaving anything that predates us alone: configfs
 * refuses to remove a bound gadget's directories, so this runs after the
 * controller has been released. A step that never happened is a step whose
 * removal fails harmlessly, which is why none of these is checked. */
static void cp_gadget_remove(void) {
	char dir[CP_LINE_MAX], path[CP_LINE_MAX], function[CP_LINE_MAX];

	if (!cp_gadget_dir(&cp_facts, dir, sizeof(dir))) return;
	if (cp_gadget_function_dir(&cp_facts, function, sizeof(function))) {
		const char* instance = strrchr(function, '/');
		if (instance && instance[1] &&
		    snprintf(path, sizeof(path), "%s/configs/c.1/%s", dir, instance + 1) < (int)sizeof(path))
			remove(path);
		rmdir(function);
	}
	if (snprintf(path, sizeof(path), "%s/configs/c.1", dir) < (int)sizeof(path)) rmdir(path);
	rmdir(dir);
}

//////////////////////////////////////////////////////////////////////////////
// the receive thread
//
// The receive direction runs on its own thread because the endpoint files have
// no poll(): in 4.9 only ep0 implements one (f_fs.c: ffs_ep0_poll, and the ep
// file operations have no .poll slot), so a poll over ep1/ep2 always reports
// ready - DEFAULT_POLLMASK, in fs/select.c - and a non-blocking read taken on
// that advice would spin. Blocking in a thread is what the shim does for the
// same reason (shim/netlink.c, one worker thread).
//
// It touches no state but the two words below: the main thread publishes, and a
// lock around every packet is a cost this link does not need to pay.
//////////////////////////////////////////////////////////////////////////////

static void* cp_rx_worker(void* arg) {
	(void)arg;
	uint8_t buffer[CP_READ_MAX];
	const uint8_t* packet = NULL;

	while (!cp_stopping) {
		ssize_t n = read(cp_ep_out, buffer, sizeof(buffer));
		if (n < 0) {
			if (errno == EINTR || errno == EAGAIN) continue;

			/* The peer went away or the endpoint was disabled under us: the
			 * same read is valid again as soon as a host configures the
			 * interface, so this is a pause rather than the end of the link -
			 * and the pause is bounded, because an endpoint that refuses every
			 * read must not become a spin. */
			if (errno == ESHUTDOWN || errno == EPROTO || errno == ENODEV || errno == EIO) {
				cp_log("receive endpoint reset: %s\n", strerror(errno));
				usleep(CP_TICK_MS * 1000);
				continue;
			}
			cp_rx_errno = errno;
			cp_rx_failed = 1;
			return NULL;
		}

		/* A zero-length read is the terminating packet that ended an aligned
		 * record: the record itself arrived in the transfer before it. */
		if (n == 0) continue;

		int len = cp_frame_decode(buffer, (size_t)n, &packet);
		if (len < 0) {
			/* The writer puts one whole record in one transfer, so a transfer
			 * that is not a whole record is a stream that has already lost its
			 * framing - and forwarding a guess from here would be worse than
			 * stopping. */
			cp_rx_errno = EPROTO;
			cp_rx_failed = 1;
			return NULL;
		}
		if (len == 0) continue;

		if (write(cp_tun_fd, packet, (size_t)len) < 0 && errno != EAGAIN) {
			cp_rx_errno = errno;
			cp_rx_failed = 1;
			return NULL;
		}
		cp_rx_records++;
	}
	return NULL;
}

//////////////////////////////////////////////////////////////////////////////
// the data path, transmit side
//////////////////////////////////////////////////////////////////////////////

/* One packet from the interface into the record slot. The slot is only read
 * when it is empty, which is the backpressure: an endpoint that is not taking
 * packets stops the interface being read, instead of a queue that grows. */
static int cp_tun_read(void) {
	uint8_t packet[CP_READ_MAX];

	ssize_t n = read(cp_tun_fd, packet, sizeof(packet));
	if (n <= 0) return 0;

	int record = cp_frame_encode(cp_tx_record, sizeof(cp_tx_record), packet, (size_t)n);
	if (record <= 0) {
		cp_log("dropping a %d-byte packet the link cannot carry\n", (int)n);
		return 0;
	}
	return record;
}

/* Hands the waiting record to the endpoint, and writes the terminating packet
 * when the record's length is an exact multiple of the packet size - 4.9's f_fs
 * appends nothing for us, and a transfer with no short packet leaves the peer
 * waiting for the rest of something that will never come. The question is the
 * pure layer's, so both ends answer it the same way. */
static void cp_tx_flush(int* pending, unsigned maxpacket) {
	if (*pending <= 0) return;

	ssize_t n = write(cp_ep_in, cp_tx_record, (size_t)*pending);
	if (n == *pending) {
		if (cp_needs_terminator((size_t)*pending, maxpacket) && write(cp_ep_in, "", 0) < 0 && errno != EAGAIN)
			cp_log("terminating packet refused: %s\n", strerror(errno));
		*pending = 0;
		return;
	}

	/* Not ready: the slot keeps the record and the loop comes back to it. */
	if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;

	/* Anything else is the endpoint being gone. The record is dropped rather
	 * than retried forever: the transport above this is TCP, and a resend is
	 * something it already knows how to do. */
	cp_log("transmit endpoint: %s\n", n < 0 ? strerror(errno) : "short write");
	*pending = 0;
}

//////////////////////////////////////////////////////////////////////////////
// ep0 and the slow tick
//////////////////////////////////////////////////////////////////////////////

/* ep0 is where the kernel says what happened to the cable. Two of its events
 * matter here: ENABLE, which says a host has configured the interface so the
 * endpoints may be used, and DISABLE, which says they may not. A SETUP arrives
 * for a control request the composite layer did not handle; we send none, so an
 * unexpected one is answered with a zero-length write, which stalls a
 * control-OUT request and completes a control-IN one. Leaving it alone would
 * keep the host's transfer pending until it timed out. */
static void cp_ep0_events(void) {
	if (cp_evfd >= 0) {
		uint64_t counter;
		while (read(cp_evfd, &counter, sizeof(counter)) == (ssize_t)sizeof(counter)) ;
	}

	for (int i = 0; i < 8; i++) {
		CP_FFS_Event event;
		ssize_t n = read(cp_ep0, &event, sizeof(event));
		if (n < 0) {
			if (errno != EAGAIN && errno != EINTR) cp_log("ep0: %s\n", strerror(errno));
			return;
		}
		if (n != (ssize_t)sizeof(event)) return;

		switch (event.type) {
		case CP_FFS_EV_ENABLE:
			cp_log("a host configured us\n");
			break;
		case CP_FFS_EV_DISABLE:
			cp_log("the host released us\n");
			break;
		case CP_FFS_EV_SETUP:
			cp_log("refusing control request %02x/%02x\n", event.setup.bRequestType, event.setup.bRequest);
			if (write(cp_ep0, "", 0) < 0 && errno != EAGAIN) cp_log("ep0 reply: %s\n", strerror(errno));
			break;
		default:
			break;
		}
	}
}

/* The daemon's reason to exist is the session file, and the app's handle on it
 * is the pidfile (app/broker.c:98-104 states the same rule for the broker). A
 * session that has never been seen is not a session that has ended: the link
 * has to be up before the app writes the session, so there is a window in which
 * a running daemon legitimately has nothing to point at. */
static bool cp_lifetime_ok(void) {
	if (!cp_file_exists(cp_pid_path)) return false;
	if (NS_isArmed()) { cp_session_seen = true; return true; }
	if (!cp_session_seen) return true;
	cp_log("the session is gone\n");
	return false;
}

/* The cadence that is not the data path: the peer question, the lifetime
 * question and the status file. A second is what the app needs - it reads the
 * status file itself - and asking more often would put a stat() and a
 * session-file read in the way of every burst. Called once per
 * CP_SIDE_EFFECT_MS, not once per tick.
 *
 * Both questions are the caller's, because only the role knows how to ask them.
 * "Our side is up" and "the peer is here" are different answers and only the
 * second is worth showing: a gadget can be bound and enabled with the far end
 * unplugged, and a port can be in host mode with nothing on it. The gadget role
 * asks the controller; the host role knows whether it is holding a device. The
 * receive thread belongs to the gadget role alone and is passed in the same way,
 * so this function stays one implementation of one cadence.
 *
 * A failed state is left alone - it is terminal, and this tick must not quietly
 * turn it back into "waiting". */
static bool cp_slow_tick(bool peer_present, bool link_failed) {
	if (link_failed) {
		cp_fail("the receive endpoint stopped: %s", strerror(cp_rx_errno ? cp_rx_errno : EIO));
		return false;
	}

	if (cp_status.state != CP_STATE_FAILED) {
		CP_State want = peer_present ? CP_STATE_UP : CP_STATE_WAIT;
		if (want != cp_status.state) {
			cp_status_set_state(&cp_status, want);
			cp_log("%s\n", peer_present ? "peer present - link up" : "no peer yet - waiting");
		}
	}

	if (!cp_lifetime_ok()) return false;
	cp_publish();
	return true;
}

//////////////////////////////////////////////////////////////////////////////
// bring-up, pump, teardown
//////////////////////////////////////////////////////////////////////////////

static bool cp_gadget_start(char* err, int errlen) {
	char udc[CP_NAME_MAX];

	/* The port first. A gadget bound to a port that is not presenting as a
	 * device is never enumerated, and the correction is the one port write that
	 * owes no record. */
	if (!cp_port_device(err, errlen)) return false;

	/* The interface next, deliberately. Everything after this point takes
	 * something away from the firmware - the controller, from its gadget - and a
	 * device that cannot do the cheap half should not be allowed to reach the
	 * expensive one, because adb is on the other side of it. */
	if (!cp_status_put("gadget", cp_facts.gadget) || !cp_status_put("iface", cp_facts.iface) ||
	    !cp_status_put("ip", NS_CABLE_HOST_IP) || !cp_status_put("peer", NS_CABLE_CLIENT_IP)) {
		snprintf(err, errlen, "the status grammar refused this link's own fields");
		return false;
	}

	cp_tun_fd = cp_tun_open(err, errlen);
	if (cp_tun_fd < 0) return false;
	if (!cp_iface_up(NS_CABLE_HOST_IP, err, errlen)) return false;
	cp_log("%s up as %s\n", cp_facts.iface, NS_CABLE_HOST_IP);

	cp_status_set_state(&cp_status, CP_STATE_GADGET);
	cp_publish();

	if (!cp_configfs_ensure(err, errlen)) return false;
	if (!cp_udc_name(udc, sizeof(udc))) {
		snprintf(err, errlen, "%s lists no controller", cp_facts.udc_dir);
		return false;
	}
	if (!cp_gadget_build(err, errlen)) return false;
	if (!cp_ffs_mount(err, errlen)) return false;
	if (!cp_ffs_open_ep0(err, errlen)) return false;
	if (!cp_eps_open(err, errlen)) return false;
	if (!cp_udc_take(udc, err, errlen)) return false;

	cp_status_put("udc", udc);
	cp_status_set_state(&cp_status, CP_STATE_WAIT);
	cp_publish();
	return true;
}

/* Unwinds in the order the bring-up built, and every step is safe to unwind
 * when it never happened - the daemon can be stopped half-way through its own
 * start. The controller goes back before the directories are removed, because
 * configfs refuses to remove a bound gadget; the engine's owner is put back
 * last, because until the record is honoured it is the only thing that knows
 * what to put back. */
static void cp_gadget_teardown(void) {
	if (cp_ep_in >= 0)  { close(cp_ep_in);  cp_ep_in = -1; }
	if (cp_ep_out >= 0) { close(cp_ep_out); cp_ep_out = -1; }
	if (cp_ep0 >= 0)    { close(cp_ep0);    cp_ep0 = -1; }
	if (cp_evfd >= 0)   { close(cp_evfd);   cp_evfd = -1; }
	if (cp_tun_fd >= 0) { close(cp_tun_fd); cp_tun_fd = -1; }   /* the interface goes with it */

	if (cp_gadget_bound) {
		char path[CP_LINE_MAX];
		if (cp_gadget_attr(&cp_facts, "UDC", path, sizeof(path))) {
			if (cp_write_attr(path, "\n")) cp_log("released the controller\n");
			else cp_log("could not release the controller: %s\n", strerror(errno));
		}
		cp_gadget_bound = false;
	}

	cp_gadget_remove();

	if (cp_ffs_mounted) {
		/* Detached rather than plain: a mount left by an earlier run may still
		 * have a file open in a process that is not this one, and refusing to
		 * unmount would leave the next session to inherit it. */
		if (umount2(cp_facts.ffs_dir, MNT_DETACH) != 0 && errno != EINVAL)
			cp_log("could not unmount %s: %s\n", cp_facts.ffs_dir, strerror(errno));
		cp_ffs_mounted = false;
	}

	cp_udc_restore();
}

static void cp_gadget_pump(void) {
	struct pollfd fds[3];
	struct timeval last_tick;
	pthread_t rx;
	int pending = 0;

	fds[0].fd = cp_tun_fd; fds[0].events = POLLIN; fds[0].revents = 0;
	fds[1].fd = cp_ep0;    fds[1].events = POLLIN; fds[1].revents = 0;
	fds[2].fd = cp_evfd;   fds[2].events = POLLIN; fds[2].revents = 0;

	if (pthread_create(&rx, NULL, cp_rx_worker, NULL) != 0) {
		cp_fail("cannot start the receive thread: %s", strerror(errno));
		return;
	}

	unsigned maxpacket = cp_maxpacket_for(cp_status.udc);
	cp_log("gadget bound on %s, %s up as %s, %u-byte packets\n",
	       cp_status.udc, cp_facts.iface, NS_CABLE_HOST_IP, maxpacket);
	gettimeofday(&last_tick, NULL);

	while (!cp_stopping) {
		int n = poll(fds, 3, pending ? CP_TX_RETRY_MS : CP_TICK_MS);
		if (n < 0) {
			if (errno == EINTR) continue;
			cp_fail("poll: %s", strerror(errno));
			break;
		}

		if (fds[1].revents & POLLIN || fds[2].revents & POLLIN) cp_ep0_events();
		if (!pending && (fds[0].revents & POLLIN)) pending = cp_tun_read();
		if (pending) cp_tx_flush(&pending, maxpacket);

		struct timeval now;
		gettimeofday(&now, NULL);
		if (cp_ms_between(&last_tick, &now) >= CP_SIDE_EFFECT_MS) {
			last_tick = now;
			if (!cp_slow_tick(cp_udc_configured(cp_status.udc), cp_rx_failed)) break;
		}
	}

	/* Cancelled rather than signalled: the thread is parked in a read() on a
	 * file that has no poll() to wake it, and read is a cancellation point, so
	 * this both interrupts the transfer inside the kernel and ends the thread
	 * before the descriptors underneath it are closed. Without the join the
	 * endpoints would be closed while that read is still in flight. */
	pthread_cancel(rx);
	pthread_join(rx, NULL);
	cp_log("received %d records\n", (int)cp_rx_records);
}

static void cp_gadget_run(void) {
	char err[CP_ERROR_MAX];

	if (!cp_gadget_start(err, sizeof(err))) {
		cp_fail("%s", err);
		cp_gadget_teardown();
		return;
	}
	cp_gadget_pump();
	cp_gadget_teardown();
}

// the host role
//
// The enumerating end of the cable. Three things happen in order: the port is
// switched to host (the role child above, because that write is the one that
// can wedge), the peer's gadget is found on the usbfs bus and claimed, and its
// two bulk endpoints are driven with asynchronous URBs bridged to the same TUN
// the gadget role builds.
//
// Asynchronous rather than USBDEVFS_BULK, and the reason is a property of usbfs
// rather than a preference: only one ioctl may be in flight on a device file at
// a time, so a synchronous read and a synchronous write on the same descriptor
// would serialize the two directions of a bridge whose entire job is to carry
// both at once. The cost is that every transfer needs memory that outlives the
// call, which is why the two URBs are file scope.
//
// Nothing here runs on a thread. usbfs does implement poll() - usbdev_poll
// reports POLLOUT once a completion is waiting - so unlike the gadget role's
// endpoint files, the device file can join the main loop's poll set. The set is
// named `link` rather than `fds` so that it reads separately from the gadget
// role's three-entry one; they describe different things and only one of them is
// pollable for the reason that matters.
//////////////////////////////////////////////////////////////////////////////

typedef enum {
	CP_LINK_OK = 0,   /* nothing happened that ends the attachment */
	CP_LINK_GONE,     /* the peer is no longer on the bus; keep waiting */
	CP_LINK_BROKEN,   /* something that is not our writer is on the pipe */
} CP_LinkResult;

/* One descriptor read, synchronously. usbfs's synchronous control transfer is
 * right here and nowhere else: these reads happen before anything is claimed or
 * running, and a descriptor is a few dozen bytes with no second direction to
 * interleave. */
static int cp_usb_descriptor(uint8_t type, void* out, size_t cap) {
	if (!out || !cap || cap > CP_DESC_MAX) return -1;

	CP_CtrlTransfer transfer;
	memset(&transfer, 0, sizeof(transfer));
	transfer.bRequestType = (uint8_t)(CP_USB_DIR_IN | CP_USB_RECIP_DEVICE);
	transfer.bRequest = (uint8_t)CP_USB_REQ_GET_DESCRIPTOR;
	transfer.wValue = (uint16_t)(type << 8);   /* descriptor type, index 0 */
	transfer.wLength = (uint16_t)cap;
	transfer.timeout = CP_USB_CTRL_TIMEOUT_MS;
	transfer.data = out;

	return (int)ioctl(cp_usb_fd, CP_USBDEVFS_CONTROL, &transfer);
}

/* Whether the device on this file is the peer. The vendor/product pair is the
 * only thing that says so, and it is read the same way the host controller read
 * it: out of the device descriptor. Nothing is assumed about the name the
 * controller gave the device - the name carries no identity. */
static bool cp_usb_is_peer(void) {
	uint8_t device[18];

	if (cp_usb_descriptor(CP_DT_DEVICE, device, sizeof(device)) < (int)sizeof(device)) return false;
	if (device[0] < (uint8_t)sizeof(device) || device[1] != (uint8_t)CP_DT_DEVICE) return false;

	uint16_t vendor = (uint16_t)(device[8] | ((uint16_t)device[9] << 8));
	uint16_t product = (uint16_t)(device[10] | ((uint16_t)device[11] << 8));
	return vendor == CP_VID && product == CP_PID;
}

/* The interface and the two endpoints, out of the configuration descriptor.
 *
 * The check is not decoration. The interface's class, its endpoint count and the
 * endpoints' transfer type together are what distinguish our peer from anything
 * else that may be on this bus, and the pair of ids above is unassigned
 * precisely so that a device claiming it is a device we built. A configuration
 * that does not match is left alone rather than guessed at.
 *
 * Walks by bLength, never by a fixed stride: descriptors are a byte stream whose
 * types are interleaved, and a reader that steps by the size it expected
 * desynchronizes at the first descriptor it did not know about - which is
 * exactly how the interface's own class-specific extras would arrive, if a
 * future revision ever sent any.
 *
 * The first matching interface wins; a second one is a shape this link does not
 * have. */
static bool cp_usb_parse_config(const uint8_t* config, size_t total, unsigned* interface_out,
                                uint8_t* in_out, uint8_t* out_out, unsigned* packet_out) {
	if (!config || total < 9 || config[1] != (uint8_t)CP_DT_CONFIG) return false;

	size_t at = config[0];
	while (at + 2 <= total) {
		size_t length = config[at];
		if (length < 2 || at + length > total) return false;
		if (config[at + 1] != (uint8_t)CP_DT_INTERFACE || length < 9) { at += length; continue; }

		unsigned count = 0, packet = 0;
		uint8_t in_addr = 0, out_addr = 0;
		bool bulk_only = true;

		size_t next = at + length;
		while (next + 2 <= total) {
			size_t entry = config[next];
			if (entry < 2 || next + entry > total) return false;
			if (config[next + 1] == (uint8_t)CP_DT_INTERFACE) break;

			if (config[next + 1] == (uint8_t)CP_DT_ENDPOINT && entry >= 7) {
				uint8_t address = config[next + 2];
				unsigned size = (unsigned)(config[next + 4] | ((unsigned)config[next + 5] << 8));
				size &= CP_MAXPACKET_MASK;
				if (size > CP_MAXPACKET_HS) size = CP_MAXPACKET_HS;

				if (!cp_ep_is_bulk(config[next + 3])) bulk_only = false;
				else if (cp_ep_dir(address) == CP_EP_IN)  { in_addr  = address; if (size > packet) packet = size; }
				else if (cp_ep_dir(address) == CP_EP_OUT) { out_addr = address; if (size > packet) packet = size; }
				else bulk_only = false;
				count++;
			}
			next += entry;
		}

		if (config[at + 5] == (uint8_t)CP_INTERFACE_CLASS_VENDOR && count == CP_BULK_ENDPOINTS &&
		    bulk_only && in_addr && out_addr && packet) {
			*interface_out = config[at + 2];
			*in_out = in_addr;
			*out_out = out_addr;
			*packet_out = packet;
			return true;
		}
		at = next;
	}
	return false;
}

/* The usbfs node the peer is on, or false.
 *
 * The directory is walked rather than the device numbers assumed: which bus and
 * which device number the controller handed out is not knowable before it
 * enumerates, and a stale node from an unplugged device may still be listed.
 * Only the shape of the name is checked here - the identity comes from the
 * descriptors, which is also why this opens the root hub and any other device on
 * the bus and reads them like anything else. */
static bool cp_host_scan(char* node_out, size_t cap) {
	DIR* bus = opendir(cp_usb_dir);
	if (!bus) return false;

	bool found = false;
	struct dirent* busentry;
	while (!found && (busentry = readdir(bus)) != NULL) {
		if (busentry->d_name[0] < '0' || busentry->d_name[0] > '9') continue;

		char busdir[CP_LINE_MAX];
		if (snprintf(busdir, sizeof(busdir), "%s/%s", cp_usb_dir, busentry->d_name) >= (int)sizeof(busdir)) continue;

		DIR* devices = opendir(busdir);
		if (!devices) continue;

		struct dirent* deventry;
		while (!found && (deventry = readdir(devices)) != NULL) {
			if (deventry->d_name[0] < '0' || deventry->d_name[0] > '9') continue;

			char node[CP_LINE_MAX];
			if (snprintf(node, sizeof(node), "%s/%s", busdir, deventry->d_name) >= (int)sizeof(node)) continue;

			cp_usb_fd = open(node, O_RDWR | O_NONBLOCK);
			if (cp_usb_fd < 0) continue;

			int n = 0;
			if (cp_usb_is_peer()) n = snprintf(node_out, cap, "%s", node);
			close(cp_usb_fd);
			cp_usb_fd = -1;
			if (n > 0 && (size_t)n < cap) found = true;
		}
		closedir(devices);
	}
	closedir(bus);
	return found;
}

/* Claim the interface and learn the two endpoints.
 *
 * The claim is explicit even though usbfs claims an interface on first use: this
 * interface is the peer's, it has no driver, and saying so is cheaper to read
 * than the warning usbfs logs when something is used without being claimed. A
 * refused claim is a real answer - another process already has the device - and
 * is reported as one.
 *
 * Failure leaves the device file open on purpose: the caller's detach closes it,
 * so there is exactly one place that knows how to undo an attachment. */
static bool cp_host_open(const char* node, char* err, int errlen) {
	uint8_t config[CP_DESC_MAX];

	cp_usb_fd = open(node, O_RDWR | O_NONBLOCK);
	if (cp_usb_fd < 0) {
		snprintf(err, errlen, "cannot open %s: %s", node, strerror(errno));
		return false;
	}

	int have = cp_usb_descriptor(CP_DT_CONFIG, config, 9);
	if (have < 9) {
		snprintf(err, errlen, "cannot read the configuration header: %s", strerror(errno));
		return false;
	}

	/* The header only, first: wTotalLength is what says how much there is to
	 * read, and it is not known before the header arrives. A device that claims
	 * more than this link could ever be told to do is refused rather than
	 * allocated for. */
	size_t total = (size_t)(config[2] | ((size_t)config[3] << 8));
	if (total < 9 || total > sizeof(config)) {
		snprintf(err, errlen, "the peer's configuration is %d bytes", (int)total);
		return false;
	}

	have = cp_usb_descriptor(CP_DT_CONFIG, config, total);
	if (have < (int)total) {
		/* A short read here is not a smaller configuration: the walk below
		 * would run past the end of what arrived. */
		snprintf(err, errlen, "the configuration came back short: %d of %d bytes", have, (int)total);
		return false;
	}

	unsigned interface = 0;
	if (!cp_usb_parse_config(config, total, &interface,
	                         &cp_usb_in_addr, &cp_usb_out_addr, &cp_usb_maxpacket)) {
		snprintf(err, errlen, "the peer is not presenting a cable link");
		return false;
	}

	unsigned int number = interface;
	if (ioctl(cp_usb_fd, CP_USBDEVFS_CLAIMINTERFACE, &number) < 0) {
		snprintf(err, errlen, "cannot claim interface %u: %s", interface, strerror(errno));
		return false;
	}

	cp_log("%s: interface %u claimed, %02x in / %02x out, %u-byte packets\n",
	       node, interface, (unsigned)cp_usb_in_addr, (unsigned)cp_usb_out_addr, cp_usb_maxpacket);
	return true;
}

/* Closing is the whole teardown: usbfs releases the claim and kills every URB
 * still in flight when the device file is released, so neither is undone by hand
 * here - doing so would be a second implementation of a rule the kernel already
 * applies. */
static void cp_host_detach(void) {
	if (cp_usb_fd >= 0) close(cp_usb_fd);
	cp_usb_fd = -1;
	cp_urb_out_in_flight = false;
	cp_usb_in_addr = 0;
	cp_usb_out_addr = 0;
	cp_usb_maxpacket = CP_MAXPACKET_HS;
}

/* One receive URB, always in flight while a peer is attached.
 *
 * No USBDEVFS_URB_SHORT_NOT_OK, and that is the point: a short transfer is not an
 * error on this link, it is the only thing that ends a record. The writer's
 * two-byte length prefix exists because a bulk transfer carries no length of its
 * own, and asking the controller to treat a short packet as a failure would
 * reject every record whose length is not a multiple of the packet size - which
 * is most of them. */
static bool cp_urb_in_submit(void) {
	memset(&cp_urb_in, 0, sizeof(cp_urb_in));
	cp_urb_in.type = (uint8_t)CP_URB_TYPE_BULK;
	cp_urb_in.endpoint = cp_usb_in_addr;
	cp_urb_in.buffer = cp_urb_in_buffer;
	cp_urb_in.buffer_length = (int32_t)sizeof(cp_urb_in_buffer);

	if (ioctl(cp_usb_fd, CP_USBDEVFS_SUBMITURB, &cp_urb_in) < 0) return false;
	return true;
}

/* One transmit URB for one record.
 *
 * The zero-length packet the kernel appends when the flag is set is the host-side
 * half of the rule the gadget role applies by writing an empty transfer: 4.9's
 * f_fs appends nothing for us, and a record whose length is an exact multiple of
 * the packet size has no short packet of its own to end it. The question is the
 * pure layer's, so both ends answer it the same way. */
static bool cp_urb_out_submit(int length) {
	memset(&cp_urb_out, 0, sizeof(cp_urb_out));
	cp_urb_out.type = (uint8_t)CP_URB_TYPE_BULK;
	cp_urb_out.endpoint = cp_usb_out_addr;
	cp_urb_out.buffer = cp_tx_record;
	cp_urb_out.buffer_length = (int32_t)length;
	if (cp_needs_terminator((size_t)length, cp_usb_maxpacket)) cp_urb_out.flags |= CP_URB_ZERO_PACKET;

	if (ioctl(cp_usb_fd, CP_USBDEVFS_SUBMITURB, &cp_urb_out) < 0) return false;
	cp_urb_out_in_flight = true;
	return true;
}

/* A completed receive transfer.
 *
 * Two lengths are legal and mean nothing to forward: the record itself, one
 * transfer long, and the empty transfer that ends an aligned one. Anything else
 * means the bytes coming back are not our records - the same conclusion the
 * gadget role's receive thread draws, and for the same reason: the writer puts
 * one whole record in one transfer, so a transfer that is not a whole record is a
 * stream that has already lost its framing, and forwarding a guess from it would
 * be worse than stopping. */
static CP_LinkResult cp_host_deliver(size_t length) {
	const uint8_t* packet = NULL;

	if (length > sizeof(cp_urb_in_buffer)) return CP_LINK_BROKEN;

	int len = cp_frame_decode(cp_urb_in_buffer, length, &packet);
	if (len < 0) {
		cp_log("a %d-byte transfer is not a record\n", (int)length);
		return CP_LINK_BROKEN;
	}
	if (len == 0) return CP_LINK_OK;

	if (write(cp_tun_fd, packet, (size_t)len) < 0 && errno != EAGAIN) {
		cp_log("cannot hand a packet to %s: %s\n", cp_facts.iface, strerror(errno));
		return CP_LINK_BROKEN;
	}
	return CP_LINK_OK;
}

/* Everything the controller has finished, and the receive URB back in the kernel.
 *
 * The kernel hands the same struct back that it was given, with status and
 * actual_length filled in - which is why the URBs are file scope rather than
 * automatic: the struct *is* the kernel's handle on the transfer. */
static CP_LinkResult cp_host_reap(void) {
	for (int i = 0; i < CP_REAP_MAX; i++) {
		CP_Urb* done = NULL;

		/* The non-blocking reap, not USBDEVFS_REAPURB: this runs because poll()
		 * said a completion was waiting, and the blocking form would be a
		 * second place to wait that no deadline covers. */
		if (ioctl(cp_usb_fd, CP_USBDEVFS_REAPURBNDELAY, &done) < 0) return CP_LINK_OK;

		if (done == &cp_urb_in) {
			if (cp_urb_in.status != 0) {
				int status = cp_urb_in.status;
				cp_log("receive urb: %s\n", strerror(-status));

				/* The peer being gone is the ordinary end of a session rather
				 * than an error: the cable was pulled, or its daemon stopped. */
				if (status == -ENODEV || status == -ESHUTDOWN || status == -ECONNRESET) return CP_LINK_GONE;

				unsigned int address = cp_usb_in_addr;
				if (ioctl(cp_usb_fd, CP_USBDEVFS_CLEAR_HALT, &address) < 0)
					cp_log("cannot clear the receive endpoint: %s\n", strerror(errno));
				/* An endpoint that keeps failing fails instantly, so the retry
				 * is paced: an unpaced one would be a busy loop. */
				usleep(CP_TICK_MS * 1000);
			} else if (cp_urb_in.actual_length > 0) {
				CP_LinkResult result = cp_host_deliver((size_t)cp_urb_in.actual_length);
				if (result != CP_LINK_OK) return result;
			}

			if (!cp_urb_in_submit()) {
				cp_log("cannot keep receiving: %s\n", strerror(errno));
				return CP_LINK_GONE;
			}
		} else if (done == &cp_urb_out) {
			cp_urb_out_in_flight = false;
			/* A refused record is dropped rather than retried: the transport
			 * above this is TCP, and a resend is something it already knows how
			 * to do. */
			if (cp_urb_out.status != 0) cp_log("transmit urb: %s\n", strerror(-cp_urb_out.status));
		}
	}
	return CP_LINK_OK;
}

/* One attempt at becoming the peer's host.
 *
 * Every failure drops what it built, because the next attempt starts from
 * nothing: the bus is re-scanned, not resumed. "No peer on the bus" is not an
 * error and leaves err empty - it is the ordinary state of a cable with nothing
 * plugged into it. */
static bool cp_host_attach(char* err, int errlen) {
	char node[CP_LINE_MAX];

	if (errlen) err[0] = '\0';
	if (!cp_host_scan(node, sizeof(node))) return false;

	if (!cp_host_open(node, err, errlen)) {
		cp_host_detach();
		return false;
	}
	if (!cp_urb_in_submit()) {
		snprintf(err, errlen, "cannot start receiving: %s", strerror(errno));
		cp_host_detach();
		return false;
	}
	return true;
}

static void cp_host_pump(void) {
	struct pollfd link[2];
	struct timeval last_tick;
	int attach_ticks = 0;

	gettimeofday(&last_tick, NULL);

	while (!cp_stopping) {
		link[0].fd = cp_tun_fd; link[0].events = POLLIN;  link[0].revents = 0;
		link[1].fd = cp_usb_fd; link[1].events = POLLOUT; link[1].revents = 0;

		int n = poll(link, 2, CP_TICK_MS);
		if (n < 0) {
			if (errno == EINTR) continue;
			cp_fail("poll: %s", strerror(errno));
			break;
		}

		if (cp_usb_fd >= 0) {
			/* A device file is hung up by usbfs when the device goes away,
			 * which is the fastest notice of an unplug there is. */
			if (link[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				cp_log("the peer left the bus\n");
				cp_host_detach();
				cp_status_set_state(&cp_status, CP_STATE_WAIT);
				cp_publish();
			} else if (link[1].revents & POLLOUT) {
				CP_LinkResult result = cp_host_reap();

				if (result == CP_LINK_BROKEN) {
					cp_fail("the cable link lost its framing");
					break;
				}
				if (result == CP_LINK_GONE) {
					cp_log("the peer is gone; waiting for it to come back\n");
					cp_host_detach();
					cp_status_set_state(&cp_status, CP_STATE_WAIT);
					cp_publish();
				}
			}
		}

		/* A packet is read from the interface only when the endpoint has room
		 * for it: one record in flight at a time, which is the backpressure - an
		 * endpoint that is not draining stops the interface being read rather
		 * than filling a queue that can overflow. */
		if (!cp_urb_out_in_flight && cp_usb_fd >= 0 && (link[0].revents & POLLIN)) {
			int record = cp_tun_read();
			if (record > 0 && !cp_urb_out_submit(record)) cp_log("cannot send: %s\n", strerror(errno));
		}

		if (cp_usb_fd < 0 && cp_status.state != CP_STATE_FAILED && ++attach_ticks >= CP_ATTACH_TICKS) {
			char err[CP_ERROR_MAX];

			attach_ticks = 0;
			if (cp_host_attach(err, sizeof(err))) {
				cp_status_set_state(&cp_status, CP_STATE_UP);
				cp_publish();
				cp_log("peer found; the link is up\n");
			} else if (err[0]) {
				/* A device that answers to our ids and then refuses to be
				 * opened is worth a line: it is not the ordinary "no cable
				 * yet", and it is the shape a half-built peer takes. */
				cp_log("%s\n", err);
			}
		}

		struct timeval now;
		gettimeofday(&now, NULL);
		if (cp_ms_between(&last_tick, &now) >= CP_SIDE_EFFECT_MS) {
			last_tick = now;
			if (!cp_slow_tick(cp_usb_fd >= 0, false)) break;
		}
	}
}

/* The port first, then the interface, then the peer. Same order as the gadget
 * role's, and for the same reason: the step that touches the platform comes
 * before the step that expects something of it. */
static bool cp_host_start(char* err, int errlen) {
	cp_status_set_state(&cp_status, CP_STATE_ROLE);
	cp_publish();
	if (!cp_role_force(err, errlen)) return false;

	/* Both addresses are fixed and known here, unlike the gadget role's peer,
	 * which is the same the other way round: the side that enumerates is the
	 * side that has to invent nothing. */
	if (!cp_status_put("iface", cp_facts.iface) || !cp_status_put("ip", NS_CABLE_CLIENT_IP) ||
	    !cp_status_put("peer", NS_CABLE_HOST_IP)) {
		snprintf(err, errlen, "the status grammar refused this link's own fields");
		return false;
	}

	cp_status_set_state(&cp_status, CP_STATE_GADGET);
	cp_publish();

	cp_tun_fd = cp_tun_open(err, errlen);
	if (cp_tun_fd < 0) return false;
	if (!cp_iface_up(NS_CABLE_CLIENT_IP, err, errlen)) return false;
	cp_log("%s up as %s\n", cp_facts.iface, NS_CABLE_CLIENT_IP);

	cp_status_set_state(&cp_status, CP_STATE_WAIT);
	cp_publish();
	return true;
}

static void cp_host_teardown(void) {
	cp_host_detach();
	if (cp_tun_fd >= 0) { close(cp_tun_fd); cp_tun_fd = -1; }   /* the interface goes with it */

	/* The port is put back last, after the interface that was using it is gone
	 * - the record is the last thing honoured, exactly as the controller's is on
	 * the gadget side. */
	cp_role_restore();
}

static void cp_host_run(void) {
	char err[CP_ERROR_MAX];

	if (!cp_host_start(err, sizeof(err))) {
		cp_fail("%s", err);
		cp_host_teardown();
		return;
	}
	cp_host_pump();
	cp_host_teardown();
}

//////////////////////////////////////////////////////////////////////////////
// entry point
//////////////////////////////////////////////////////////////////////////////

static void cp_usage(FILE* out) {
	fprintf(out, "usage: netplay-cable --role gadget|host\n");
}

int main(int argc, char** argv) {
	CP_Role role = CP_ROLE_GADGET;
	bool role_given = false;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--role") && i + 1 < argc) {
			if (!cp_role_from_name(argv[++i], &role)) {
				fprintf(stderr, "netplay-cable: unknown role '%s'\n", argv[i]);
				return 2;
			}
			role_given = true;
		} else if (!strcmp(argv[i], "--help")) {
			cp_usage(stdout);
			return 0;
		} else {
			fprintf(stderr, "netplay-cable: unexpected argument '%s'\n", argv[i]);
			cp_usage(stderr);
			return 2;
		}
	}
	if (!role_given) {
		cp_usage(stderr);
		return 2;
	}
	/* The role comes from the command line, not from the session file: on both
	 * ends the link has to be up *before* the app arms the session, which is the
	 * same ordering the WiFi paths use. Both roles are served here, and which
	 * one runs is the only difference between them. */

	NS_init();
	cp_facts_init();

	if (!cp_paths_init()) {
		fprintf(stderr, "netplay-cable: the state directory is unusable\n");
		return 1;
	}

	/* Own our output before saying anything: the file is opened for append
	 * before this process exists, so rotating it is only correct here. */
	cp_log_open();
	cp_log("starting as %s (pid %d)\n", cp_role_name(role), (int)getpid());

	if (!cp_pid_write()) {
		cp_log("another cable daemon owns the session: %s\n", strerror(errno));
		return 1;
	}

	signal(SIGINT, cp_stop_signal);
	signal(SIGTERM, cp_stop_signal);
	signal(SIGHUP, cp_stop_signal);

	cp_status_init(&cp_status, role);
	cp_publish();

	if (role == CP_ROLE_GADGET) cp_gadget_run();
	else cp_host_run();

	/* Both artifacts go on every path out, including the one where bring-up
	 * failed: a status file left behind is a session the next app believes is
	 * already running. */
	remove(cp_status_path);
	remove(cp_pid_path);
	cp_log("stopped\n");
	return 0;
}
