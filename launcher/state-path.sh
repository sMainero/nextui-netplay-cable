#!/bin/sh
# Shared by launcher helpers. Persistent and recovery state belongs outside the
# replaceable pak directory so an update cannot erase preferences or strand a
# device mid-session.
: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${NETPLAY_PAK:=${NP:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}}"
: "${NETPLAY_STATE:=$SDCARD_PATH/.userdata/shared/Netplay}"
mkdir -p "$NETPLAY_STATE"

# One-time compatibility with builds that stored everything below the pak.
if [ -n "$NETPLAY_PAK" ] && [ -d "$NETPLAY_PAK/state" ]; then
	for _netplay_old in "$NETPLAY_PAK/state"/*; do
		[ -e "$_netplay_old" ] || continue
		if [ "$(basename "$_netplay_old")" = harness-backup ]; then
			mkdir -p "$NETPLAY_STATE/netplay-harness"
			_netplay_new="$NETPLAY_STATE/netplay-harness/session-backups"
		else
			_netplay_new="$NETPLAY_STATE/$(basename "$_netplay_old")"
		fi
		[ -e "$_netplay_new" ] || mv "$_netplay_old" "$_netplay_new" 2>/dev/null || :
	done
fi
if [ -n "$NETPLAY_PAK" ] && [ -f "$NETPLAY_PAK/session.conf" ] &&
   [ ! -e "$NETPLAY_STATE/session.conf" ]; then
	mv "$NETPLAY_PAK/session.conf" "$NETPLAY_STATE/session.conf" 2>/dev/null || :
fi
export NETPLAY_STATE

# The medium an armed session names, as one of the three tokens the app writes.
#
# The session file is authoritative; the fallback is for a session armed by a
# build that predates the link= line. That build recorded adhoc_ssid for exactly
# the sessions that had moved the radio - a host only wrote it while serving, a
# client only after joining - so its presence answers the same question. An
# unrecognised value falls through to the same place, matching the app.
#
# Echoes "none" when there is no session at all, so a caller can tell "not
# armed" from "armed and not a cable link".
netplay_link_kind() {
	[ -f "${1:-}" ] || { echo none; return 0; }
	_netplay_link=$(sed -n 's/^link=//p' "$1" 2>/dev/null | head -1)
	case "$_netplay_link" in
	cable|adhoc|wifi) echo "$_netplay_link"; return 0 ;;
	esac
	if grep -q '^adhoc_ssid=.' "$1" 2>/dev/null; then echo adhoc; else echo wifi; fi
}

# Remove a session file and its bookkeeping, then idle Game Switcher.
# Shared by session-cleanup.sh and wifi-watchdog.sh so the set of files that
# make up "session ended" state cannot go out of sync between the two.
#
# The cable daemon's pid and status files join that set for the same reason: one
# left behind makes the next session believe a daemon is already running. Its
# usb_restore breadcrumb deliberately does NOT, and neither does its log: the
# breadcrumb is the record of an owed repair and is removed only once that
# repair succeeds - the same policy as wifi_restore - and the log is not session
# state, exactly as broker.log is not.
netplay_end_session() {
	rm -f "$1" "$NETPLAY_STATE/broker.pid" "$NETPLAY_STATE/broker.status" \
	      "$NETPLAY_STATE/cable.pid" "$NETPLAY_STATE/cable.status"
	if [ -x "$NETPLAY_PAK/launcher/gameswitcher.sh" ]; then
		SDCARD_PATH="${SDCARD_PATH:-/mnt/SDCARD}" PLATFORM="${PLATFORM:-tg5040}" \
			NETPLAY_PAK="$NETPLAY_PAK" \
			sh "$NETPLAY_PAK/launcher/gameswitcher.sh" idle >/dev/null 2>&1
	fi
}
