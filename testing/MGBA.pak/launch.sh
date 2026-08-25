#!/bin/sh
#
# Baseline mGBA on my282, standalone.
#
# NextUI builds mgba for my282 but does not ship it on the card, so there is no
# way to run it without adding one. This pak carries its own copy and launches
# it directly, which keeps the measurement honest: nothing here touches the
# system cores or the GBA pak, and removing the directory removes the change.
#
# The point is a number. What does one mGBA instance cost on this device, for
# GBA and for GB/GBC content? Instanced link play needs two consoles inside one
# 16.7ms frame, so a baseline much above 8ms per frame settles the question
# before any wrapper is written.

EMU_EXE=mgba
CORES_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
mkdir -p "$CHEATS_PATH/$EMU_TAG"
mkdir -p "$LOGS_PATH"
export HOME="$USERDATA_PATH"

cd "$HOME" || exit 1

LOG_FILE="$LOGS_PATH/$EMU_TAG.txt"
if ( : > "$LOG_FILE" ) 2>/dev/null; then
	exec > "$LOG_FILE" 2>&1
else
	exec > "/tmp/nextui-$EMU_TAG.txt" 2>&1
fi

CORE="$CORES_PATH/${EMU_EXE}_libretro.so"
SESSION=${NETPLAY_SESSION:-"$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak/state/session"}
if [ -f "$SESSION" ]; then
	NETPLAY_SESSION="$SESSION"
	export NETPLAY_SESSION
else
	unset NETPLAY_SESSION
fi
if [ "${NETPLAY_DUAL_DISABLE:-0}" != 1 ] &&
	[ -f "$CORES_PATH/mgba_dual_libretro.so" ] &&
	grep -q '^instanced_mgba=1$' "$SESSION" 2>/dev/null; then
	CORE="$CORES_PATH/mgba_dual_libretro.so"
	echo "launcher: paired mGBA selected by $SESSION"
fi

# Route through the netplay shim when one is staged for this platform.
#
# The shim can answer mGBA's request for a target audio sample rate, which
# minarch never answers. Measured on an A30: declining (mGBA's own 65536) costs
# 12.63 ms/frame and forcing 32768 costs 14.64, because 32768 is the GBA's reset
# rate rather than what games run at once they write SOUNDBIAS - so forcing it
# switches mGBA's resampler on rather than off. Default is to decline.
#
# The knob remains for re-measuring. It only takes effect at launch:
#
#   echo 32768 > /mnt/SDCARD/Emus/$PLATFORM/MGBA.pak/sample_rate.txt
#   rm           /mnt/SDCARD/Emus/$PLATFORM/MGBA.pak/sample_rate.txt   (default)
SHIM="$CORES_PATH/netplay_shim.$PLATFORM.so"
if [ -f "$SHIM" ]; then
	if [ -f "$CORES_PATH/sample_rate.txt" ]; then
		NETPLAY_MGBA_SAMPLE_RATE=$(cat "$CORES_PATH/sample_rate.txt")
		export NETPLAY_MGBA_SAMPLE_RATE
	fi
	export NETPLAY_REAL_CORE="$CORE"
	echo "launcher: shim=$SHIM real=$CORE rate=${NETPLAY_MGBA_SAMPLE_RATE:-default}"
	CORE="$SHIM"
else
	echo "launcher: no shim for $PLATFORM, running the core directly"
fi

echo "launcher: system=$EMU_TAG core=$EMU_EXE rom=$ROM"
exec minarch.elf "$CORE" "$ROM"
