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
if [ "$fail" -eq 0 ]; then
	echo "PASS"
else
	echo "FAIL"
	echo "--- trace ---"
	cat "$OUT/trace"
fi
exit $fail
