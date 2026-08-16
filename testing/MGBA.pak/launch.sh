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

echo "launcher: system=$EMU_TAG core=$EMU_EXE rom=$ROM"
exec minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM"
