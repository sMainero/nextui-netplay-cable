#!/bin/sh
# Two Gambatte-shaped cores per process. This verifies the instanced control
# plane without requiring a copyrighted ROM: each device bootstraps its hidden
# instance from the peer, then both local cores consume delayed network input.

set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
OUT=$(mktemp -d)
PORT=${PORT:-$((42000 + ($$ % 18000)))}
cleanup() {
	kill "$HOST_PID" "$CLIENT_PID" 2>/dev/null || true
	if [ -n "$KEEP_TEST_OUTPUT" ]; then echo "test logs kept at $OUT"; else rm -rf "$OUT"; fi
}
trap cleanup EXIT

(cd .. && make native >/dev/null)
$CC fake_core.c -o "$OUT/gambatte_libretro.so" -shared -fPIC -I../include -O0 -std=gnu99 -DFAKE_DUAL
$CC harness.c -o "$OUT/harness" -I../include -O0 -std=gnu99 -ldl
SHIM=../../bin/native/netplay_shim.so

printf 'role=host\nport=%s\nmode=link\ninput_delay=3\ninstanced_gambatte=1\n' "$PORT" > "$OUT/host.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\nmode=link\ninput_delay=3\ninstanced_gambatte=1\n' "$PORT" > "$OUT/client.session"

FAKE_CORE_NAME=Gambatte FAKE_CORE_NO_NETPACKET=1 HARNESS_EXPECT_CORE_NAME=Gambatte \
	FAKE_SRAM_BYTE=17 HARNESS_BUTTONS=11 NETPLAY_REAL_CORE="$OUT/gambatte_libretro.so" \
	HARNESS_RESET_AT=120 \
	NETPLAY_SESSION="$OUT/host.session" "$OUT/harness" "$SHIM" 180 5 > "$OUT/host.log" 2>&1 &
HOST_PID=$!
sleep 0.3
FAKE_CORE_NAME=Gambatte FAKE_CORE_NO_NETPACKET=1 HARNESS_EXPECT_CORE_NAME=Gambatte \
	FAKE_SRAM_BYTE=34 HARNESS_BUTTONS=22 NETPLAY_REAL_CORE="$OUT/gambatte_libretro.so" \
	NETPLAY_SESSION="$OUT/client.session" "$OUT/harness" "$SHIM" 180 5 > "$OUT/client.log" 2>&1 &
CLIENT_PID=$!

HRC=0; wait "$HOST_PID" || HRC=$?
CRC=0; wait "$CLIENT_PID" || CRC=$?
fail=0
expect() {
	if grep -q "$2" "$1"; then echo "  ok   $3"; else echo "  MISS $3"; fail=1; fi
}

echo "== dual-instance Gambatte"
expect "$OUT/host.log"   "dual ABI v1 enabled (console A visible)" "host selected ABI console A"
expect "$OUT/client.log" "dual ABI v1 enabled (console B visible)" "client selected ABI console B"
expect "$OUT/host.log"   "sent console A save memory to peer" "host exported its local save memory"
expect "$OUT/client.log" "sent console B save memory to peer" "client exported its local save memory"
expect "$OUT/host.log"   "console B save memory adopted" "host adopted client save memory"
expect "$OUT/client.log" "console A save memory adopted" "client adopted host save memory"
expect "$OUT/host.log"   "sent authoritative paired checkpoint" "host established authoritative paired state"
expect "$OUT/client.log" "adopted authoritative paired checkpoint" "client adopted authoritative paired state"
expect "$OUT/host.log"   "applied synchronized reset to console A" "host applied its logical-console reset"
expect "$OUT/client.log" "applied synchronized peer reset to console A" "client mirrored the host console reset"
expect "$OUT/host.log"   "serial is local and Wi-Fi is input-only" "host completed bootstrap"
expect "$OUT/client.log" "serial is local and Wi-Fi is input-only" "client completed bootstrap"
expect "$OUT/host.log"   "core:ports p0=11 p1=22" "host mapped A/B inputs into the paired core"
expect "$OUT/client.log" "core:ports p0=11 p1=22" "client mapped the same A/B inputs into its replica"
host_runs=$(grep -c '^core:run$' "$OUT/host.log" || true)
client_runs=$(grep -c '^core:run$' "$OUT/client.log" || true)
[ "$host_runs" -ge 90 ] && [ "$host_runs" -lt 180 ] && echo "  ok   host advanced one paired core ($host_runs calls)" \
	|| { echo "  MISS host made $host_runs paired-core calls"; fail=1; }
[ "$client_runs" -ge 90 ] && [ "$client_runs" -lt 180 ] && echo "  ok   client advanced one paired core ($client_runs calls)" \
	|| { echo "  MISS client made $client_runs paired-core calls"; fail=1; }
[ "$HRC" -eq 0 ] || { echo "  MISS host exited $HRC"; fail=1; }
[ "$CRC" -eq 0 ] || { echo "  MISS client exited $CRC"; fail=1; }

if [ "$fail" -ne 0 ]; then
	echo "--- host ---"; tail -n 100 "$OUT/host.log"
	echo "--- client ---"; tail -n 100 "$OUT/client.log"
fi
exit "$fail"
