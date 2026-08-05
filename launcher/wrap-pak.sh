#!/bin/sh
#
# Covers emulator paks that already own their SD path, without writing to them.
#
#   wrap-pak.sh sync     stage a copy of each wrappable pak under our own dir
#   wrap-pak.sh up       bind-mount the staged copies over the originals
#   wrap-pak.sh down     unmount
#   wrap-pak.sh status
#
# install-stubs.sh handles paks under $SYSTEM_PATH by dropping an override at
# the free SD path. An EXTRAS pak occupies that path itself, so there is nothing
# to override. Rather than modify a pak we do not own, we stage a copy:
#
#   $NP/wrapped/<TAG>.pak/
#   |-- launch.sh       ours - puts the shim on PATH, then runs launch.sh.old
#   |-- launch.sh.old   the original, verbatim
#   `-- ...             everything else the original contained
#
# and bind-mount that directory over the original. The original bytes on disk
# are never touched, and `down` (or a reboot) restores everything.
#
# Why the copy has to include the pak's other files: the mount replaces the
# whole directory, and paks like FBN and SUPA bundle their core and locate it
# with CORES_PATH=$(dirname "$0"). Under the mount that resolves into our staged
# directory, so the core has to be there. Paks that use the system cores dir
# stage as just a couple of shell scripts.
#
# Scoped per pak on purpose. If something is wrong, one system is affected
# rather than every game launch.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"

NP="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"
STAGE="$NP/wrapped"
SD_EMUS="$SDCARD_PATH/Emus/$PLATFORM"
MARKER="Installed by Netplay.pak"

NETPLAY_CORES="fbneo fceumm snes9x mednafen_supafaust picodrive pcsx_rearmed gpsp gambatte"

log() { echo "[netplay-wrap] $*"; }

core_is_supported() {
	_exe="$1"
	for _c in $NETPLAY_CORES; do
		[ "$_exe" = "$_c" ] && return 0
	done
	return 1
}

is_mounted() {
	grep -q " $(echo "$1" | sed 's/ /\\040/g') " /proc/mounts 2>/dev/null
}

# Paks on the SD path that we could wrap: real pak, supported core, not already
# one of our own stubs.
wrappable() {
	for _pak in "$SD_EMUS"/*.pak; do
		[ -d "$_pak" ] || continue
		_launch="$_pak/launch.sh"
		[ -f "$_launch" ] || continue
		grep -q "$MARKER" "$_launch" 2>/dev/null && continue
		is_mounted "$_pak" && { echo "$_pak"; continue; }

		_exe="$(sed -n 's/^EMU_EXE=\(.*\)$/\1/p' "$_launch" | head -n 1)"
		core_is_supported "$_exe" && echo "$_pak"
	done
}

do_sync() {
	# Staging reads the originals, so nothing may be shadowing them.
	for _pak in $(wrappable); do
		if is_mounted "$_pak"; then
			log "refusing to sync while mounted - run 'down' first"
			return 1
		fi
	done

	mkdir -p "$STAGE"
	_n=0
	for _pak in $(wrappable); do
		_tag="$(basename "$_pak" .pak)"
		_dst="$STAGE/$_tag.pak"

		rm -rf "$_dst"
		mkdir -p "$_dst"
		cp -a "$_pak"/. "$_dst"/ 2>/dev/null || { log "copy failed for $_tag"; continue; }

		mv "$_dst/launch.sh" "$_dst/launch.sh.old"
		chmod 755 "$_dst/launch.sh.old"

		{
			echo "#!/bin/sh"
			echo "# $MARKER - wraps $_tag, original preserved as launch.sh.old"
			echo "DIR=\"\$(dirname \"\$0\")\""
			echo "PATH=\"$NP/launcher:\$PATH\""
			echo "export PATH"
			echo "exec \"\$DIR/launch.sh.old\" \"\$@\""
		} > "$_dst/launch.sh"
		chmod 755 "$_dst/launch.sh"

		# Fingerprint the original so a later sync can tell it changed.
		( cd "$_pak" && ls -la ) > "$_dst/.source-fingerprint" 2>/dev/null

		_n=$((_n + 1))
		log "staged $_tag ($(du -sh "$_dst" 2>/dev/null | cut -f1))"
	done
	log "$_n pak(s) staged"
}

# The staged copy goes stale if the pak is updated underneath us.
needs_resync() {
	_pak="$1"
	_dst="$STAGE/$(basename "$_pak" .pak).pak"
	[ -d "$_dst" ] || return 0
	[ -f "$_dst/.source-fingerprint" ] || return 0
	_now="$( cd "$_pak" && ls -la 2>/dev/null )"
	[ "$_now" != "$(cat "$_dst/.source-fingerprint")" ]
}

do_up() {
	command -v mount >/dev/null 2>&1 || { log "no mount available"; return 1; }

	_n=0
	for _pak in $(wrappable); do
		_tag="$(basename "$_pak" .pak)"
		_dst="$STAGE/$_tag.pak"

		is_mounted "$_pak" && { log "$_tag already up"; continue; }

		if [ ! -d "$_dst" ]; then
			log "$_tag not staged - run 'sync' first"
			continue
		fi
		if needs_resync "$_pak"; then
			log "$_tag changed on disk - run 'down' then 'sync'"
			continue
		fi

		if mount --bind "$_dst" "$_pak"; then
			_n=$((_n + 1))
			log "up $_tag"
		else
			log "could not mount $_tag (needs root?)"
		fi
	done
	log "$_n pak(s) mounted"
}

do_down() {
	_n=0
	for _pak in "$SD_EMUS"/*.pak; do
		[ -d "$_pak" ] || continue
		is_mounted "$_pak" || continue
		if umount "$_pak" 2>/dev/null; then
			_n=$((_n + 1))
			log "down $(basename "$_pak" .pak)"
		else
			log "could not unmount $(basename "$_pak" .pak)"
		fi
	done
	log "$_n pak(s) unmounted"
}

do_status() {
	for _pak in $(wrappable); do
		_tag="$(basename "$_pak" .pak)"
		if is_mounted "$_pak"; then
			log "up      $_tag"
		elif [ -d "$STAGE/$_tag.pak" ]; then
			needs_resync "$_pak" && log "stale   $_tag" || log "staged  $_tag"
		else
			log "unstaged $_tag"
		fi
	done
}

case "$1" in
	sync)   do_sync ;;
	up)     do_up ;;
	down)   do_down ;;
	status) do_status ;;
	*)      echo "usage: $0 sync|up|down|status" >&2; exit 2 ;;
esac
