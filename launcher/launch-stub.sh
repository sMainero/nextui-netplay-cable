#!/bin/sh
#
# Installed by Netplay.pak as /mnt/SDCARD/Emus/<PLATFORM>/<TAG>.pak/launch.sh
#
# getEmuPath (utils.c) checks the SD card before the system paks, so this file
# takes precedence over $SYSTEM_PATH/paks/Emus/<TAG>.pak/launch.sh. All it does
# is put our minarch.elf shadow on PATH and hand off to the original, which is
# left completely untouched.
#
# Identical for every system - the tag comes from our own path - so the
# installer writes the same bytes to each one.
#
# This only works for paks that live in $SYSTEM_PATH. An EXTRAS pak already
# occupies the SD path, so there is nothing to override; those are covered by
# the bind-mount route instead (see bind-mount.sh). We do not modify paks we
# do not own.

DIR="$(dirname "$0")"
TAG="$(basename "$DIR" .pak)"

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"

ORIGINAL="$SYSTEM_PATH/paks/Emus/$TAG.pak/launch.sh"

if [ ! -f "$ORIGINAL" ]; then
	echo "[netplay] no original pak for $TAG at $ORIGINAL" >&2
	exit 1
fi

# The original recomputes EMU_TAG and CORES_PATH from its own $0, so running it
# at its real location keeps every path it derives correct.
PATH="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak/launcher:$PATH"
export PATH

exec "$ORIGINAL" "$@"
