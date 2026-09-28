#!/bin/sh
# The cable link's host-side suite.
#
#   ./testing/test-cable.sh
#
# Four things are asked, in the order they can fail:
#
#   the pure layer     the descriptor and strings blobs, the framing codec, the
#                      zero-length-packet rule, the configfs path spellings and
#                      the status grammar - all pure functions, built and run
#                      natively with no cross compiler, no NextUI checkout and no
#                      device
#   one vocabulary     this transport is split across two processes, two
#                      languages and four readers, and every shared name - the
#                      six state tokens, the two role words, the three link
#                      tokens, the four state-file names, the four usb_restore
#                      keys - is written down twice. Each pair is compared here,
#                      because when they drift the daemon and the app do not
#                      crash: they quietly disagree about what "up" means
#   the launcher guards the link= key as a session reads it, the one set of files
#                      an ended session removes, the repair record a reboot makes
#                      moot, and the WiFi work a cable launch skips
#   the harness        that the session writer names the medium, so a hand-armed
#                      session is classified the way the app classifies it
#
# Deliberately not here: gadget bring-up, the usbfs URB loop, and whether a C-to-C
# cable between two of these devices enumerates at all. A machine with no USB
# controller, no /dev/net/tun and no usbfs cannot ask, and those answers are the
# device checklist in docs/cable.md. The daemon is cross-built - it links
# netsetup.c, so it wants the NextUI tree and the platform's ABI makefile, and
# app/makefile has no host rule for it at all - so the lifecycle half at the
# bottom is gated on a binary this machine can actually *run*, not merely execute
# the x bit of. On the tree as it stands that gate has never opened, and the two
# `skip` lines it prints say so rather than reporting a pass.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
PAK="$(cd "$HERE/.." && pwd)"
APP="$PAK/app"
fail=0
ok()   { echo "  ok   $1"; }
bad()  { echo "  FAIL $1"; fail=1; }
skip() { echo "  skip $1"; }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1 (got '$2', want '$3')"; fi; }

ROOT=$(mktemp -d)
trap 'rm -rf "$ROOT"' EXIT

###########################################################################
echo "== the pure protocol layer"
###########################################################################

if make -C "$APP" proto-test > "$ROOT/proto.log" 2> "$ROOT/proto.err"; then
	grep -q 'RESULT: ok (0 failures)' "$ROOT/proto.log" \
		&& ok "every descriptor, framing and status check passed" \
		|| { bad "the unit test did not report ok"; sed -n '1,30p' "$ROOT/proto.log"; }
else
	bad "make proto-test exited non-zero"
	sed -n '1,30p' "$ROOT/proto.log"
fi
if [ -s "$ROOT/proto.err" ]; then
	bad "the native build is not warning-clean"
	sed -n '1,20p' "$ROOT/proto.err"
else
	ok "warning-clean under the native flags"
fi

###########################################################################
echo
echo "== one vocabulary, two processes"
###########################################################################

# The six lifecycle states. The daemon writes them into cable.status and the app
# reads them back, so the two tables have to be the same six words in the same
# order - a state that is writable and unreadable is a session the app believes
# never started.
sed -n '/cp_state_names\[\] = {/,/};/p' "$APP/cableproto.c" | grep -o '"[a-z]*"' > "$ROOT/states-daemon"
sed -n '/ns_cable_states\[\] = {/,/};/p' "$APP/netsetup.c" | grep -o '"[a-z]*"' > "$ROOT/states-app"
if diff -q "$ROOT/states-daemon" "$ROOT/states-app" >/dev/null 2>&1; then
	ok "both sides know the six states: $(tr '\n' ' ' < "$ROOT/states-daemon")"
else
	bad "the daemon's states and the app's differ"
	diff "$ROOT/states-daemon" "$ROOT/states-app"
fi

# The two role words the app hands the daemon on the command line and the daemon
# parses back. A role the daemon does not know is a refused argument; a role the
# app spells differently is a daemon that never starts.
sed -n '/cp_role_names\[\] = {/,/};/p' "$APP/cableproto.c" | grep -o '"[a-z]*"' | tr -d '"' | sort > "$ROOT/roles-daemon"
grep -oE 'cable_start\("(gadget|host)"' "$APP/netsetup.c" | grep -o '"[a-z]*"' | tr -d '"' | sort > "$ROOT/roles-app"
check "both sides know the two role words" \
	"$(tr '\n' ' ' < "$ROOT/roles-daemon")" "$(tr '\n' ' ' < "$ROOT/roles-app")"

# The three link tokens, in the app and in the shell. netplay_link_kind's case
# pattern is the launcher's copy of the app's table, and a token the app writes
# that the shell does not recognise falls back to the ad-hoc/wifi inference -
# which is a session the launcher classifies as something else entirely.
sed -n '/ns_link_names\[\] = {/,/};/p' "$APP/netsetup.c" | grep -o '"[a-z]*"' | tr -d '"' | sort > "$ROOT/links-app"
grep -o 'cable|adhoc|wifi' "$PAK/launcher/state-path.sh" | head -1 | tr '|' '\n' | sort > "$ROOT/links-shell"
check "the launcher's link tokens are the app's" \
	"$(tr '\n' ' ' < "$ROOT/links-app")" "$(tr '\n' ' ' < "$ROOT/links-shell")"

# The four files the daemon owns, named once, in its own header.
grep -oE '^#define CP_(PID|STATUS|LOG|RESTORE)_FILE +"[a-z_.]+"' "$APP/usbnet.h" \
	| grep -o '"[a-z_.]*"' | tr -d '"' | sort > "$ROOT/files-daemon"
printf 'cable.log\ncable.pid\ncable.status\nusb_restore\n' > "$ROOT/files-want"
check "the daemon's four state files" \
	"$(tr '\n' ' ' < "$ROOT/files-daemon")" "$(tr '\n' ' ' < "$ROOT/files-want")"

# The repair record's keys: the daemon writes them, the app's recovery reads
# them, and a key neither of them spells the same way is a repair that never
# happens. The three the takeover added name what it displaced in the firmware's
# gadget, and they are read by the same recovery as the two the old record had.
for key in gadget udc function config mount exe identity role_node role_value; do
	if grep -qE "CP_RESTORE_KEY_[A-Z_]+[[:space:]]+\"$key\"" "$APP/usbnet.c" \
	   && grep -q "\"$key\"" "$APP/netsetup.c"; then
		ok "usb_restore key '$key' is written and read"
	else
		bad "usb_restore key '$key' is named differently on the two sides"
	fi
done

# The cap has to hold the widest record either role writes: the gadget role's
# seven plus the port's two. A cap that is one short is a record the daemon
# silently cannot write, which is a repair nobody performs.
check "the record's line cap holds every key" \
	"$(sed -n 's/^#define CP_RESTORE_MAX_LINES \([0-9]*\)$/\1/p' "$APP/usbnet.c")" "9"
check "the record's keys all fit the cap" \
	"$(grep -cE '^#define CP_RESTORE_KEY_' "$APP/usbnet.c")" "9"

###########################################################################
echo
echo "== the launcher guards"
###########################################################################

# A fake SD tree with just the launcher scripts in it, the shape launcher/test.sh
# builds. Game Switcher and the ad-hoc rejoin are stubbed, because
# netplay_end_session idles the first and pre-launch.sh runs the second, and both
# real ones would reach for a device.
NP="$ROOT/SD/Tools/tg5040/Netplay.pak"
STATE="$ROOT/SD/.userdata/shared/Netplay"
STUB_BIN="$ROOT/stub-bin"
mkdir -p "$STATE" "$NP/launcher" "$STUB_BIN"
cp "$PAK/launcher/state-path.sh" "$PAK/launcher/session-cleanup.sh" \
   "$PAK/launcher/pre-launch.sh" "$PAK/launcher/wifi-platform.sh" \
   "$PAK/launcher/adhoc-join.sh" "$NP/launcher/"
cat > "$NP/launcher/gameswitcher.sh" <<EOF
#!/bin/sh
echo "gameswitcher \$*" >> "$ROOT/gs-calls"
EOF
cat > "$STUB_BIN/iw" <<EOF
#!/bin/sh
echo "iw \$*" >> "$ROOT/iw-calls"
EOF
chmod 755 "$NP/launcher"/* "$STUB_BIN/iw"
export SDCARD_PATH="$ROOT/SD" PLATFORM=tg5040 NETPLAY_PAK="$NP"

# The link kind: the session is authoritative, an absent key falls back the way
# the app falls back, and so does a token this build does not know.
printf 'role=host\nlink=cable\n' > "$ROOT/s-cable"
printf 'role=host\nadhoc_ssid=nextui-TEST\n' > "$ROOT/s-legacy"
printf 'role=host\nlink=bogus\n' > "$ROOT/s-bogus"
. "$NP/launcher/state-path.sh"
check "the session's own key is the answer" "$(netplay_link_kind "$ROOT/s-cable")" "cable"
check "the previous build's ad-hoc session still reads as ad hoc" \
	"$(netplay_link_kind "$ROOT/s-legacy")" "adhoc"
check "an unrecognised token falls back to the same inference" \
	"$(netplay_link_kind "$ROOT/s-bogus")" "wifi"
check "no session at all is its own answer" "$(netplay_link_kind "$ROOT/none")" "none"

echo
echo "== the set of files an ended session removes"
# The daemon's pid and status go with the broker's, because one left behind makes
# the next session believe a daemon is already running and already owns the wire.
# Its repair record and its log deliberately do not: the record is a repair this
# device still owes, and a log is not session state.
REMOVAL=$(grep -A2 'rm -f "$1"' "$NP/launcher/state-path.sh")
echo "$REMOVAL" | grep -q 'cable\.pid' \
	&& ok "cable.pid is in the removal set" || bad "cable.pid is not in the removal set"
echo "$REMOVAL" | grep -q 'cable\.status' \
	&& ok "cable.status is in the removal set" || bad "cable.status is not in the removal set"
echo "$REMOVAL" | grep -q 'usb_restore' \
	&& bad "the repair record is removed with the session" || ok "the repair record is not"
echo "$REMOVAL" | grep -q 'cable\.log' \
	&& bad "the log is removed with the session" || ok "the log is not"

printf 'role=host\nlink=cable\n' > "$STATE/session"
: > "$STATE/broker.pid"; : > "$STATE/broker.status"
: > "$STATE/cable.pid";  : > "$STATE/cable.status"
: > "$STATE/usb_restore"; : > "$STATE/cable.log"
netplay_end_session "$STATE/session"
for f in session broker.pid broker.status cable.pid cable.status; do
	[ -e "$STATE/$f" ] && bad "an ended session left $f behind" || ok "an ended session removed $f"
done
for f in usb_restore cable.log; do
	[ -e "$STATE/$f" ] && ok "an ended session kept $f" || bad "an ended session removed $f"
done
grep -q 'gameswitcher idle' "$ROOT/gs-calls" \
	&& ok "and the Game Switcher was idled" || bad "the Game Switcher was left armed"
rm -f "$STATE/usb_restore" "$STATE/cable.log"

echo
echo "== the repair record a reboot makes moot"
# configfs is a RAM filesystem and the port's role is a register, so a reboot puts
# the firmware's own gadget and its own port mode back by itself: a record that
# outlived its boot names a repair that no longer exists and cannot be performed.
printf 'boot-now\n' > "$ROOT/boot-id"
printf 'gadget=fw\nudc=abc\nfunction=ffs.adb\nconfig=c.1\nmount=/dev/usb-ffs/adb\nexe=/bin/adbd -D\nidentity=0x18d1,0xd002,0x0409\nrole_node=usb_null\nrole_value=null\n' > "$STATE/usb_restore"
printf 'role=host\nboot_id=boot-now\n' > "$STATE/session"
NETPLAY_BOOT_ID_PATH="$ROOT/boot-id" "$NP/launcher/session-cleanup.sh" >/dev/null 2>&1
[ -e "$STATE/usb_restore" ] \
	&& ok "a session from this boot keeps the record it owns" \
	|| bad "a live session's own repair record was dropped"
printf 'role=host\nboot_id=boot-old\n' > "$STATE/session"
NETPLAY_BOOT_ID_PATH="$ROOT/boot-id" "$NP/launcher/session-cleanup.sh" >/dev/null 2>&1
[ -e "$STATE/usb_restore" ] \
	&& bad "a previous boot's repair record survived cleanup" \
	|| ok "a previous boot's repair record was dropped"
rm -f "$STATE/session" "$STATE/cleanup.log" "$STATE/cable.pid" "$STATE/cable.status"

echo
echo "== the WiFi work a cable launch does not do"
# pre-launch.sh is sourced by the launch routes, so `set -e` is in force exactly
# as it is at launch - a step that fails aborts the launch. That is why the stub
# bin exists and is ahead of PATH on every call below.
mv "$NP/launcher/adhoc-join.sh" "$NP/launcher/adhoc-join.real"
cat > "$NP/launcher/adhoc-join.sh" <<EOF
#!/bin/sh
echo "adhoc-join \$*" >> "$ROOT/adhoc-calls"
EOF
chmod 755 "$NP/launcher/adhoc-join.sh"

prelaunch() { PATH="$STUB_BIN:$PATH" . "$NP/launcher/pre-launch.sh"; }

# A cable link never moved the radio, so it owes the radio nothing: no power-save
# to reapply and no network to rejoin.
: > "$ROOT/iw-calls"; : > "$ROOT/adhoc-calls"
printf 'role=host\nlink=cable\n' > "$STATE/session"
( prelaunch )
[ -s "$ROOT/iw-calls" ] \
	&& bad "a cable launch touched WiFi power-save" || ok "a cable launch left power-save alone"
[ -s "$ROOT/adhoc-calls" ] \
	&& bad "a cable launch ran the ad-hoc rejoin" || ok "a cable launch did not rejoin an ad-hoc network"

# And the other direction, so the guard is not simply an off switch.
: > "$ROOT/iw-calls"; : > "$ROOT/adhoc-calls"
printf 'role=host\nlink=adhoc\nadhoc_ssid=nextui-TEST\n' > "$STATE/session"
( prelaunch )
[ -s "$ROOT/iw-calls" ] \
	&& ok "an ad-hoc launch reapplied power-save" || bad "an ad-hoc launch skipped power-save"
[ -s "$ROOT/adhoc-calls" ] \
	&& ok "an ad-hoc launch rejoined the network" || bad "an ad-hoc launch skipped the rejoin"

# Including one armed by the build that predates link=.
: > "$ROOT/adhoc-calls"
printf 'role=host\nadhoc_ssid=nextui-TEST\n' > "$STATE/session"
( prelaunch )
[ -s "$ROOT/adhoc-calls" ] \
	&& ok "a previous build's ad-hoc session still rejoins" \
	|| bad "the fallback stopped an armed ad-hoc session rejoining"

rm -f "$STATE/session"
mv "$NP/launcher/adhoc-join.real" "$NP/launcher/adhoc-join.sh"

###########################################################################
echo
echo "== the harness names the medium"
###########################################################################

HARNESS="$PAK/tools/netplay-harness.py"
check "a LAN pair is written as a wifi session" \
	"$(python3 "$HARNESS" session --host 192.168.0.180 --client 192.168.0.181 --dry-run 2>/dev/null | grep -c '^link=wifi$')" "2"
check "an ad-hoc host address is written as an ad-hoc session" \
	"$(python3 "$HARNESS" session --host 10.0.0.1 --client 192.168.0.181 --dry-run 2>/dev/null | grep -c '^link=adhoc$')" "2"
check "a caller that knows better is written once, not twice" \
	"$(python3 "$HARNESS" session --host 192.168.0.180 --client 192.168.0.181 --set link=cable --dry-run 2>/dev/null | grep -c '^link=')" "2"
# argparse prints a subcommand's choice list twice - once in the usage line and
# once on the option itself - so one artifact that belongs in the list is two
# occurrences of its name here, not one.
check "the cable ELF is a deployable artifact" \
	"$(python3 "$HARNESS" deploy --help 2>&1 | grep -c 'netplay-cable')" "2"
if PYTHONDONTWRITEBYTECODE=1 python3 "$PAK/tools/test-netplay-harness.py" > "$ROOT/harness.log" 2>&1; then
	ok "the harness's own suite still passes"
else
	bad "the harness's own suite failed"
	sed -n '1,30p' "$ROOT/harness.log"
fi

###########################################################################
echo
echo "== the daemon's lifecycle, against a fake configfs tree"
###########################################################################

# The daemon is cross-built and links netsetup.c, so there is no host build of it
# - see the header. Where a binary this machine can run exists, the contract the
# fake tree is for can be asked: an unknown role is refused by name, a missing
# role prints the usage, and a daemon that does come up takes the pid file as a
# lock, publishes its status, and leaves neither behind on the way out. Where
# there is none this says so.
# The gate is whether it runs, not whether the x bit is set: `make app` produces
# an executable aarch64 ELF on any host, and a macOS machine would then try to
# execute it. Asking it for its usage and taking a real answer is the signal.
CABLE_BIN="${CABLE_BIN:-$PAK/bin/${PLATFORM:-tg5040}/netplay-cable.elf}"
cable_runnable=0
if [ -x "$CABLE_BIN" ] && "$CABLE_BIN" --help > "$ROOT/usage.log" 2>&1; then
	cable_runnable=1
fi
if [ "$cable_runnable" -eq 0 ]; then
	skip "no netplay-cable.elf this machine can run ($(uname -s)/$(uname -m))"
	skip "the gadget, URB and role halves are the device checklist in docs/cable.md"
else
	FAKE="$ROOT/fake"
	FAKE_STATE="$ROOT/SD/.userdata/shared/Netplay"
	mkdir -p "$FAKE_STATE" "$FAKE/config/usb_gadget" "$FAKE/udc/sunxi-udc" \
	         "$FAKE/ffs" "$FAKE/tun" "$FAKE/role"
	rm -f "$FAKE_STATE/cable.pid" "$FAKE_STATE/cable.status"

	cable() {  # cable <args...>
		NP_CABLE_CONFIGFS="$FAKE/config" NP_CABLE_FFS_DIR="$FAKE/ffs" \
		NP_CABLE_UDC_DIR="$FAKE/udc" NP_CABLE_TUN="$FAKE/tun" \
		NP_CABLE_ROLE_DIR="$FAKE/role" \
		SDCARD_PATH="$ROOT/SD" PLATFORM=tg5040 "$CABLE_BIN" "$@"
	}

	if cable --role bogus > "$ROOT/bogus.log" 2>&1; then rc=0; else rc=$?; fi
	check "an unknown role is refused with a usage error" "$rc" "2"
	grep -q "unknown role 'bogus'" "$ROOT/bogus.log" \
		&& ok "and it names the role it refused" || bad "the refusal did not name the role"

	if cable > "$ROOT/norole.log" 2>&1; then rc=0; else rc=$?; fi
	check "a missing role prints the usage and exits 2" "$rc" "2"
	grep -q 'usage: netplay-cable --role gadget|host' "$ROOT/norole.log" \
		&& ok "the usage names both roles" || bad "the usage does not name the role argument"

	# How far a fake tree lets the gadget role get depends on how good the tree
	# is, which is the device's business rather than this machine's. Ask about the
	# lifetime only while it is still up.
	cable --role gadget > "$ROOT/gadget.log" 2>&1 &
	pid=$!
	sleep 2
	if kill -0 "$pid" 2>/dev/null; then
		[ -s "$FAKE_STATE/cable.pid" ] \
			&& ok "the pid file is the single-instance lock" || bad "no pid file was taken"
		[ -s "$FAKE_STATE/cable.status" ] \
			&& ok "the status file was published before anything was attempted" \
			|| bad "no status file was published"
		grep -q '^state=' "$FAKE_STATE/cable.status" \
			&& ok "and it carries a state the app can read" \
			|| bad "the status file carries no state"
		if cable --role gadget > "$ROOT/second.log" 2>&1; then rc=0; else rc=$?; fi
		[ "$rc" != "0" ] \
			&& ok "a second daemon is refused the session" || bad "a second daemon was allowed to start"
		kill -TERM "$pid" 2>/dev/null
		i=0; while kill -0 "$pid" 2>/dev/null && [ "$i" -lt 40 ]; do sleep 0.1; i=$((i + 1)); done
		[ -e "$FAKE_STATE/cable.pid" ] \
			&& bad "a daemon that stopped left its pid file behind" \
			|| ok "a stopped daemon removes its pid file"
		[ -e "$FAKE_STATE/cable.status" ] \
			&& bad "a daemon that stopped left its status file behind" \
			|| ok "a stopped daemon removes its status file"
	else
		skip "the daemon left before its lifetime could be asked ($(sed -n '1p' "$ROOT/gadget.log"))"
	fi
	wait "$pid" 2>/dev/null
	rm -f "$FAKE_STATE/cable.pid" "$FAKE_STATE/cable.status"
fi

###########################################################################
echo
[ "$fail" -eq 0 ] && echo "PASS" || echo "FAIL"
exit $fail
