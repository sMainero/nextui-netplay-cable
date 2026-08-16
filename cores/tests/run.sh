#!/bin/sh
# Determinism test for the paired core's in-process serial cable.
#
# The bus decides what byte an emulated console receives and whether it receives
# one at all, so its answers are emulated state. Two devices mirroring the same
# pair must get identical answers for the same sequence of emulated events, and
# for a long time they did not: one on-device session recorded 385 exchanges on
# one handheld and 0 on the other over the same window.
#
# This drives the bus directly with a fixed script under deliberately hostile
# timing and asserts the whole sequence of answers is reproducible. No ROM, no
# device, no frontend - which is the point, because the failure it catches was
# otherwise only reachable by getting two handhelds into a linking game.
#
# Needs the pinned core sources. `make core-sources` fetches them; without them
# this skips rather than failing, so a checkout with no .cache still runs the
# rest of the suite.
set -e
cd "$(dirname "$0")/../.."

CORE=.cache/cores/gambatte
if [ ! -f "$CORE/libgambatte/libretro/local_serial.cpp" ]; then
	echo "== paired serial bus and cartridge clocks"
	echo "  SKIP core sources not fetched (run 'make core-sources')"
	exit 0
fi

CXX="${CXX:-c++}"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

echo "== paired serial bus and cartridge clocks"
$CXX -g -O1 -pthread cores/tests/bustest.cpp \
	"$CORE/libgambatte/libretro/local_serial.cpp" \
	-I"$CORE/libgambatte/libretro" -I"$CORE/libgambatte/src" \
	-o "$OUT/bustest"

# Under load, deliberately. The two console threads have to be preempted at
# awkward moments for a timing-dependent answer to show itself, and an idle
# desktop simply will not do it: the frame-barrier bug passed 20 of 20 runs
# unloaded and failed 34 of 60 with eight competing spinners. A handheld running
# two Game Boys on four small cores is the loaded case, so test the loaded case.
LOAD="${BUSTEST_LOAD:-8}"
SPINNERS=""
i=0
while [ "$i" -lt "$LOAD" ]; do
	sh -c 'while :; do :; done' & SPINNERS="$SPINNERS $!"
	i=$((i + 1))
done
# shellcheck disable=SC2086
trap 'kill $SPINNERS 2>/dev/null; rm -rf "$OUT"' EXIT

RUNS="${BUSTEST_RUNS:-12}"
if "$OUT/bustest" "$RUNS" > "$OUT/log" 2>&1; then
	echo "  ok   $RUNS runs of each scenario answered identically, under $LOAD-way load"
	grep "exchanges=" "$OUT/log" | sed 's/^  run *[0-9]*: /  ok   /' | sort -u
else
	echo "  MISS the bus answered differently across runs"
	tail -20 "$OUT/log"
	exit 1
fi

# Cartridge clocks. A paired RTC game turns "what time is it" into emulated
# state, so the arithmetic has to be a function of emulated progress. It is
# small, it is easy to get wrong, and getting it wrong is not subtle: an
# underflowed elapsed time sends Rtc::doLatch into a loop that normalises in
# 44-day steps, which freezes the game on the first latch rather than merely
# showing it the wrong date. Hence the timeout - a regression here hangs.
$CXX -g -O1 cores/tests/clocktest.cpp \
	"$CORE/libgambatte/src/mem/rtc.cpp" \
	-I"$CORE/libgambatte/src" -I"$CORE/libgambatte/include" \
	-o "$OUT/clocktest"

set +e
timeout 60 "$OUT/clocktest" > "$OUT/clocklog" 2>&1
clock_rc=$?
set -e

if [ "$clock_rc" -eq 0 ]; then
	grep '^  ok' "$OUT/clocklog"
	exit 0
fi

if [ "$clock_rc" -eq 124 ]; then
	echo "  MISS the cartridge clock hung - elapsed time has probably underflowed"
else
	echo "  MISS the cartridge clock is not a function of emulated progress"
fi
cat "$OUT/clocklog"
exit 1
