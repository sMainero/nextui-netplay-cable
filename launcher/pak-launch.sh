#!/bin/sh
#
# Netplay.pak entry point. Runs the session-setup app.
#
# Deliberately does NOT run the old src/netplay.elf, which installed patched
# system binaries - the thing this architecture replaces.

DIR="$(dirname "$0")"
cd "$DIR" || exit 1

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"
: "${LOGS_PATH:=$SDCARD_PATH/.userdata/$PLATFORM/logs}"

export SDCARD_PATH PLATFORM SYSTEM_PATH
export LD_LIBRARY_PATH="$DIR:$DIR/bin:$DIR/bin/$PLATFORM:$LD_LIBRARY_PATH"
export HOME="$SDCARD_PATH/.userdata/$PLATFORM"

mkdir -p "$LOGS_PATH"
"$DIR/bin/$PLATFORM/netplay.elf" > "$LOGS_PATH/netplay.txt" 2>&1
