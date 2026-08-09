#!/bin/sh
#
# Get back onto the normal network if the ad hoc one goes away underneath us.
#
# The gap this closes: you join a host's network, the host powers off or ends
# its session, and nothing is running that would notice. The other three
# recovery points all need something to happen first - opening the app, starting
# a game, or pressing Restore WiFi - so sitting in the system menu you are
# stranded until you go looking.
#
# Scope is deliberately narrow:
#
#   * Started when a join succeeds, not at boot. A device that never uses ad hoc
#     never runs this, so it costs nothing to have.
#   * Exits as soon as state/wifi_restore is gone. That file is written when we
#     take the client stack down and removed when we put it back, so the
#     watchdog's lifetime is exactly the window where stranding is possible.
#   * Exits if we are already back on a normal network, cleaning up the record.
#   * Never acts on one failed check. A host in a menu, or a moment of RF noise,
#     must not kick anyone off a working session.
#
# A reboot does not need covering: the temporary supplicant config lives in
# /tmp, so the platform's own init brings WiFi back normally on the next boot.

# Pak path as $1 when the caller knows it (the launch stub derives it from the
# session file); otherwise fall back to the environment. Relying on inherited
# SDCARD_PATH/PLATFORM alone was fragile - the stub does not export them.
if [ -n "$1" ] && [ -d "$1" ]; then
	NP="$1"
else
	: "${SDCARD_PATH:=/mnt/SDCARD}"
	: "${PLATFORM:=tg5040}"
	NP="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"
fi

RESTORE="$NP/state/wifi_restore"
LOCK="/tmp/netplay_watchdog.pid"
HOST=10.0.0.1
PREFIX=nextui

INTERVAL=20      # seconds between checks
FAILS_NEEDED=3   # consecutive failures before we act - so ~60s to recover
MAX_CHECKS=1440  # ~8h backstop, so a stuck record cannot leave this running

# Own our output. Callers used to redirect, which forced them to know where the
# log lives and left us tied to their descriptors.
exec >>"$NP/state/watchdog.log" 2>&1

# Same HH:MM:SS.mmm shape as the C loggers so the three logs can be read
# side by side. busybox date has no %N, so milliseconds come from
# /proc/uptime's centiseconds - close enough to order events.
log() { echo "[$(date +%H:%M:%S).$(cut -d" " -f1 /proc/uptime | cut -d. -f2)0 ] [netplay-watchdog] $*"; }

# One at a time. A second join should not start a second watchdog.
# mkdir is atomic; a test-then-write is not. Two joins in quick succession both
# passed the old check before either wrote, and two watchdogs ran at once -
# visible in the log as two "watching" lines with no failures between them.
LOCKDIR=/tmp/netplay_watchdog.lock
if ! mkdir "$LOCKDIR" 2>/dev/null; then
	_old=$(cat "$LOCKDIR/pid" 2>/dev/null)
	if [ -n "$_old" ] && [ -d "/proc/$_old" ]; then
		exit 0
	fi
	rm -rf "$LOCKDIR"
	mkdir "$LOCKDIR" 2>/dev/null || exit 0
fi
echo $$ > "$LOCKDIR/pid"
echo $$ > "$LOCK"          # the app reads this one
trap 'rm -rf "$LOCKDIR"; rm -f "$LOCK"' EXIT

link_up() {
	iw dev wlan0 link 2>/dev/null | grep -q "Connected to"
}

on_adhoc() {
	iw dev wlan0 link 2>/dev/null | grep -q "SSID: $PREFIX-"
}

restore_wifi() {
	# Same command the app captured before it moved the client stack. Replaying
	# it verbatim is the only portable route back: /etc/wifi/wifi_init.sh exists
	# on some platforms and not others.
	_cmd=$(head -1 "$RESTORE" 2>/dev/null)
	[ -n "$_cmd" ] || { log "nothing recorded to restore with"; return 1; }

	killall -q wpa_supplicant 2>/dev/null
	ip addr flush dev wlan0 2>/dev/null
	ip link set wlan0 up 2>/dev/null
	sh -c "$_cmd" >/dev/null 2>&1

	_w=1
	while [ $_w -le 10 ]; do
		iw dev wlan0 link 2>/dev/null | grep -q "Connected to" && break
		sleep 1
		_w=$((_w + 1))
	done
	udhcpc -i wlan0 -n -q -t 8 >/dev/null 2>&1

	_ip=$(ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)
	[ -n "$_ip" ] && log "recovered, back on wifi as $_ip" || log "restore attempt did not get an address"
	rm -f "$RESTORE"

	# The ad hoc network is gone - stop the launch stub chasing it.
	#
	# Without this the session still names a network that no longer exists, so
	# the next game launch spends five bounded attempts (~60s of spinner)
	# rejoining nothing before falling back. The rest of the session is left
	# alone: the peer address is still recorded and the bindings still stand, so
	# ending or replacing it stays the user's decision.
	_sess="$NP/state/session"
	if [ -f "$_sess" ] && grep -q '^adhoc_ssid=' "$_sess" 2>/dev/null; then
		grep -v '^adhoc_ssid=' "$_sess" | grep -v '^adhoc_psk=' > "$_sess.tmp" 2>/dev/null &&
			mv "$_sess.tmp" "$_sess" &&
			log "cleared adhoc_ssid from the session - that network is gone"
	fi
}

log "watching (host $HOST, every ${INTERVAL}s, act after $FAILS_NEEDED failures)"

fails=0
checks=0
while [ $checks -lt $MAX_CHECKS ]; do
	sleep $INTERVAL
	checks=$((checks + 1))

	# The session ended, or someone already put us back.
	[ -f "$RESTORE" ] || { log "no restore record - session over, exiting"; exit 0; }

	# Three states, and only one of them means "done".
	#
	# Not being on the ad hoc network is NOT the same as being home. When the
	# host's AP disappears, our supplicant is holding a config that contains
	# only that network, so it sits associated to nothing and keeps scanning -
	# which is precisely the case this watchdog exists for. Treating that as
	# "home" would delete the recovery record and leave the device stranded
	# with no breadcrumb left to recover from.
	if link_up; then
		if ! on_adhoc; then
			log "associated to another network - clearing record and exiting"
			rm -f "$RESTORE"
			exit 0
		fi
		if ping -c2 -W2 "$HOST" >/dev/null 2>&1; then
			[ $fails -gt 0 ] && log "host answered again after $fails failure(s)"
			fails=0
			continue
		fi
		reason="host unreachable"
	else
		reason="not associated to anything"
	fi

	fails=$((fails + 1))
	log "$reason ($fails/$FAILS_NEEDED)"
	if [ $fails -ge $FAILS_NEEDED ]; then
		log "giving up ($reason) - restoring wifi"
		restore_wifi
		exit 0
	fi
done

log "backstop reached after $checks checks - exiting"
