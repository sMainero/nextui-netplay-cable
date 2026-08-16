#!/bin/sh

EMU_EXE=gambatte_dual
CORES_PATH=$(dirname "$0")

###############################

EMU_TAG=$(basename "$(dirname "$0")" .pak)
ROM="$1"
mkdir -p "$BIOS_PATH/$EMU_TAG"
mkdir -p "$SAVES_PATH/$EMU_TAG"
mkdir -p "$CHEATS_PATH/$EMU_TAG"
mkdir -p "$LOGS_PATH"
export HOME="$USERDATA_PATH"

# Standalone performance test: NextUI supplies one physical controller, so
# mirror it to both emulated Game Boys. L2 restricts input to console A and R2
# to console B. The production path supplies local/remote ports instead.
export GBLC_MIRROR_INPUT=1

cd "$HOME" || exit 1

LOG_FILE="$LOGS_PATH/$EMU_TAG.txt"
if ( : > "$LOG_FILE" ) 2>/dev/null; then
	exec > "$LOG_FILE" 2>&1
else
	exec > "/tmp/nextui-$EMU_TAG.txt" 2>&1
fi

echo "launcher: system=$EMU_TAG core=$EMU_EXE rom=$ROM input=mirrored"
exec minarch.elf "$CORES_PATH/${EMU_EXE}_libretro.so" "$ROM"
