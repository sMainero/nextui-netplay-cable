/*
 * USB cable link - the vocabulary the daemon, its pure layer and the setup app
 * share.
 *
 * Deliberately standalone: it includes nothing from this tree and calls
 * nothing, because the pure half of the cable work is built and tested on a
 * developer's machine, where neither NextUI nor a device exists. Anything that
 * reached for netsetup.h would take those tests with it.
 *
 * Three kinds of fact live here, and all three are things two processes have to
 * agree about rather than decide separately:
 *
 *   identity  the gadget's vendor/product pair, its class and its endpoint
 *             shape - written by the gadget role, checked by the host role.
 *   framing   the record format on the bulk pipe, and the rule that ends one.
 *   status    the grammar of cable.status, which is the setup app's only view
 *             of a daemon it does not own.
 */
#ifndef USBNET_H
#define USBNET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- gadget identity ----------------------------------------------------
 *
 * A local, unassigned pair, used only between two of these devices on a
 * private cable. Unassigned is the point: no kernel driver claims it, so the
 * host side reaches the interface through usbfs without detaching anything, and
 * it cannot collide with the firmware's own gadget - which holds the UDC when
 * we are not bound.
 */
#define CP_VID        0x4e50
#define CP_PID        0x0001
#define CP_BCD_DEVICE 0x0100

/* One interface, two bulk endpoints: the shape the firmware's adbd already runs
 * on this device, and the shape the vendor UDC's fixed endpoint table maps. */
#define CP_INTERFACE_CLASS_VENDOR 0xffu
#define CP_BULK_ENDPOINTS        2
#define CP_EP_ADDR_IN           0x81u   /* bulk IN:  device -> host */
#define CP_EP_ADDR_OUT          0x02u   /* bulk OUT: host -> device */
#define CP_MAXPACKET_FS          64u
#define CP_MAXPACKET_HS          512u

/* The endpoint files functionfs creates, in descriptor order. We do not set
 * FUNCTIONFS_VIRTUAL_ADDR, so the kernel autoconfigures the real addresses and
 * names the files ep1..epN by descriptor index - the order in the blob below is
 * therefore the only thing that fixes which file is which direction. */
#define CP_EP_FILE_IN  "ep1"
#define CP_EP_FILE_OUT "ep2"

/* One interface string, requested by the descriptor blob and provided by the
 * strings blob. The kernel refuses the pair if only one of the two is there. */
#define CP_IFACE_STRING 1
#define CP_STRING_TEXT  "netplay cable"

/* --- functionfs blob layout ---------------------------------------------
 *
 * The numbers below are transcribed from the 4.9 uapi header rather than
 * included from it, because the pure layer is compiled where no kernel header
 * exists. The last field of the descriptor header is why this blob is built by
 * a function under test instead of being written at the mount site: with
 * FUNCTIONFS_EVENTFD set, f_fs.c reads a 4-byte eventfd *between* the flags and
 * the descriptor counts, which is not where the field table in the kernel's own
 * documentation puts it.
 *
 *   off  field     type  value
 *     0  magic     LE32  CP_FFS_DESCS_MAGIC_V2
 *     4  length    LE32  the whole blob - the kernel checks it against the
 *                         write's byte count, so the two cannot drift
 *     8  flags     LE32  HAS_FS_DESC | HAS_HS_DESC | EVENTFD
 *    12  eventfd   LE32  the daemon's eventfd, as a number
 *    16  fs_count  LE32  CP_DESC_COUNT
 *    20  hs_count  LE32  CP_DESC_COUNT
 *    24  fs descs        interface, bulk IN, bulk OUT at 64-byte packets
 *    47  hs descs        the same three at 512-byte packets
 *
 * fs_count and hs_count must describe the same interface and endpoint count,
 * and that endpoint count must be the interface descriptor's bNumEndpoints.
 */
#define CP_FFS_DESCS_MAGIC_V2 3u
#define CP_FFS_STRINGS_MAGIC  2u
#define CP_FFS_HAS_FS_DESC    1u
#define CP_FFS_HAS_HS_DESC    2u
#define CP_FFS_EVENTFD        32u

#define CP_IFACE_DESC_LEN  9
#define CP_EP_DESC_LEN     7
#define CP_DESC_COUNT      (1 + CP_BULK_ENDPOINTS)
#define CP_SPEED_DESCS_LEN (CP_IFACE_DESC_LEN + CP_BULK_ENDPOINTS * CP_EP_DESC_LEN)
#define CP_DESCS_HEADER    24
#define CP_DESCS_MAX       (CP_DESCS_HEADER + 2 * CP_SPEED_DESCS_LEN)

/* The largest descriptor read the host role asks for. The peer's configuration
 * descriptor is 9 + 2*(9+7) = 41 bytes, so this is generous headroom; the cap
 * exists so cp_usb_descriptor refuses a request it cannot fill rather than
 * truncating one. Referenced from Phase 4, which does not define it. */
#define CP_DESC_MAX 256

/* Strings blob: magic, length, str_count, lang_count, then for each language a
 * 16-bit language id followed by str_count NUL-terminated UTF-8 strings.
 *
 * In 4.9 this write is not optional and not skippable: ep0 only reaches the
 * state in which the endpoint files appear - the state a gadget has to be in
 * before it can bind - after it, and every write to ep0 is refused below 16
 * bytes, so a header alone is not a blob either. */
#define CP_STRINGS_HEADER 16
#define CP_STRINGS_MAX    (CP_STRINGS_HEADER + 2 + sizeof(CP_STRING_TEXT))
#define CP_LANG_EN_US     0x0409u

/* --- the link ----------------------------------------------------------- */

/* Layer 3 with no packet-information header, so a read gives the IP packet
 * itself and nothing else has to be stripped. */
#define CP_IFACE_NAME   "npc0"
#define CP_MTU          1500
#define CP_FRAME_HEADER 2
#define CP_RECORD_MAX   (CP_FRAME_HEADER + CP_MTU)

/* Read buffers are whole multiples of the largest max packet size. f_fs sizes a
 * read request up to a maxpacket multiple and drops anything the controller
 * delivers beyond that, so a buffer which is not a multiple can lose the tail
 * of the very packet that filled it. 2048 is 4 x 512 and 32 x 64. */
#define CP_READ_MAX 2048

/* --- shared state -------------------------------------------------------
 *
 * cable.pid, cable.status and cable.log live in the pak's state directory
 * beside the broker's, under the same rules: the pid file is the
 * single-instance lock, the status file is published by rename so a reader
 * never sees a half-written record, and the log rotates one generation by size
 * under its own override.
 *
 * usb_restore is different in kind and is deliberately not session state. It is
 * the record of a repair this device owes the firmware - the UDC's previous
 * owner - kept until that repair succeeds, exactly as wifi_restore is kept.
 */
#define CP_PID_FILE     "cable.pid"
#define CP_STATUS_FILE  "cable.status"
#define CP_LOG_FILE     "cable.log"
#define CP_RESTORE_FILE "usb_restore"
#define CP_LOG_TAG      "[netplay-cable] "
#define CP_LOG_MAX_ENV  "NETPLAY_CABLE_LOG_MAX"

/* --- per-platform facts -------------------------------------------------
 *
 * Which files a platform drives the gadget through. The table itself lives in
 * usbnet.c under the PLATFORM macro the makefile already defines, because this
 * is a fact only the daemon needs and the daemon is cross-built once per
 * platform. It is a struct rather than a set of #ifdefs so that the path
 * construction below can be tested against a table that is not the device's.
 *
 * role_dir is NULL on a platform with no OTG role nodes: the link then depends
 * on the port's own negotiation, and "no cable peer" is reported without ever
 * having written anything.
 */
typedef struct {
	const char* configfs;   /* where configfs is mounted */
	const char* ffs_dir;    /* where we mount our functionfs instance */
	const char* udc_dir;    /* the controller list */
	const char* tun_dev;    /* the tun clone device */
	const char* role_dir;   /* vendor OTG role nodes, or NULL */
	const char* top_hcd;    /* controller behind a host-only socket, or NULL when
	                         * the device has one shared USB port */
	const char* gadget;     /* the gadget we ride: discovered at run time, never created - the vendor kernel allows exactly one */
	const char* function;   /* the functionfs instance name: ffs.<function> */
	const char* iface;      /* the point-to-point interface we bring up */
} CP_Facts;

/* --- declarations ------------------------------------------------------- */

/* Descriptor blob for ep0. `eventfd` is the file descriptor the daemon created;
 * the kernel takes it from the blob, so it cannot be set afterwards. Returns
 * the byte count written, or 0 when the buffer is too small or the blob cannot
 * be built - a caller must not write a short blob, because the kernel checks
 * the embedded length against the write. */
int cp_build_descriptors(uint8_t* out, size_t cap, int eventfd);

/* Strings blob for ep0, written in a second call after the descriptors. */
int cp_build_strings(uint8_t* out, size_t cap);

/* One record: a big-endian 16-bit length, then the packet.
 *
 * A bulk transfer is delimited by a short packet, so a record whose length is
 * an exact multiple of the endpoint's packet size has no short packet to end it
 * and needs one written explicitly - a zero-length write on the gadget side,
 * URB_ZERO_PACKET on the host side. 4.9's f_fs appends nothing for us. The
 * question is asked here, once, so both roles answer it the same way; the
 * length is the record's length *after* framing, which is why the header is
 * part of what is counted. */
bool cp_needs_terminator(size_t record_len, unsigned maxpacket);

/* Frames a packet. Returns the record length, or 0 when the packet is longer
 * than the link carries or the buffer cannot hold the record. */
int cp_frame_encode(uint8_t* out, size_t cap, const uint8_t* packet, size_t len);

/* Unframes one record. Returns the packet length - which may be 0, an empty
 * record being a legal thing to have received - or -1 when there is no whole
 * record in `have` bytes. *packet, when given, points into `in`. */
int cp_frame_decode(const uint8_t* in, size_t have, const uint8_t** packet);

/* Paths. Each returns the length written, or 0 when the result would not fit.
 * A silently truncated sysfs path opens a different file, and that failure
 * surfaces as "the attribute does not exist" - so these refuse instead. */
int cp_gadget_dir(const CP_Facts* f, char* out, size_t cap);
int cp_gadget_attr(const CP_Facts* f, const char* attr, char* out, size_t cap);
int cp_gadget_function_dir(const CP_Facts* f, char* out, size_t cap);
int cp_ffs_ep_file(const CP_Facts* f, const char* ep, char* out, size_t cap);
int cp_role_node(const CP_Facts* f, const char* node, char* out, size_t cap);

/* The node whose read puts the vendor OTG port in `mode` - the trigger for a
 * mode, as cp_role_node is the path for a node. See cableproto.c for why the
 * unset mode's node is spelled differently from the mode itself. */
int cp_role_trigger_name(const char* mode, char* out, size_t cap);

/* The token a functionfs mount must claim as its source. configfs names the
 * function directory ffs.<function> and the mount has to name the instance
 * without the prefix; getting that wrong is an ENOENT that reads like "the
 * filesystem is missing", so both come from one fact. */
int cp_ffs_source(const CP_Facts* f, char* out, size_t cap);

/* Direction and transfer type of an endpoint descriptor, read the one way.
 * Both roles interpret these bytes - the gadget role to learn which file the
 * kernel gave it, the host role to decide whether the device in front of it is
 * the peer it is looking for. */
typedef enum {
	CP_EP_UNKNOWN = 0,
	CP_EP_IN,
	CP_EP_OUT,
} CP_EndpointDir;

CP_EndpointDir cp_ep_dir(uint8_t bEndpointAddress);
bool           cp_ep_is_bulk(uint8_t bmAttributes);

/* A small file's contents as a value: leading and trailing blanks removed,
 * including the newline a sysfs attribute is written with. Returns false when
 * nothing is left - an unbound UDC attribute, for instance. Rewrites the buffer
 * in place and never grows it. */
bool cp_attr_trim(char* text);

/* --- status file --------------------------------------------------------
 *
 * cable.status is the app's only view of a daemon it does not own and cannot
 * call, so its grammar is a contract between two processes rather than an
 * implementation detail. One key=value per line, state and role always present,
 * the rest omitted while empty, and no escaping anywhere: a value that carries
 * a line break would forge a line, so such a value is refused instead.
 *
 *   state   idle | role | gadget | wait | up | failed
 *   role    gadget | host
 *   gadget  the gadget directory we created
 *   udc     the controller we bound it to
 *   iface   the point-to-point interface
 *   ip      our address on it
 *   peer    the far end's address
 *   error   what went wrong, when state is failed
 */
typedef enum {
	CP_ROLE_GADGET = 0,   /* this device is the USB device the other one enumerates */
	CP_ROLE_HOST,         /* this device is the USB host that enumerates the other */
} CP_Role;

/* The daemon's lifecycle, as the app can see it. Ordered so that the states a
 * reader treats as "not yet" all precede "up". */
typedef enum {
	CP_STATE_IDLE = 0,   /* running, nothing attempted */
	CP_STATE_ROLE,       /* settling which end of the cable is the host */
	CP_STATE_GADGET,     /* bringing the gadget or the interface up */
	CP_STATE_WAIT,       /* our side is up, no peer has appeared */
	CP_STATE_UP,         /* the peer is there and records are moving */
	CP_STATE_FAILED,     /* terminal; error= says what */
} CP_State;

#define CP_NAME_MAX   40    /* configfs's own MAX_INST_NAME_LEN */
#define CP_IFACE_MAX  16
#define CP_ADDR_MAX   24    /* NS_IP_LEN */
#define CP_ERROR_MAX  128
#define CP_LINE_MAX   256
#define CP_STATUS_MAX 768   /* every field at its longest, plus keys */

typedef struct {
	CP_State state;
	CP_Role  role;
	char     gadget[CP_NAME_MAX];
	char     udc[CP_NAME_MAX];
	char     iface[CP_IFACE_MAX];
	char     ip[CP_ADDR_MAX];
	char     peer[CP_ADDR_MAX];
	char     error[CP_ERROR_MAX];
} CP_Status;

const char* cp_state_name(CP_State state);
const char* cp_role_name(CP_Role role);
bool        cp_state_from_name(const char* name, CP_State* out);
bool        cp_role_from_name(const char* name, CP_Role* out);

/* A value that can go on a line of one of our key=value files: printable, and
 * without a line break that would forge a line. */
bool cp_value_ok(const char* value);

void cp_status_init(CP_Status* s, CP_Role role);

/* Sets one field by name. Refuses an unknown key rather than ignoring it: this
 * writer belongs to the daemon, so a key that does not exist is a typo in the
 * daemon, and a field nobody reads is worse than a loud failure. Readers here
 * ignore unknown keys on purpose - that asymmetry is the point. Returns false
 * on an unknown key or a value that cannot be written, and leaves the struct
 * untouched in both cases. */
bool cp_status_set(CP_Status* s, const char* key, const char* value);
bool cp_status_set_state(CP_Status* s, CP_State state);

/* Serialises, or returns 0 when the buffer cannot hold the record. */
int cp_status_write(const CP_Status* s, char* out, size_t cap);

/* Parses. False when the body is empty, when either of state or role is absent
 * or unreadable, or when a value could not have been written by
 * cp_status_write - so a body that was cut short is never mistaken for a daemon
 * that is merely idle. The last occurrence of a repeated key wins, which is
 * what NS_sessionInfo does with the session file: one rule for a key=value file
 * in this tree, so a duplicated line cannot mean two things depending on which
 * parser reads it. */
bool cp_status_parse(const char* text, CP_Status* out);

/* True when the peer is there and the link is carrying records.
 *
 * Stated once, here, because it is the app's dynamic question and the answer
 * has to be the same wherever it is asked: waiting is not it. A gadget can be
 * bound and enabled with the far end unplugged, and a port can be in host mode
 * with no device on it, so "our side is up" is not "the peer is here". */
bool cp_status_link_ready(const CP_Status* s);

#endif
