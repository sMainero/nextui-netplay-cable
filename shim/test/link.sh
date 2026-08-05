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

printf 'role=host\nport=%s\n'            "$PORT"     > "$OUT/host.session"
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
if [ "$fail" -eq 0 ]; then
	echo "PASS"
else
	echo "FAIL"
	echo "--- host ---";   tail -25 "$OUT/host.log"
	echo "--- client ---"; tail -25 "$OUT/client.log"
fi
exit $fail
