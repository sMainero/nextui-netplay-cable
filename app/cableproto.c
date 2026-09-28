/*
 * The cable link's pure layer: the bytes functionfs is handed, the shape of a
 * record on the bulk pipe, the paths configfs is driven through, and the
 * grammar of the status file the setup app reads.
 *
 * Pure on purpose. Every function here is a calculation over its arguments: no
 * file is opened, no clock is read, nothing outside the arguments is touched.
 * That is what makes the whole of this file provable on a developer's machine -
 * where there is no /dev/net/tun, no usbfs and no cross compiler - and leaves
 * the parts that need a device (the configfs sequence, the URB loop, the port
 * role dance) with nothing untested in them but the kernel's answers.
 *
 * No kernel headers: the numbers below are transcribed from the 4.9 uapi header
 * so this compiles where that header does not exist. The unit test pins the
 * resulting bytes exactly, which is the check the include would have given.
 */
#include "usbnet.h"

#include <stdio.h>
#include <string.h>

//////////////////////////////////////////////////////////////////////////////
// little-endian fields
//
// The device is little-endian and so is every host that could ever be at the
// other end of this cable, but the fields are written byte by byte anyway: the
// blob is compared byte for byte by the test, and a struct laid over a buffer
// would make that comparison depend on the compiler's padding rather than on
// the wire format.
//////////////////////////////////////////////////////////////////////////////

static void cp_put_le16(uint8_t* p, uint16_t v) {
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
}

static void cp_put_le32(uint8_t* p, uint32_t v) {
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)((v >> 24) & 0xff);
}

//////////////////////////////////////////////////////////////////////////////
// descriptors and strings
//////////////////////////////////////////////////////////////////////////////

int cp_build_descriptors(uint8_t* out, size_t cap, int eventfd) {
	/* Written out rather than assembled from structs, because these bytes are
	 * the contract with the kernel's descriptor validator: a reader can check
	 * them against the field table in usbnet.h line by line, and the two
	 * endpoint entries follow the interface in the order that names ep1 and
	 * ep2. */
	static const uint8_t fs[CP_SPEED_DESCS_LEN] = {
		0x09, 0x04, 0x00, 0x00, (uint8_t)CP_BULK_ENDPOINTS,
		(uint8_t)CP_INTERFACE_CLASS_VENDOR, 0x00, 0x00, (uint8_t)CP_IFACE_STRING,
		0x07, 0x05, (uint8_t)CP_EP_ADDR_IN, 0x02,
		(uint8_t)(CP_MAXPACKET_FS & 0xff), (uint8_t)(CP_MAXPACKET_FS >> 8), 0x00,
		0x07, 0x05, (uint8_t)CP_EP_ADDR_OUT, 0x02,
		(uint8_t)(CP_MAXPACKET_FS & 0xff), (uint8_t)(CP_MAXPACKET_FS >> 8), 0x00,
	};
	static const uint8_t hs[CP_SPEED_DESCS_LEN] = {
		0x09, 0x04, 0x00, 0x00, (uint8_t)CP_BULK_ENDPOINTS,
		(uint8_t)CP_INTERFACE_CLASS_VENDOR, 0x00, 0x00, (uint8_t)CP_IFACE_STRING,
		0x07, 0x05, (uint8_t)CP_EP_ADDR_IN, 0x02,
		(uint8_t)(CP_MAXPACKET_HS & 0xff), (uint8_t)(CP_MAXPACKET_HS >> 8), 0x00,
		0x07, 0x05, (uint8_t)CP_EP_ADDR_OUT, 0x02,
		(uint8_t)(CP_MAXPACKET_HS & 0xff), (uint8_t)(CP_MAXPACKET_HS >> 8), 0x00,
	};

	if (!out || cap < CP_DESCS_MAX || eventfd < 0) return 0;

	cp_put_le32(out + 0,  CP_FFS_DESCS_MAGIC_V2);
	cp_put_le32(out + 4,  (uint32_t)CP_DESCS_MAX);
	cp_put_le32(out + 8,  CP_FFS_HAS_FS_DESC | CP_FFS_HAS_HS_DESC | CP_FFS_EVENTFD);
	cp_put_le32(out + 12, (uint32_t)eventfd);
	cp_put_le32(out + 16, (uint32_t)CP_DESC_COUNT);
	cp_put_le32(out + 20, (uint32_t)CP_DESC_COUNT);
	memcpy(out + CP_DESCS_HEADER, fs, sizeof(fs));
	memcpy(out + CP_DESCS_HEADER + sizeof(fs), hs, sizeof(hs));
	return (int)CP_DESCS_MAX;
}

int cp_build_strings(uint8_t* out, size_t cap) {
	static const char text[] = CP_STRING_TEXT;

	if (!out || cap < CP_STRINGS_MAX) return 0;

	cp_put_le32(out + 0,  CP_FFS_STRINGS_MAGIC);
	cp_put_le32(out + 4,  (uint32_t)CP_STRINGS_MAX);
	cp_put_le32(out + 8,  1);   /* one string per language */
	cp_put_le32(out + 12, 1);   /* one language */
	cp_put_le16(out + CP_STRINGS_HEADER, CP_LANG_EN_US);
	memcpy(out + CP_STRINGS_HEADER + 2, text, sizeof(text));
	return (int)CP_STRINGS_MAX;
}

//////////////////////////////////////////////////////////////////////////////
// framing
//
// One record is a big-endian 16-bit length followed by one IP packet. The
// length is not decoration even though the transport below is already packet
// shaped: a bulk transfer carries no length of its own, and a read is up to a
// whole max packet size past the end of the packet.
//////////////////////////////////////////////////////////////////////////////

bool cp_needs_terminator(size_t record_len, unsigned maxpacket) {
	/* A zero-length record is not a record, and asking about one here would
	 * answer "yes" and have the caller write a zero-length transfer that ends
	 * nothing. */
	if (!record_len || !maxpacket) return false;
	return (record_len % maxpacket) == 0;
}

int cp_frame_encode(uint8_t* out, size_t cap, const uint8_t* packet, size_t len) {
	if (!out || len > CP_MTU || cap < len + CP_FRAME_HEADER) return 0;
	out[0] = (uint8_t)((len >> 8) & 0xff);
	out[1] = (uint8_t)(len & 0xff);
	if (len) memcpy(out + CP_FRAME_HEADER, packet, len);
	return (int)(len + CP_FRAME_HEADER);
}

int cp_frame_decode(const uint8_t* in, size_t have, const uint8_t** packet) {
	if (!in || have < CP_FRAME_HEADER) return -1;

	size_t len = ((size_t)in[0] << 8) | (size_t)in[1];
	/* Both checks are refusals rather than something to wait for: the writer
	 * puts one whole record in one transfer, so a record that is not whole here
	 * is a stream that has already lost its framing. */
	if (len > CP_MTU || have < CP_FRAME_HEADER + len) return -1;

	if (packet) *packet = in + CP_FRAME_HEADER;
	return (int)len;
}

//////////////////////////////////////////////////////////////////////////////
// paths
//
// Every path is built from the platform facts so that no two call sites can
// disagree about where a file is - the configfs function directory and the
// functionfs mount source in particular, which have to name the same instance.
//////////////////////////////////////////////////////////////////////////////

static int cp_join(const char* dir, const char* name, char* out, size_t cap) {
	if (!dir || !name || !out || !cap) return 0;
	int n = snprintf(out, cap, "%s/%s", dir, name);
	return (n > 0 && (size_t)n < cap) ? n : 0;
}

int cp_gadget_dir(const CP_Facts* f, char* out, size_t cap) {
	if (!f || !out || !f->configfs || !f->gadget) return 0;
	char root[CP_LINE_MAX];
	if (!cp_join(f->configfs, "usb_gadget", root, sizeof(root))) return 0;
	return cp_join(root, f->gadget, out, cap);
}

int cp_gadget_attr(const CP_Facts* f, const char* attr, char* out, size_t cap) {
	if (!f || !attr || !out) return 0;
	char dir[CP_LINE_MAX];
	if (!cp_gadget_dir(f, dir, sizeof(dir))) return 0;
	return cp_join(dir, attr, out, cap);
}

int cp_gadget_function_dir(const CP_Facts* f, char* out, size_t cap) {
	if (!f || !out || !f->function) return 0;

	/* configfs refuses an instance name whose length plus its NUL exceeds
	 * MAX_INST_NAME_LEN, so a longer one here is a path that could never have
	 * been created: building it would only move the failure to the mkdir, with
	 * nothing left in the message to say which limit was hit. */
	if (strlen(f->function) >= CP_NAME_MAX) return 0;

	char dir[CP_LINE_MAX], functions[CP_LINE_MAX], name[CP_NAME_MAX + 8];
	if (!cp_gadget_dir(f, dir, sizeof(dir))) return 0;
	if (!cp_join(dir, "functions", functions, sizeof(functions))) return 0;

	/* The buffer is longer than CP_NAME_MAX because this is the name with the
	 * driver's prefix on it, which is what configfs is asked to create. */
	int n = snprintf(name, sizeof(name), "ffs.%s", f->function);
	if (n <= 0 || (size_t)n >= sizeof(name)) return 0;
	return cp_join(functions, name, out, cap);
}

int cp_ffs_ep_file(const CP_Facts* f, const char* ep, char* out, size_t cap) {
	if (!f || !f->ffs_dir || !ep || !out) return 0;
	return cp_join(f->ffs_dir, ep, out, cap);
}

int cp_ffs_source(const CP_Facts* f, char* out, size_t cap) {
	if (!f || !f->function || !out || !cap) return 0;
	int n = snprintf(out, cap, "%s", f->function);
	return (n > 0 && (size_t)n < cap) ? n : 0;
}

int cp_role_node(const CP_Facts* f, const char* node, char* out, size_t cap) {
	if (!f || !f->role_dir || !node || !out) return 0;
	return cp_join(f->role_dir, node, out, cap);
}

/* The node whose *read* puts the port in `mode`. The vendor's otg_role reports
 * the unset mode as "null" while the node that selects it is "usb_null"; the
 * other two modes already carry the prefix and are their own node names. Pure,
 * and in this layer for the same reason the paths are: the daemon's force, the
 * daemon's restore and the setup app's recovery all have to agree on which
 * word names which node, and a convention each caller re-derives is a
 * disagreement waiting for the first caller that guesses. Returns 0 when the
 * mode is not one this platform knows. */
int cp_role_trigger_name(const char* mode, char* out, size_t cap) {
	if (!mode || !mode[0] || !out || !cap) return 0;
	if (strlen(mode) >= cap) return 0;

	static const struct {
		const char* mode;
		const char* node;
	} table[] = {
		{ "null",       "usb_null"   },
		{ "usb_device", "usb_device" },
		{ "usb_host",   "usb_host"   },
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (!strcmp(mode, table[i].mode)) {
			int n = snprintf(out, cap, "%s", table[i].node);
			return (n > 0 && (size_t)n < cap) ? n : 0;
		}
	return 0;
}

//////////////////////////////////////////////////////////////////////////////
// descriptor bytes, read the one way
//////////////////////////////////////////////////////////////////////////////

CP_EndpointDir cp_ep_dir(uint8_t bEndpointAddress) {
	if (!(bEndpointAddress & 0x80)) return bEndpointAddress ? CP_EP_OUT : CP_EP_UNKNOWN;
	return (bEndpointAddress & 0x0f) ? CP_EP_IN : CP_EP_UNKNOWN;
}

bool cp_ep_is_bulk(uint8_t bmAttributes) {
	return (bmAttributes & 0x03) == 0x02;
}

//////////////////////////////////////////////////////////////////////////////
// small file contents
//////////////////////////////////////////////////////////////////////////////

bool cp_attr_trim(char* text) {
	if (!text) return false;

	/* A sysfs attribute is a value with a newline after it, and the newline is
	 * not part of the value. Written in place: the caller's buffer is already
	 * as long as the file, and nothing here can make the result longer than it
	 * was. */
	char* start = text;
	while (*start == ' ' || *start == '\t') start++;

	size_t len = strlen(start);
	while (len && (start[len - 1] == '\n' || start[len - 1] == '\r' ||
	               start[len - 1] == ' '  || start[len - 1] == '\t')) {
		len--;
	}

	if (start != text) memmove(text, start, len);
	text[len] = '\0';
	return len != 0;
}

//////////////////////////////////////////////////////////////////////////////
// the status file
//
// The tokens are a table rather than a switch at each of the three places that
// needs one, so a state cannot be writable and unreadable, or readable and
// unnameable, by omission.
//////////////////////////////////////////////////////////////////////////////

static const struct {
	const char* name;
	CP_State    state;
} cp_state_names[] = {
	{ "idle",   CP_STATE_IDLE   },
	{ "role",   CP_STATE_ROLE   },
	{ "gadget", CP_STATE_GADGET },
	{ "wait",   CP_STATE_WAIT   },
	{ "up",     CP_STATE_UP     },
	{ "failed", CP_STATE_FAILED },
};

#define CP_STATE_NAME_COUNT (sizeof(cp_state_names) / sizeof(cp_state_names[0]))

static const struct {
	const char* name;
	CP_Role     role;
} cp_role_names[] = {
	{ "gadget", CP_ROLE_GADGET },
	{ "host",   CP_ROLE_HOST   },
};

#define CP_ROLE_NAME_COUNT (sizeof(cp_role_names) / sizeof(cp_role_names[0]))

/* The text fields, in the order they are written. state and role are not in
 * here: they are always present and always from the tables above, so a record
 * that names no medium or no role cannot be produced by this writer. */
static const struct {
	const char* key;
	size_t      offset;
	size_t      size;
} cp_status_fields[] = {
	{ "gadget", offsetof(CP_Status, gadget), sizeof(((CP_Status*)0)->gadget) },
	{ "udc",    offsetof(CP_Status, udc),    sizeof(((CP_Status*)0)->udc)    },
	{ "iface",  offsetof(CP_Status, iface),  sizeof(((CP_Status*)0)->iface)  },
	{ "ip",     offsetof(CP_Status, ip),     sizeof(((CP_Status*)0)->ip)     },
	{ "peer",   offsetof(CP_Status, peer),   sizeof(((CP_Status*)0)->peer)   },
	{ "error",  offsetof(CP_Status, error),  sizeof(((CP_Status*)0)->error)  },
};

#define CP_STATUS_FIELD_COUNT (sizeof(cp_status_fields) / sizeof(cp_status_fields[0]))

const char* cp_state_name(CP_State state) {
	for (size_t i = 0; i < CP_STATE_NAME_COUNT; i++)
		if (cp_state_names[i].state == state) return cp_state_names[i].name;
	return cp_state_names[0].name;
}

const char* cp_role_name(CP_Role role) {
	for (size_t i = 0; i < CP_ROLE_NAME_COUNT; i++)
		if (cp_role_names[i].role == role) return cp_role_names[i].name;
	return cp_role_names[0].name;
}

bool cp_state_from_name(const char* name, CP_State* out) {
	if (!name) return false;
	for (size_t i = 0; i < CP_STATE_NAME_COUNT; i++) {
		if (!strcmp(cp_state_names[i].name, name)) {
			if (out) *out = cp_state_names[i].state;
			return true;
		}
	}
	return false;
}

bool cp_role_from_name(const char* name, CP_Role* out) {
	if (!name) return false;
	for (size_t i = 0; i < CP_ROLE_NAME_COUNT; i++) {
		if (!strcmp(cp_role_names[i].name, name)) {
			if (out) *out = cp_role_names[i].role;
			return true;
		}
	}
	return false;
}

bool cp_value_ok(const char* value) {
	if (!value) return false;
	for (const unsigned char* p = (const unsigned char*)value; *p; p++)
		if (*p < 0x20 || *p == 0x7f) return false;
	return true;
}

void cp_status_init(CP_Status* s, CP_Role role) {
	if (!s) return;
	memset(s, 0, sizeof(*s));
	s->state = CP_STATE_IDLE;
	s->role = role;
}

bool cp_status_set_state(CP_Status* s, CP_State state) {
	if (!s) return false;

	/* cp_state_name falls back to idle for a value outside the enum, and idle is
	 * the one thing this file must never say about a session that is up - so the
	 * token is handed back to the reader and the state is kept only if it
	 * survives the round trip. */
	CP_State checked;
	if (!cp_state_from_name(cp_state_name(state), &checked) || checked != state) return false;

	s->state = state;
	return true;
}

bool cp_status_set(CP_Status* s, const char* key, const char* value) {
	if (!s || !key || !value) return false;

	if (!strcmp(key, "state")) {
		CP_State state;
		if (!cp_state_from_name(value, &state)) return false;
		s->state = state;
		return true;
	}
	if (!strcmp(key, "role")) {
		CP_Role role;
		if (!cp_role_from_name(value, &role)) return false;
		s->role = role;
		return true;
	}

	if (!cp_value_ok(value)) return false;
	for (size_t i = 0; i < CP_STATUS_FIELD_COUNT; i++) {
		if (strcmp(cp_status_fields[i].key, key)) continue;
		snprintf((char*)s + cp_status_fields[i].offset,
		         cp_status_fields[i].size, "%s", value);
		return true;
	}
	return false;
}

int cp_status_write(const CP_Status* s, char* out, size_t cap) {
	if (!s || !out || !cap) return 0;

	int n = snprintf(out, cap, "state=%s\nrole=%s\n",
	                 cp_state_name(s->state), cp_role_name(s->role));
	if (n < 0 || (size_t)n >= cap) return 0;

	for (size_t i = 0; i < CP_STATUS_FIELD_COUNT; i++) {
		/* An empty field is left out rather than written as key=: the app
		 * asks "is there an error" rather than "is error non-empty", and an
		 * absent key and an empty one then mean the same thing here. */
		const char* value = (const char*)s + cp_status_fields[i].offset;
		if (!value[0]) continue;

		int m = snprintf(out + n, cap - (size_t)n, "%s=%s\n",
		                 cp_status_fields[i].key, value);
		if (m < 0 || (size_t)m >= cap - (size_t)n) return 0;
		n += m;
	}
	return n;
}

bool cp_status_parse(const char* text, CP_Status* out) {
	if (!text || !out) return false;

	CP_Status status;
	memset(&status, 0, sizeof(status));

	bool have_state = false, have_role = false;
	const char* p = text;
	while (*p) {
		const char* end = strchr(p, '\n');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		if (len >= CP_LINE_MAX) return false;

		char line[CP_LINE_MAX];
		memcpy(line, p, len);
		line[len] = '\0';
		if (len && line[len - 1] == '\r') line[len - 1] = '\0';
		p = end ? end + 1 : p + len;

		if (!line[0]) continue;
		char* value = strchr(line, '=');
		if (!value) continue;   /* not a field; readers here ignore what they do not know */
		*value++ = '\0';

		if (!strcmp(line, "state")) {
			/* An unreadable state is not a status. This is the check that
			 * keeps a body cut off in the middle of its first line from being
			 * read as a daemon that has simply not started yet. */
			if (!cp_state_from_name(value, &status.state)) return false;
			have_state = true;
		} else if (!strcmp(line, "role")) {
			if (!cp_role_from_name(value, &status.role)) return false;
			have_role = true;
		} else {
			/* A value this writer could not have produced means the file is
			 * not ours, whatever else it says. */
			if (!cp_value_ok(value)) return false;
			(void)cp_status_set(&status, line, value);
		}
	}

	if (!have_state || !have_role) return false;
	*out = status;
	return true;
}

bool cp_status_link_ready(const CP_Status* s) {
	return s && s->state == CP_STATE_UP;
}
