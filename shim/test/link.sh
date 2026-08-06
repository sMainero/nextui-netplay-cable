#!/bin/sh
# Two shim instances over loopback, one host one client, each with a core that
# uses the netpacket interface. Asserts the interface is accepted, the session
# starts on both sides with the right client ids, real packets cross in both
# directions, and teardown reaches the core.
#
#   ./link.sh
#
# Host-only. No device, no NextUI, no frontend that knows netplay exists - which
# is the point: a stock minarch would answer SET_NETPACKET_INTERFACE with false
# and the core would disable link entirely.

set -e
cd "$(dirname "$0")"

CC="${CC:-cc}"
OUT=$(mktemp -d)
PORT=${PORT:-45437}
trap 'kill $HOST_PID $CLIENT_PID 2>/dev/null || true; rm -rf "$OUT"' EXIT

echo "== building"
(cd .. && make native >/dev/null)
$CC fake_core.c -o "$OUT/fake_libretro.so" -shared -fPIC -I../include -O0 -std=gnu99
$CC harness.c   -o "$OUT/harness"          -I../include -O0 -std=gnu99 -ldl

SHIM=../../bin/native/netplay_shim.so

printf 'role=host\nport=%s\noption.gpsp_serial=mul_aw2\n' "$PORT" > "$OUT/host.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\n' "$PORT" > "$OUT/client.session"

echo "== running host and client for ~3s"
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/host.session" \
	"$OUT/harness" "$SHIM" 200 15 > "$OUT/host.log" 2>&1 &
HOST_PID=$!

sleep 0.5   # let the host bind before the client dials

NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/client.session" \
	"$OUT/harness" "$SHIM" 200 15 > "$OUT/client.log" 2>&1 &
CLIENT_PID=$!

HOST_RC=0;   wait $HOST_PID   || HOST_RC=$?
CLIENT_RC=0; wait $CLIENT_PID || CLIENT_RC=$?

fail=0
expect() { # <file> <pattern> <description>
	if grep -q "$2" "$1"; then
		echo "  ok   $3"
	else
		echo "  MISS $3"
		fail=1
	fi
}

echo
echo "== interface"
expect "$OUT/host.log"   "core:netpacket_accepted" "host: core's interface accepted"
expect "$OUT/client.log" "core:netpacket_accepted" "client: core's interface accepted"

echo
echo "== session"
# libretro assigns the host client_id 0; the other side must see the peer's id.
expect "$OUT/host.log"   "core:np_start id=0"      "host: started as client_id 0"
expect "$OUT/client.log" "core:np_start id=1"      "client: started as client_id 1"
expect "$OUT/host.log"   "core:np_connected id=1"  "host: told the client joined"
expect "$OUT/client.log" "core:np_connected id=0"  "client: told the host is there"

echo
echo "== data actually crossed"
# The client sends P1-*, so seeing it in the host's log means it went out over
# TCP, through the shim, and into the peer core's receive callback.
expect "$OUT/host.log"   "core:np_recv from=1 .*data=P1-" "host received the client's packets"
expect "$OUT/client.log" "core:np_recv from=0 .*data=P0-" "client received the host's packets"

HOST_RX=$(grep -c "core:np_recv" "$OUT/host.log" || true)
CLIENT_RX=$(grep -c "core:np_recv" "$OUT/client.log" || true)
echo "  ..   host received $HOST_RX packets, client received $CLIENT_RX"
[ "$HOST_RX" -gt 10 ] && [ "$CLIENT_RX" -gt 10 ] \
	&& echo "  ok   sustained both directions" \
	|| { echo "  MISS expected a sustained stream"; fail=1; }

echo
echo "== core option forced by the session"
# gpSP link modes are per-game; both peers must agree, and the frontend's own
# value (typically "auto") must not win.
expect "$OUT/host.log"   "core:option gpsp_serial=mul_aw2" "host: shim answered the option"
expect "$OUT/client.log" "core:option gpsp_serial=<unset>" "client: no override, frontend answers"

echo
echo "== teardown"
expect "$OUT/host.log"   "core:np_stop" "host: core told the session ended"
expect "$OUT/client.log" "core:np_stop" "client: core told the session ended"

echo
echo "== no packets dropped"
if grep -q "were dropped" "$OUT/host.log" "$OUT/client.log"; then
	grep -h "were dropped" "$OUT/host.log" "$OUT/client.log" | sed 's/^/  WARN /'
	fail=1
else
	echo "  ok   queue never overflowed"
fi

echo
echo "== harness assertions still pass under a live link"
[ "$HOST_RC" -eq 0 ] && echo "  ok   host exited 0"   || { echo "  MISS host rc=$HOST_RC"; fail=1; }
[ "$CLIENT_RC" -eq 0 ] && echo "  ok   client exited 0" || { echo "  MISS client rc=$CLIENT_RC"; fail=1; }

echo
echo "== a stalled frontend must pause its peer, not overflow it"
# The menu case seen on device: one side stops calling retro_run, the other keeps
# running and fills a queue nobody drains. The peer should be told to hold.
PORT3=$((PORT + 2))
printf 'role=host\nport=%s\n'                  "$PORT3" > "$OUT/h3.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\n' "$PORT3" > "$OUT/c3.session"

NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h3.session" \
	"$OUT/harness" "$SHIM" 600 15 > "$OUT/h3.log" 2>&1 &
H3=$!
sleep 0.5
HARNESS_STALL_AT=150 HARNESS_STALL_MS=4000 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c3.session" \
	"$OUT/harness" "$SHIM" 600 15 > "$OUT/c3.log" 2>&1 &
C3=$!
wait $H3 2>/dev/null || true
wait $C3 2>/dev/null || true

expect "$OUT/c3.log" "fe:stall_begin"                  "client frontend stalled"
expect "$OUT/c3.log" "frontend stalled - told peer"    "client announced the stall"
expect "$OUT/h3.log" "peer paused"                     "host was told to hold"
expect "$OUT/h3.log" "peer resumed"                    "host was released"
expect "$OUT/c3.log" "frontend resumed - told peer"    "client announced recovery"

if grep -q "were dropped" "$OUT/h3.log" "$OUT/c3.log"; then
	grep -h "were dropped" "$OUT/h3.log" "$OUT/c3.log" | sed 's/^/  WARN /'
	fail=1
else
	echo "  ok   a 4s stall cost no packets"
fi

# The host is frame-skipping for ~4s. If the shim did not present anything, the
# screen would just freeze - visually identical to a crash.
h3_video=$(sed -n 's/.*fe:totals video=\([0-9]*\).*/\1/p' "$OUT/h3.log")
h3_audio=$(sed -n 's/.*audio=\([0-9]*\).*/\1/p' "$OUT/h3.log")
[ "${h3_video:-0}" -ge 590 ] \
	&& echo "  ok   host kept presenting while paused ($h3_video presented)" \
	|| { echo "  MISS host stopped presenting ($h3_video frames of 600)"; fail=1; }
[ "${h3_audio:-0}" -ge 590 ] \
	&& echo "  ok   host kept feeding audio while paused ($h3_audio batches)" \
	|| { echo "  MISS audio gap while paused ($h3_audio batches of 600)"; fail=1; }

h3_input=$(sed -n 's/.*input=\([0-9]*\).*/\1/p' "$OUT/h3.log")
# Input is polled only from inside core.run(). Skipping it without polling
# manually leaves the frontend unable to see any button, including MENU.
[ "${h3_input:-0}" -ge 590 ] \
	&& echo "  ok   host kept polling input while paused ($h3_input polls)" \
	|| { echo "  MISS input unpolled while paused ($h3_input of 600) - UI would be frozen"; fail=1; }

echo
echo "== a peer that quits while paused must not strand us"
# The device bug: peer opens a menu (we pause), then quits. CMD_RESUME never
# comes, so we waited forever with a dead UI.
PORT4=$((PORT + 3))
printf 'role=host\nport=%s\n'                  "$PORT4" > "$OUT/h4.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\n' "$PORT4" > "$OUT/c4.session"

NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h4.session" \
	"$OUT/harness" "$SHIM" 700 15 > "$OUT/h4.log" 2>&1 &
H4=$!
sleep 0.5
# Client stalls, then exits while still stalled - never sending CMD_RESUME.
HARNESS_STALL_AT=100 HARNESS_STALL_MS=1500 HARNESS_DIE_IN_STALL=1 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c4.session" \
	"$OUT/harness" "$SHIM" 700 15 > "$OUT/c4.log" 2>&1 &
C4=$!
wait $C4 2>/dev/null || true
wait $H4 2>/dev/null || true

expect "$OUT/c4.log" "fe:died_in_stall" "client quit while still paused"
expect "$OUT/h4.log" "peer paused"       "host paused as expected"
h4_runs=$(grep -c "^core:run$" "$OUT/h4.log" || true)
# Count core runs, not presented frames: the shim presents held frames while
# paused, so video would reach 700 even if the core never advanced again.
# Allow for the ~100 frames legitimately paused before the peer vanished; a
# stranded host stops near 150 and never recovers.
[ "${h4_runs:-0}" -ge 550 ] \
	&& echo "  ok   host resumed after the peer vanished ($h4_runs core runs)" \
	|| { echo "  MISS host stranded at $h4_runs of 700 core runs"; fail=1; }
grep -q "RESULT: ok" "$OUT/h4.log" \
	&& echo "  ok   host completed its run" || { echo "  MISS host never finished"; fail=1; }

echo
echo "== a core blocked inside retro_run is not a stalled frontend"
# The GB link deadlock: gambatte blocks in retro_run waiting on its peer. Read
# as a frontend stall, that pauses the peer - removing the data the blocked core
# is waiting for, so neither side can proceed.
PORT5=$((PORT + 4))
printf 'role=host\nport=%s\n'                  "$PORT5" > "$OUT/h5.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\n' "$PORT5" > "$OUT/c5.session"

NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h5.session" \
	"$OUT/harness" "$SHIM" 500 15 > "$OUT/h5.log" 2>&1 &
H5=$!
sleep 0.5
FAKE_CORE_BLOCK_AT=100 FAKE_CORE_BLOCK_MS=3000 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c5.session" \
	"$OUT/harness" "$SHIM" 500 15 > "$OUT/c5.log" 2>&1 &
C5=$!
wait $H5 2>/dev/null || true
wait $C5 2>/dev/null || true

expect "$OUT/c5.log" "core:block_begin" "client core blocked inside retro_run"
if grep -q "frontend stalled" "$OUT/c5.log"; then
	echo "  MISS a blocked core was reported as a frontend stall"
	fail=1
else
	echo "  ok   blocked core not mistaken for a stall"
fi
if grep -q "peer paused" "$OUT/h5.log"; then
	echo "  MISS host paused while its peer was mid-frame"
	fail=1
else
	echo "  ok   host never paused"
fi

echo
echo "== menu still pauses the peer for a core that owns its own link"
# gambatte never registers netpacket, but a menu on one device should still hold
# the other - otherwise it sits blocked on a serial read with no idea why.
PORT6=$((PORT + 5))
printf 'role=host\nport=%s\n'                  "$PORT6" > "$OUT/h6.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\n' "$PORT6" > "$OUT/c6.session"

FAKE_CORE_NO_NETPACKET=1 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h6.session" \
	"$OUT/harness" "$SHIM" 500 15 > "$OUT/h6.log" 2>&1 &
H6=$!
sleep 0.5
FAKE_CORE_NO_NETPACKET=1 HARNESS_STALL_AT=120 HARNESS_STALL_MS=2500 \
	NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c6.session" \
	"$OUT/harness" "$SHIM" 500 15 > "$OUT/c6.log" 2>&1 &
C6=$!
wait $H6 2>/dev/null || true
wait $C6 2>/dev/null || true

expect "$OUT/c6.log" "core:netpacket_skipped"      "core carries its own link"
expect "$OUT/c6.log" "frontend stalled - told peer" "menu still announced"
expect "$OUT/h6.log" "peer paused: frontend"        "peer paused, with a reason"
expect "$OUT/h6.log" "peer resumed"                 "peer released"

echo
echo "== one-sided traffic must not starve the heartbeat"
# The regression that shipped: the heartbeat lived in the poll()-timeout branch,
# so a peer sending steadily kept POLLIN set, the idle branch never ran, and the
# quiet side got timed out mid-session. Both sides sending every frame - as the
# test above does - hides it completely.
PORT2=$((PORT + 1))
printf 'role=host\nport=%s\n'                  "$PORT2" > "$OUT/h2.session"
printf 'role=client\nport=%s\npeer=127.0.0.1\n' "$PORT2" > "$OUT/c2.session"

# Host talks constantly; client stays silent for well over the 5s timeout.
NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/h2.session" \
	"$OUT/harness" "$SHIM" 700 15 > "$OUT/h2.log" 2>&1 &
H2=$!
sleep 0.5
FAKE_CORE_QUIET=1 NETPLAY_REAL_CORE="$OUT/fake_libretro.so" NETPLAY_SESSION="$OUT/c2.session" \
	"$OUT/harness" "$SHIM" 700 15 > "$OUT/c2.log" 2>&1 &
C2=$!
wait $H2 2>/dev/null || true
wait $C2 2>/dev/null || true

for side in h2 c2; do
	if grep -q "silence timeout" "$OUT/$side.log"; then
		echo "  MISS $side timed out despite an active peer"
		grep "peer lost" "$OUT/$side.log" | sed 's/^/       /'
		fail=1
	else
		echo "  ok   $side stayed connected"
	fi
done
# One clean teardown each at the end is expected; more means it flapped.
flaps=$(grep -c "netpacket session started" "$OUT/c2.log" || true)
[ "$flaps" -eq 1 ] && echo "  ok   session never flapped" \
	|| { echo "  MISS session restarted $flaps times"; fail=1; }

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS"
else
	echo "FAIL"
	echo "--- host ---";   tail -25 "$OUT/host.log"
	echo "--- client ---"; tail -25 "$OUT/client.log"
fi
exit $fail
