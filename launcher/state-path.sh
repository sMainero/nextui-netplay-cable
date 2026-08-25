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
