#!/bin/sh
# Remove only sessions that can be proven to belong to an earlier device boot.
#
# Guest presence is deliberately irrelevant. An armed host with zero guests is
# a healthy hot-seat lobby, and a guest may leave and return without shortening
# the host session. Likewise, a stopped broker is restartable within this boot.
# The kernel boot id is the conservative boundary: processes, AP ownership and
# temporary network state cannot survive it, while closing Netplay.pak can.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${NETPLAY_PAK:=$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}"
: "${NETPLAY_BOOT_ID_PATH:=/proc/sys/kernel/random/boot_id}"

NP="$NETPLAY_PAK"
. "$NETPLAY_PAK/launcher/state-path.sh"
STATE="$NETPLAY_STATE"
SESSION="$STATE/session"
[ -f "$SESSION" ] || exit 0

current=$(sed -n '1p' "$NETPLAY_BOOT_ID_PATH" 2>/dev/null)
recorded=$(sed -n 's/^boot_id=//p' "$SESSION" 2>/dev/null | head -1)

# Older sessions have no boot marker. Preserve them rather than guessing; once
# the user ends/re-arms, the replacement session receives a marker.
[ -n "$current" ] && [ -n "$recorded" ] || exit 0
[ "$current" != "$recorded" ] || exit 0

mkdir -p "$STATE"
printf '%s stale session removed (boot %s -> %s)\n' \
	"$(date '+%Y-%m-%d %H:%M:%S' 2>/dev/null)" "$recorded" "$current" \
	>> "$STATE/cleanup.log"

# The cable link's repair record goes with the radio's, and for the same reason:
# it names work this device owed the firmware in the *previous* boot - the
# controller it had taken from the firmware's gadget, and the port mode it had
# forced. A reboot undoes both by itself: configfs is a RAM filesystem, so the
# gadget directories are gone and the platform's own gadget has the controller
# again, and the port's role is a register that resets. What the record would
# hand the controller back to no longer exists, so nothing is owed and nothing
# can be done with it.
#
# The live case is deliberately not this script's. A device that is still up
# with a record it owes is repaired by NS_cableRecoverIfStranded when the app
# opens, and only a repair that worked may remove the record.
rm -f "$STATE/wifi_restore" "$STATE/wifi_restore_hook" "$STATE/wifi_powersave" \
      "$STATE/usb_restore"

# The bindings remain installed by design; without a shared session they are pure
# passthrough. Only the armed Game Switcher view is derived session state.
netplay_end_session "$SESSION"

# Distinct from failure so the UI can report that cleanup occurred.
exit 10
