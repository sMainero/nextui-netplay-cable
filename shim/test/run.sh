#!/bin/sh
# Host-side check of the shim's forwarding contract. Builds a fake core and a
# harness that loads the shim the way minarch does, then asserts on both the
# call trace and the values that come back through.
#
#   ./run.sh
#
# Needs only a host C compiler; nothing here is device- or NextUI-specific.

set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "== building"
(cd .. && make native >/dev/null)
$CC fake_core.c -o "$OUT/fake_libretro.so" -shared -fPIC -I../include -O0 -std=gnu99
$CC harness.c   -o "$OUT/harness"          -I../include -O0 -std=gnu99 -ldl

SHIM=../../bin/native/netplay_shim.so

echo "== passthrough"
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" "$OUT/harness" "$SHIM" > "$OUT/trace" 2>&1 || {
	cat "$OUT/trace"; echo "FAIL: harness exited non-zero"; exit 1
}

fail=0
expect() {
	if grep -qxF "$1" "$OUT/trace"; then
		echo "  ok   $1"
	else
		echo "  MISS $1"
		fail=1
	fi
}

# Reached the real core.
expect "core:get_system_info"
expect "core:init"
expect "core:load_game"
expect "core:run"
expect "core:unload_game"
expect "core:deinit"

# Core -> frontend, through the shim's wrappers.
expect "fe:input_poll"
expect "fe:video_refresh 240x160"
expect "fe:audio_sample_batch 32"
expect "fe:environment cmd=3"   # RETRO_ENVIRONMENT_GET_CAN_DUPE

# The frontend's input value survived the round trip.
expect "core:input_state=42"

# With no session armed the shim must not claim netpacket support it cannot
# back: the request forwards to the frontend, which refuses, leaving the core
# exactly as it would be without the shim.
expect "core:netpacket_refused"

grep -q "RESULT: ok" "$OUT/trace" || { echo "  MISS assertions passed"; fail=1; }

echo
echo "== an active session hides frontend save-state support"
printf 'role=host\nport=55991\nmode=link\n' > "$OUT/session"
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/session" \
	HARNESS_EXPECT_NO_STATES=1 "$OUT/harness" "$SHIM" > "$OUT/session-state" 2>&1 || {
	cat "$OUT/session-state"; echo "  MISS active-session state policy"; fail=1
}
grep -q "frontend save-state load blocked during session" "$OUT/session-state" \
	&& grep -q "RESULT: ok" "$OUT/session-state" \
	&& echo "  ok   save, load, auto-resume and autosave are unavailable" \
	|| { echo "  MISS active-session state policy"; fail=1; }

echo
echo "== selected core outcome is shown over live startup frames"
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/session" \
	NETPLAY_CORE_NOTICE=compatibility HARNESS_EXPECT_OVERLAY=1 \
	"$OUT/harness" "$SHIM" 2 > "$OUT/startup-notice" 2>&1 || {
	cat "$OUT/startup-notice"; echo "  MISS compatibility startup notice"; fail=1
}
grep -q "startup notice: Starting with compatibility core" "$OUT/startup-notice" \
	&& grep -q "RESULT: ok" "$OUT/startup-notice" \
	&& echo "  ok   compatibility notice is drawn while the core runs" \
	|| { echo "  MISS compatibility startup notice"; fail=1; }
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/session" \
	NETPLAY_CORE_NOTICE=mismatch HARNESS_EXPECT_OVERLAY=1 \
	"$OUT/harness" "$SHIM" 2 > "$OUT/mismatch-notice" 2>&1 || {
	cat "$OUT/mismatch-notice"; echo "  MISS core mismatch startup notice"; fail=1
}
grep -q "startup notice: Core builds differ. Desyncs may occur" "$OUT/mismatch-notice" \
	&& grep -q "RESULT: ok" "$OUT/mismatch-notice" \
	&& echo "  ok   differing-core warning is drawn while the core runs" \
	|| { echo "  MISS core mismatch startup notice"; fail=1; }

echo
echo "== fails loudly with no real core"
if NETPLAY_REAL_CORE= "$OUT/harness" "$SHIM" >"$OUT/nocore" 2>&1; then
	echo "  MISS expected non-zero exit"
	fail=1
else
	grep -q "NETPLAY_REAL_CORE is unset" "$OUT/nocore" \
		&& echo "  ok   exits with a diagnosable message" \
		|| { echo "  MISS expected message"; cat "$OUT/nocore"; fail=1; }
fi

echo
# An armed session pins CPU scaling, because the frame-paced workload never
# looks busy enough for a conservative governor to leave its floor. The sysfs
# root is overridable so this is testable without touching the real one.
echo
echo "== an armed session pins CPU scaling and puts it back"
say() { # <condition-result> <description>
	if [ "$1" -eq 0 ]; then echo "  ok   $2"; else echo "  MISS $2"; fail=1; fi
}
CPUROOT="$OUT/cpufreq"
for n in 0 1; do
	mkdir -p "$CPUROOT/cpu$n/cpufreq"
	echo conservative > "$CPUROOT/cpu$n/cpufreq/scaling_governor"
	echo 648000       > "$CPUROOT/cpu$n/cpufreq/scaling_min_freq"
	echo 1344000      > "$CPUROOT/cpu$n/cpufreq/scaling_max_freq"
done
printf 'role=host\nport=55999\nmode=link\n' > "$OUT/cpu.session"
NETPLAY_CPUFREQ_ROOT="$CPUROOT" NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	NETPLAY_SESSION="$OUT/cpu.session" "$OUT/harness" "$SHIM" 5 > "$OUT/cpu.log" 2>&1 || true

grep -q "pinned 2 of 2 CPU policies to performance" "$OUT/cpu.log"; say $? "both policies pinned"
grep -q "restored CPU scaling to 'conservative'" "$OUT/cpu.log"; say $? "scaling restored at teardown"
[ "$(cat "$CPUROOT/cpu0/cpufreq/scaling_governor")" = "conservative" ]
say $? "governor put back the way it was"
[ "$(cat "$CPUROOT/cpu0/cpufreq/scaling_min_freq")" = "648000" ]
say $? "frequency floor put back the way it was"

# A launch with no session must not touch the governor at all.
for n in 0 1; do echo conservative > "$CPUROOT/cpu$n/cpufreq/scaling_governor"; done
NETPLAY_CPUFREQ_ROOT="$CPUROOT" NETPLAY_REAL_CORE="$OUT/fake_libretro.so" \
	"$OUT/harness" "$SHIM" 5 > "$OUT/nocpu.log" 2>&1 || true
! grep -q "pinned .* CPU policies" "$OUT/nocpu.log"
say $? "a disarmed launch leaves CPU scaling alone"

if [ "$fail" -eq 0 ]; then
	echo "PASS"
else
	echo "FAIL"
	echo "--- trace ---"
	cat "$OUT/trace"
fi
exit $fail
