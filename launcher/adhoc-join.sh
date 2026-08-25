#!/bin/sh
#
# Re-join the ad hoc network, if the armed session says we belong on it.
#
# The app joins when you pick a peer, but the game is launched later from the
# NextUI menu, and the platform re-establishes its own WiFi in between - the
# device is back on the house network by the time the core starts, with the
# session still pointing at 10.0.0.1. Same shape as WiFi power save: the state
# does not survive leaving the app, so it has to be re-applied per launch.
#
# No-op unless a session names an ad hoc network, and no-op if we are already
# associated to it - the common case once it has worked once.
#
#   adhoc-join.sh <session-file>

SESSION="$1"
[ -f "$SESSION" ] || exit 0

# The host session also records the ad-hoc SSID so the persistent broker and a
# reopened setup UI can describe the network they own.  That is metadata, not
# an instruction to associate as a station.  Running the client rejoin path on
# a host tears down/reconfigures wlan0 while hostapd owns wlan1 and can make the
# host's 10.0.0.1 endpoint unreachable just before the emulator starts.
ROLE=$(sed -n 's/^role=//p' "$SESSION" | head -1)
[ "$ROLE" = "client" ] || exit 0

# The session now lives in shared userdata, so it no longer identifies a
# platform pak. Callers export NETPLAY_PAK; retain defaults for manual use.
: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${NETPLAY_PAK:=$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}"
. "$NETPLAY_PAK/launcher/state-path.sh"

SSID=$(sed -n 's/^adhoc_ssid=//p' "$SESSION" | head -1)
PSK=$(sed -n 's/^adhoc_psk=//p' "$SESSION" | head -1)
[ -n "$SSID" ] || exit 0

say() { echo "[netplay-adhoc] $*" >&2; }

# Already there? Nothing to do.
if iw dev wlan0 link 2>/dev/null | grep -q "SSID: $SSID"; then
	say "already on $SSID"
	exit 0
fi

say "not on $SSID - rejoining"

# Reuse the platform's control socket, so the frontend's wifi status keeps
# working while we are on the ad hoc network. Without this the signal icon and
# SSID go blank even though the radio is associated - see docs/adhoc.md.
CTRL=""
for d in /proc/[0-9]*; do
	a0=$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null | head -1)
	case "$a0" in
	*/wpa_supplicant|wpa_supplicant)
		# Both spellings occur: `-O/etc/wifi/sockets` joined on tg5040,
		# `-c /path` as two arguments on my282.
		want=""
		for a in $(tr '\0' '\n' < "$d/cmdline" 2>/dev/null); do
			if [ -n "$want" ]; then
				[ "$want" = "O" ] && CTRL="$a" || CONFPATH="$a"
				want=""
				continue
			fi
			case "$a" in
			-O)   want=O ;;
			-c)   want=c ;;
			-O?*) CTRL=${a#-O} ;;
			-c?*) CONFPATH=${a#-c} ;;
			esac
		done
		break ;;
	esac
done
if [ -z "$CTRL" ] && [ -n "${CONFPATH:-}" ] && [ -f "$CONFPATH" ]; then
	CTRL=$(sed -n 's/^ctrl_interface=\(DIR=\)\?//p' "$CONFPATH" 2>/dev/null | sed 's/ .*//' | head -1)
fi
[ -n "$CTRL" ] || CTRL=/var/run/wpa_supplicant
say "using ctrl_interface $CTRL"

CONF=/tmp/netplay_adhoc_join.conf
cat > "$CONF" <<EOF
ctrl_interface=$CTRL
update_config=1
network={
	ssid="$SSID"
	psk="$PSK"
	key_mgmt=WPA-PSK
	priority=99
}
EOF

# Record how to get back BEFORE taking the client stack down.
#
# The app does this when it joins, but the app is not what joins most of the
# time - this script is, on every game launch, because the platform reclaims
# the radio whenever the app exits. Without writing the record here, a session
# whose joining was done by the stub had no recovery breadcrumb at all and no
# watchdog: the two mechanisms meant to prevent stranding were simply never
# armed. That is exactly what happened in testing.
RESTORE="$NETPLAY_STATE/wifi_restore"
if [ ! -f "$RESTORE" ]; then
	for d in /proc/[0-9]*; do
		a0=$(tr '\0' '\n' < "$d/cmdline" 2>/dev/null | head -1)
		case "$a0" in
		*/wpa_supplicant|wpa_supplicant)
			tr '\0' ' ' < "$d/cmdline" > "$RESTORE" 2>/dev/null
			echo >> "$RESTORE"
			say "recorded restore command"
			break ;;
		esac
	done
fi

killall -q wpa_supplicant 2>/dev/null
killall -q udhcpc 2>/dev/null
ip addr flush dev wlan0 2>/dev/null
sleep 1

attempt=1
while [ $attempt -le 5 ]; do
	ip link set wlan0 up 2>/dev/null
	wpa_supplicant -B -D nl80211 -i wlan0 -c "$CONF" >/dev/null 2>&1

	w=1
	while [ $w -le 8 ]; do
		iw dev wlan0 link 2>/dev/null | grep -q "SSID: $SSID" && break
		sleep 1
		w=$((w + 1))
	done

	if iw dev wlan0 link 2>/dev/null | grep -q "SSID: $SSID"; then
		# iw reports the SSID once associated, which is before the 4-way
		# handshake finishes; a DHCP request sent in that gap is dropped.
		sleep 1
		udhcpc -i wlan0 -n -q -t 6 >/dev/null 2>&1
		IP=$(ip -4 addr show wlan0 2>/dev/null | sed -n 's/.*inet \([0-9.]*\).*/\1/p' | head -1)
		if [ -n "$IP" ]; then
			say "rejoined $SSID as $IP (attempt $attempt)"
			# Guard the session we just re-entered. Self-terminating and
			# single-instance, so launching game after game cannot pile these up.
			if [ -x "$NETPLAY_PAK/launcher/wifi-watchdog.sh" ]; then
				# Properly detached, not merely backgrounded.
				#
				# `( cmd & )` forks but leaves the child in this process group
				# and session - and this runs inside the game launch, so the
				# frontend's teardown waited on it when the game exited. The
				# symptom was a black screen that came back the moment the
				# watchdog finished, which is a poor way to learn about process
				# groups. start-stop-daemon -b forks, setsids and detaches.
				# -m -p: track by pidfile. Without it, busybox matches on the
				# executable, sees some other /bin/sh and refuses with
				# "already running" - so the detach silently does not happen.
				if command -v start-stop-daemon >/dev/null 2>&1; then
					start-stop-daemon -S -b -m -p /tmp/netplay_watchdog_ssd.pid \
						-x /bin/sh -- \
						"$NETPLAY_PAK/launcher/wifi-watchdog.sh" "$NETPLAY_PAK"
				else
					( sh "$NETPLAY_PAK/launcher/wifi-watchdog.sh" "$NETPLAY_PAK" </dev/null & )
				fi
				say "watchdog armed"
			fi
			exit 0
		fi
		say "  associated but no address (attempt $attempt)"
	else
		say "  no association (attempt $attempt)"
	fi

	killall -q wpa_supplicant 2>/dev/null
	[ $attempt -lt 5 ] && sleep 3
	attempt=$((attempt + 1))
done

# Leave the radio usable rather than stranded: the launch continues either way,
# and the shim will report that it never reached its peer.
say "could not rejoin $SSID - restoring previous network"
killall -q wpa_supplicant 2>/dev/null
ip addr flush dev wlan0 2>/dev/null
if [ -x /etc/wifi/wifi_init.sh ]; then
	/etc/wifi/wifi_init.sh stop  >/dev/null 2>&1
	/etc/wifi/wifi_init.sh start >/dev/null 2>&1
elif [ -f "$RESTORE" ]; then
	# A30 has no wifi_init.sh. Replay the command captured before joining,
	# which is the same portable fallback used by the app and watchdog.
	OLD_SUPPLICANT=$(head -1 "$RESTORE" 2>/dev/null)
	if [ -n "$OLD_SUPPLICANT" ]; then
		sh -c "$OLD_SUPPLICANT" >/dev/null 2>&1
		w=1
		while [ $w -le 10 ]; do
			iw dev wlan0 link 2>/dev/null | grep -q "Connected to" && break
			sleep 1
			w=$((w + 1))
		done
		udhcpc -i wlan0 -n -q -t 3 -T 2 >/dev/null 2>&1
	fi
fi
exit 1
