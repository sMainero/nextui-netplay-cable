/*
 * Byte-exact checks on the cable link's pure layer.
 *
 * Everything here runs on the developer's machine: no device, no configfs, no
 * usbfs, no cross compiler. That is the point of the layer existing in this
 * shape. The three things it pins are the three where being wrong is expensive
 * and being wrong is invisible:
 *
 *   the descriptor blob  the kernel validates it and refuses the whole gadget
 *                        if a field is off, and the failure surfaces at bind
 *                        time as "the function did not become ready"
 *   the framing          a record whose length is a multiple of the endpoint's
 *                        packet size needs a terminator that 4.9's f_fs does
 *                        not write; get it wrong and the peer waits forever
 *   the status grammar   it is the app's only view of the daemon, so a body
 *                        that parses into the wrong state is worse than one
 *                        that does not parse at all
 *
 *   cableproto-unit
 *
 * Exit status is 0 only if every check passed.
 */
#include <stdio.h>
#include <string.h>

#include "usbnet.h"

static int failures = 0;

#define CHECK(cond, ...)                                    \
	do {                                                    \
		if (!(cond)) {                                      \
			printf("FAIL: ");                               \
			printf(__VA_ARGS__);                            \
			printf("\n");                                   \
			failures++;                                     \
		}                                                   \
	} while (0)

/* A stand-in for the tg5040 table in usbnet.c. Nothing here is tied to that
 * platform: the layout is the same on every Linux the pak runs on, and what the
 * table is for is that these paths have exactly one spelling. */
static const CP_Facts facts = {
	.configfs = "/sys/kernel/config",
	.ffs_dir  = "/dev/usb-ffs/cable",
	.udc_dir  = "/sys/class/udc",
	.tun_dev  = "/dev/net/tun",
	.role_dir = "/sys/devices/platform/soc/usbc0",
	.gadget   = "netplay",
	.function = "cable",
	.iface    = "npc0",
};

static bool bytes_equal(const uint8_t* got, const uint8_t* want, size_t len) {
	return memcmp(got, want, len) == 0;
}

static void test_descriptors(void) {
	/* Every byte, from the field table in usbnet.h. The eventfd is at offset
	 * 12 - before the counts, not after the descriptors - which is where
	 * f_fs.c reads it and not where the kernel's documentation prints it. */
	static const uint8_t want[CP_DESCS_MAX] = {
		0x03, 0x00, 0x00, 0x00,      /* 0  magic, FUNCTIONFS_DESCRIPTORS_MAGIC_V2 */
		0x46, 0x00, 0x00, 0x00,      /* 4  length, 70 */
		0x23, 0x00, 0x00, 0x00,      /* 8  flags, HAS_FS|HAS_HS|EVENTFD */
		0x07, 0x00, 0x00, 0x00,      /* 12 eventfd, the fd we were handed */
		0x03, 0x00, 0x00, 0x00,      /* 16 fs_count */
		0x03, 0x00, 0x00, 0x00,      /* 20 hs_count */

		/* 24 full speed: interface, bulk IN, bulk OUT */
		0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x01,
		0x07, 0x05, 0x81, 0x02, 0x40, 0x00, 0x00,
		0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00,

		/* 47 high speed: the same three at 512-byte packets */
		0x09, 0x04, 0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x01,
		0x07, 0x05, 0x81, 0x02, 0x00, 0x02, 0x00,
		0x07, 0x05, 0x02, 0x02, 0x00, 0x02, 0x00,
	};

	uint8_t out[CP_DESCS_MAX];
	CHECK(cp_build_descriptors(out, sizeof(out), 7) == (int)CP_DESCS_MAX,
	      "descriptor blob is %d bytes, expected %d", CP_DESCS_MAX, CP_DESCS_MAX);
	CHECK(bytes_equal(out, want, sizeof(want)), "descriptor blob differs from the wire format");
	for (size_t i = 0; i < sizeof(want); i++)
		if (out[i] != want[i]) printf("       byte %zu: got 0x%02x want 0x%02x\n", i, out[i], want[i]);

	/* The blob's own length field is what the kernel checks against the write,
	 * so a builder that disagrees with itself is a gadget that never binds. */
	CHECK(out[4] == (uint8_t)CP_DESCS_MAX, "length field does not match the blob");

	/* A buffer one byte short must be refused rather than truncated. */
	CHECK(cp_build_descriptors(out, sizeof(out) - 1, 7) == 0, "short buffer accepted");
	CHECK(cp_build_descriptors(out, sizeof(out), -1) == 0, "negative eventfd accepted");
	CHECK(cp_build_descriptors(NULL, sizeof(out), 7) == 0, "null buffer accepted");
}

static void test_strings(void) {
	static const uint8_t want[CP_STRINGS_MAX] = {
		0x02, 0x00, 0x00, 0x00,                 /* magic, FUNCTIONFS_STRINGS_MAGIC */
		0x20, 0x00, 0x00, 0x00,                 /* length, 32: the string's NUL counts */
		0x01, 0x00, 0x00, 0x00,                 /* str_count: one string, for iInterface 1 */
		0x01, 0x00, 0x00, 0x00,                 /* lang_count */
		0x09, 0x04,                             /* language, en-US */
		'n', 'e', 't', 'p', 'l', 'a', 'y', ' ',
		'c', 'a', 'b', 'l', 'e', 0x00,
	};

	uint8_t out[CP_STRINGS_MAX];
	CHECK(cp_build_strings(out, sizeof(out)) == (int)CP_STRINGS_MAX, "strings blob length");
	CHECK(bytes_equal(out, want, sizeof(want)), "strings blob differs from the wire format");

	/* The count has to cover the interface descriptor's iInterface, or the
	 * kernel discards the blob and the interface comes up unnamed. */
	CHECK(CP_STRINGS_MAX > CP_STRINGS_HEADER + 2, "strings blob has no string in it");
	CHECK((size_t)CP_DESCS_MAX > 16 && (size_t)CP_STRINGS_MAX >= 16,
	      "ep0 refuses a write shorter than 16 bytes");

	CHECK(cp_build_strings(out, sizeof(out) - 1) == 0, "short buffer accepted");
	CHECK(cp_build_strings(NULL, sizeof(out)) == 0, "null buffer accepted");
}

static void test_framing(void) {
	uint8_t packet[CP_MTU];
	for (size_t i = 0; i < sizeof(packet); i++) packet[i] = (uint8_t)(i * 7 + 1);

	uint8_t record[CP_RECORD_MAX];
	const uint8_t* seen = NULL;

	/* The empty record: the length class that has no packet at all. */
	CHECK(cp_frame_encode(record, sizeof(record), NULL, 0) == 2, "empty record length");
	CHECK(record[0] == 0x00 && record[1] == 0x00, "empty record header is not 0x0000");
	CHECK(cp_frame_decode(record, 2, &seen) == 0 && seen == record + 2, "empty record did not decode");

	/* 510 bytes makes the record 512, an exact multiple of the high-speed
	 * packet size - the aligned case that needs the terminator. */
	CHECK(cp_frame_encode(record, sizeof(record), packet, 510) == 512, "510-byte record length");
	CHECK(record[0] == 0x01 && record[1] == 0xfe, "510 is not big-endian in the header");
	CHECK(cp_needs_terminator(512, CP_MAXPACKET_HS), "aligned record at high speed needs no terminator");
	CHECK(cp_needs_terminator(512, CP_MAXPACKET_FS), "aligned record at full speed needs no terminator");
	CHECK(cp_frame_decode(record, 512, &seen) == 510, "aligned record did not decode");
	CHECK(seen == record + CP_FRAME_HEADER && !memcmp(seen, packet, 510), "aligned record payload");

	/* A full-size packet plus the header is not a multiple of either packet
	 * size, so the short packet ends it and no terminator is written. */
	CHECK(cp_frame_encode(record, sizeof(record), packet, CP_MTU) == CP_RECORD_MAX, "MTU record length");
	CHECK(!cp_needs_terminator(CP_RECORD_MAX, CP_MAXPACKET_HS), "1502 needs a terminator at high speed");
	CHECK(!cp_needs_terminator(CP_RECORD_MAX, CP_MAXPACKET_FS), "1502 needs a terminator at full speed");
	CHECK(cp_frame_decode(record, CP_RECORD_MAX, &seen) == CP_MTU, "MTU record did not decode");
	CHECK(!memcmp(seen, packet, CP_MTU), "MTU record payload");

	/* A terminator is never written for a non-record or a non-length. */
	CHECK(!cp_needs_terminator(0, CP_MAXPACKET_HS), "a zero-length record asked for a terminator");
	CHECK(!cp_needs_terminator(512, 0), "a zero max packet size asked for a terminator");

	/* Absences: no length, an impossible length, and a record cut short. */
	CHECK(cp_frame_decode(record, 1, &seen) == -1, "a one-byte buffer decoded");
	record[0] = 0x07; record[1] = 0xd0;   /* 2000 > MTU */
	CHECK(cp_frame_decode(record, CP_RECORD_MAX, &seen) == -1, "an over-MTU record decoded");
	CHECK(cp_frame_encode(record, sizeof(record), packet, CP_MTU) == CP_RECORD_MAX, "MTU record length again");
	CHECK(cp_frame_decode(record, CP_RECORD_MAX - 1, &seen) == -1, "a truncated record decoded");
	CHECK(cp_frame_decode(NULL, 64, &seen) == -1, "a null buffer decoded");
	CHECK(cp_frame_decode(record, sizeof(record), NULL) == CP_MTU, "decoding without the out parameter failed");

	/* An over-MTU packet is refused by the encoder, so the decoder should never
	 * have to describe one. */
	CHECK(cp_frame_encode(record, sizeof(record), packet, CP_MTU + 1) == 0, "an over-MTU packet was encoded");
	CHECK(cp_frame_encode(record, 3, packet, 2) == 0, "a record that does not fit was encoded");
}

static void test_paths(void) {
	char out[CP_LINE_MAX];

#define EXPECT(expr, want)                                          \
	do {                                                            \
		out[0] = '\0';                                              \
		CHECK((expr) > 0, "path refused: %s", #expr);                \
		CHECK(!strcmp(out, want), "%s -> '%s', expected '%s'",       \
		      #expr, out, want);                                    \
	} while (0)

	EXPECT(cp_gadget_dir(&facts, out, sizeof(out)),
	       "/sys/kernel/config/usb_gadget/netplay");
	EXPECT(cp_gadget_attr(&facts, "UDC", out, sizeof(out)),
	       "/sys/kernel/config/usb_gadget/netplay/UDC");
	EXPECT(cp_gadget_function_dir(&facts, out, sizeof(out)),
	       "/sys/kernel/config/usb_gadget/netplay/functions/ffs.cable");
	EXPECT(cp_ffs_ep_file(&facts, CP_EP_FILE_IN, out, sizeof(out)),
	       "/dev/usb-ffs/cable/ep1");
	EXPECT(cp_ffs_ep_file(&facts, CP_EP_FILE_OUT, out, sizeof(out)),
	       "/dev/usb-ffs/cable/ep2");
	EXPECT(cp_ffs_source(&facts, out, sizeof(out)), "cable");
	EXPECT(cp_role_node(&facts, "usb_host", out, sizeof(out)),
	       "/sys/devices/platform/soc/usbc0/usb_host");
#undef EXPECT

	/* The mode words the port reports, and the node each is restored through.
	 * "null" is the wrinkle: the mode is reported without the prefix while its
	 * node carries one, and a caller that re-derived that would be a caller that
	 * could disagree with this table. */
	char node[CP_LINE_MAX];
	CHECK(cp_role_trigger_name("null", node, sizeof(node)) > 0 && !strcmp(node, "usb_null"),
	      "null did not resolve to usb_null");
	CHECK(cp_role_trigger_name("usb_device", node, sizeof(node)) > 0 && !strcmp(node, "usb_device"),
	      "usb_device did not resolve to itself");
	CHECK(cp_role_trigger_name("usb_host", node, sizeof(node)) > 0 && !strcmp(node, "usb_host"),
	      "usb_host did not resolve to itself");
	CHECK(cp_role_trigger_name("host", node, sizeof(node)) == 0, "an unknown mode was accepted");
	CHECK(cp_role_trigger_name("", node, sizeof(node)) == 0, "an empty mode was accepted");
	CHECK(cp_role_trigger_name(NULL, node, sizeof(node)) == 0, "a null mode was accepted");
	CHECK(cp_role_trigger_name("null", node, 0) == 0, "a zero-size buffer was accepted");
	char small_node[8];
	CHECK(cp_role_trigger_name("usb_host", small_node, sizeof(small_node)) == 0,
	      "a truncated trigger name was accepted");

	/* Refusals, because a truncated path opens a different file and reports it
	 * as a missing attribute. */
	char small[12];
	CHECK(cp_gadget_dir(&facts, small, sizeof(small)) == 0, "a truncated gadget path was accepted");
	CHECK(cp_gadget_attr(&facts, NULL, out, sizeof(out)) == 0, "a null attribute was accepted");
	CHECK(cp_gadget_attr(&facts, "UDC", out, 0) == 0, "a zero-size buffer was accepted");
	CHECK(cp_gadget_dir(NULL, out, sizeof(out)) == 0, "null facts accepted");
	CHECK(cp_ffs_ep_file(&facts, NULL, out, sizeof(out)) == 0, "a null endpoint was accepted");

	/* A platform whose port role it cannot force: there is no path, and saying
	 * so is not an error at the call site - it is the answer "this device
	 * cannot do that", which is what the app reports. */
	CP_Facts norole = facts;
	norole.role_dir = NULL;
	CHECK(cp_role_node(&norole, "usb_host", out, sizeof(out)) == 0, "a role node was built with no role directory");
	CHECK(cp_gadget_dir(&norole, out, sizeof(out)) > 0, "the gadget path depends on the role directory");

	/* An instance name at the kernel's own limit still fits; one past it is
	 * refused by configfs anyway, and is refused here too. */
	CP_Facts longname = facts;
	longname.function = "012345678901234567890123456789012345678";    /* 39: the longest configfs takes */
	CHECK(cp_gadget_function_dir(&longname, out, sizeof(out)) > 0,
	      "an instance name at configfs's limit was refused");
	longname.function = "0123456789012345678901234567890123456789";   /* 40 */
	CHECK(cp_gadget_function_dir(&longname, out, sizeof(out)) == 0,
	      "an instance name configfs would refuse was accepted");

	/* And a path that does not fit is refused rather than cut, whatever the
	 * name's length. */
	CHECK(cp_gadget_function_dir(&facts, small, sizeof(small)) == 0,
	      "a truncated function path was accepted");
}

static void test_descriptor_bytes(void) {
	CHECK(cp_ep_dir(CP_EP_ADDR_IN) == CP_EP_IN, "0x81 is not an IN endpoint");
	CHECK(cp_ep_dir(CP_EP_ADDR_OUT) == CP_EP_OUT, "0x02 is not an OUT endpoint");
	CHECK(cp_ep_dir(0x85) == CP_EP_IN, "0x85 is not an IN endpoint");
	CHECK(cp_ep_dir(0x00) == CP_EP_UNKNOWN, "address 0 is an endpoint");
	CHECK(cp_ep_dir(0x80) == CP_EP_UNKNOWN, "an endpoint with number 0 is an endpoint");

	CHECK(cp_ep_is_bulk(0x02), "bulk is not bulk");
	CHECK(!cp_ep_is_bulk(0x03), "interrupt read as bulk");
	CHECK(!cp_ep_is_bulk(0x01), "isochronous read as bulk");
}

static void test_attr_trim(void) {
	char buf[64];

	CHECK(!cp_attr_trim(NULL), "a null attribute trimmed to something");

	strcpy(buf, "sunxi-udc\n");
	CHECK(cp_attr_trim(buf) && !strcmp(buf, "sunxi-udc"), "a UDC name did not trim: '%s'", buf);

	strcpy(buf, "  g1 \t\r\n");
	CHECK(cp_attr_trim(buf) && !strcmp(buf, "g1"), "blanks did not trim: '%s'", buf);

	strcpy(buf, "\n");
	CHECK(!cp_attr_trim(buf) && !buf[0], "an unbound attribute read as a value");

	strcpy(buf, "");
	CHECK(!cp_attr_trim(buf), "an empty attribute read as a value");
}

static void test_status(void) {
	CP_Status status;
	char text[CP_STATUS_MAX];

	/* What a daemon writes before it has tried anything. */
	cp_status_init(&status, CP_ROLE_GADGET);
	int n = cp_status_write(&status, text, sizeof(text));
	CHECK(n == (int)strlen("state=idle\nrole=gadget\n"), "the initial record is %d bytes", n);
	CHECK(!strcmp(text, "state=idle\nrole=gadget\n"), "the initial record is '%s'", text);

	/* Every field, in the documented order, with the empty one left out. */
	CHECK(cp_status_set_state(&status, CP_STATE_FAILED), "failed is not a state");
	CHECK(cp_status_set(&status, "role", "host"), "role=host refused");
	CHECK(cp_status_set(&status, "gadget", "netplay"), "gadget refused");
	CHECK(cp_status_set(&status, "udc", "sunxi-udc"), "udc refused");
	CHECK(cp_status_set(&status, "iface", CP_IFACE_NAME), "iface refused");
	CHECK(cp_status_set(&status, "ip", "10.77.0.2"), "ip refused");
	CHECK(cp_status_set(&status, "peer", "10.77.0.1"), "peer refused");
	CHECK(cp_status_set(&status, "error", "could not claim interface"), "error refused");

	n = cp_status_write(&status, text, sizeof(text));
	CHECK(n > 0, "the full record did not serialise");
	CHECK(!strcmp(text,
	              "state=failed\n"
	              "role=host\n"
	              "gadget=netplay\n"
	              "udc=sunxi-udc\n"
	              "iface=npc0\n"
	              "ip=10.77.0.2\n"
	              "peer=10.77.0.1\n"
	              "error=could not claim interface\n"),
	      "the full record is:\n%s", text);

	CP_Status back;
	CHECK(cp_status_parse(text, &back), "our own record did not parse");
	CHECK(back.state == status.state && back.role == status.role, "state or role did not survive");
	CHECK(!strcmp(back.gadget, status.gadget) && !strcmp(back.udc, status.udc) &&
	      !strcmp(back.iface, status.iface) && !strcmp(back.ip, status.ip) &&
	      !strcmp(back.peer, status.peer) && !strcmp(back.error, status.error),
	      "a field did not survive the round trip");

	/* The record a reader treats as live. */
	cp_status_init(&status, CP_ROLE_GADGET);
	CHECK(cp_status_set_state(&status, CP_STATE_WAIT), "wait is not a state");
	CHECK(!cp_status_link_ready(&status), "waiting read as a live link");
	CHECK(cp_status_set_state(&status, CP_STATE_UP), "up is not a state");
	CHECK(cp_status_link_ready(&status), "up did not read as a live link");
	CHECK(cp_status_set_state(&status, CP_STATE_FAILED), "failed is not a state");
	CHECK(!cp_status_link_ready(&status), "a failed daemon read as a live link");
	CHECK(!cp_status_link_ready(NULL), "null read as a live link");

	/* A body that could not have been written is not a status. */
	CHECK(!cp_status_parse("", &back), "an empty body parsed");
	CHECK(!cp_status_parse("state=up\n", &back), "a body with no role parsed");
	CHECK(!cp_status_parse("role=host\n", &back), "a body with no state parsed");
	CHECK(!cp_status_parse("state=nonsense\nrole=host\n", &back), "an unknown state parsed");
	CHECK(!cp_status_parse("state=up\nrole=wizard\n", &back), "an unknown role parsed");
	CHECK(!cp_status_parse("state=u\nrole=host\n", &back), "a half-written state parsed");
	/* A value cannot carry a line break, because the format has no escape for
	 * one: the line simply ends, and what follows is read as the next field.
	 * That is why the defence is on the writing side, and there is nothing to
	 * reject here. */
	CHECK(cp_status_parse("state=up\nrole=host\nerror=a\nb\n", &back) && !strcmp(back.error, "a"),
	      "a value did not read back up to its line end");
	CHECK(!cp_status_parse(NULL, &back), "a null body parsed");
	CHECK(!cp_status_parse("state=up\nrole=host\n", NULL), "a null output parsed");

	/* ...and what a reader is allowed to be generous about: fields it does not
	 * know, a stray line, a carriage return from a hand-edited file. */
	CHECK(cp_status_parse("state=up\nrole=host\nnonsense=1\n", &back), "an unknown key rejected the body");
	CHECK(cp_status_parse("state=up\r\nrole=host\r\n", &back), "a carriage return rejected the body");
	CHECK(cp_status_parse("gibberish\nstate=up\nrole=host\n", &back), "a line that is not a field rejected the body");
	CHECK(cp_status_parse("state=up\nrole=host", &back), "a body with no trailing newline rejected");

	/* The last occurrence wins, as it does for the session file. */
	CHECK(cp_status_parse("state=up\nrole=host\nstate=wait\n", &back) && back.state == CP_STATE_WAIT,
	      "a repeated key did not take the last value");

	/* Every prefix shorter than a complete state and role fails to parse, and
	 * every prefix of at least that length parses. This is the property that
	 * keeps a record caught mid-write from being read as a daemon that is
	 * merely idle - the failure the writer's rename is supposed to prevent, and
	 * the one it cannot help if the file is ever truncated by something else. */
	cp_status_init(&status, CP_ROLE_HOST);
	n = cp_status_write(&status, text, sizeof(text));
	CHECK(n > 0, "the prefix base record did not serialise");
	/* Complete once the header's last field is, whether or not that field's
	 * newline arrived: everything before that point has to be refused. */
	const char* last_newline = strrchr(text, '\n');
	const size_t need = last_newline ? (size_t)(last_newline - text) : 0;
	for (size_t cut = 0; cut <= (size_t)n; cut++) {
		char prefix[CP_STATUS_MAX];
		memcpy(prefix, text, cut);
		prefix[cut] = '\0';
		bool parsed = cp_status_parse(prefix, &back);
		if (cut < need && parsed) {
			CHECK(false, "a body cut to %zu bytes parsed", cut);
			break;
		}
		if (cut >= need && !parsed) {
			CHECK(false, "a body cut to %zu bytes did not parse", cut);
			break;
		}
	}

	/* The writer refuses what the reader would have to reject. */
	CHECK(!cp_status_set(&status, "gadgett", "netplay"), "an unknown key was written");
	CHECK(!cp_status_set(&status, "error", "line\nbreak"), "a value with a line break was written");
	CHECK(!status.error[0], "a refused value was stored anyway");
	CHECK(!cp_status_set(&status, "state", "nonsense"), "an unknown state token was written");
	CHECK(!cp_status_set(&status, "role", "wizard"), "an unknown role token was written");
	CHECK(!cp_status_set(&status, "iface", NULL), "a null value was written");
	CHECK(!cp_status_set(NULL, "iface", "npc0"), "a null status was written");
	CHECK(!cp_status_set_state(&status, (CP_State)99), "a state outside the enum was set");
	CHECK(cp_status_set_state(&status, CP_STATE_IDLE), "idle is not a state");
	CHECK(!cp_status_set_state(NULL, CP_STATE_UP), "a null status was set");

	CHECK(cp_value_ok("could not claim interface"), "an ordinary value was refused");
	CHECK(!cp_value_ok("a\nb"), "a newline in a value was allowed");
	CHECK(!cp_value_ok("\x7f"), "a delete character in a value was allowed");

	/* A field longer than its storage is truncated, as everywhere else in this
	 * codebase, and stays a string a reader can use. */
	char toolong[CP_IFACE_MAX + 8];
	memset(toolong, 'x', sizeof(toolong) - 1);
	toolong[sizeof(toolong) - 1] = '\0';
	CHECK(cp_status_set(&status, "iface", toolong), "an over-long value was refused");
	CHECK(strlen(status.iface) == CP_IFACE_MAX - 1, "an over-long value was not truncated");

	/* Serialising into a buffer that cannot hold the record is refused rather
	 * than cut. */
	char tiny[16];
	CHECK(cp_status_write(&status, tiny, sizeof(tiny)) == 0, "a record that does not fit was written");
	CHECK(cp_status_write(NULL, tiny, sizeof(tiny)) == 0, "a null status was written");
	CHECK(cp_status_write(&status, NULL, sizeof(tiny)) == 0, "a null buffer was written");

	/* An unset field is absent, not empty, and both read back the same way. */
	cp_status_init(&status, CP_ROLE_HOST);
	CHECK(cp_status_write(&status, text, sizeof(text)) > 0, "the empty record did not serialise");
	CHECK(!strstr(text, "error="), "an unset field was written: %s", text);
	CHECK(cp_status_parse(text, &back) && !back.error[0], "an absent field did not read as empty");
}

int main(void) {
	printf("descriptors\n");         test_descriptors();
	printf("strings\n");             test_strings();
	printf("framing\n");             test_framing();
	printf("descriptor bytes\n");    test_descriptor_bytes();
	printf("paths\n");               test_paths();
	printf("attribute trimming\n");  test_attr_trim();
	printf("status grammar\n");      test_status();

	printf(failures ? "\nRESULT: %d failure(s)\n" : "\nRESULT: ok (0 failures)\n", failures);
	return failures ? 1 : 0;
}
