#!/bin/sh
#
# Installs launch stubs for the netplay-capable system emulator paks.
#
#   install-stubs.sh install
#   install-stubs.sh uninstall
#   install-stubs.sh status
#
# Only touches $SDCARD_PATH/Emus/<PLATFORM>/<TAG>.pak directories that we
# create ourselves. Paks that already exist on the SD card belong to somebody
# else and are skipped - see bind-mount.sh for covering those.
#
# Safe to run repeatedly; run it at boot so a NextUI update that adds a system
# gets picked up.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"
: "${SYSTEM_PATH:=$SDCARD_PATH/.system/$PLATFORM}"

NP="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"
STUB="$NP/launcher/launch-stub.sh"
MANIFEST="$NP/state/stubs.list"
SD_EMUS="$SDCARD_PATH/Emus/$PLATFORM"

# Identifies a launch.sh as ours. Anything without this marker is not ours to
# remove, no matter what the manifest says.
MARKER="Installed by Netplay.pak"

# Cores the netplay shim knows how to drive. Systems whose pak uses anything
# else are left alone.
NETPLAY_CORES="fbneo fceumm snes9x mednafen_supafaust picodrive pcsx_rearmed gpsp gambatte"

log() { echo "[netplay-stubs] $*"; }

core_is_supported() {
	_exe="$1"
	for _c in $NETPLAY_CORES; do
		[ "$_exe" = "$_c" ] && return 0
	done
	return 1
}

is_ours() {
	[ -f "$1" ] && grep -q "$MARKER" "$1" 2>/dev/null
}

do_install() {
	if [ ! -f "$STUB" ]; then
		log "missing $STUB"
		return 1
	fi

	mkdir -p "$(dirname "$MANIFEST")" "$SD_EMUS"
	: > "$MANIFEST.new"

	_n=0
	for _pak in "$SYSTEM_PATH/paks/Emus"/*.pak; do
		[ -d "$_pak" ] || continue
		_tag="$(basename "$_pak" .pak)"
		_launch="$_pak/launch.sh"
		[ -f "$_launch" ] || continue

		_exe="$(sed -n 's/^EMU_EXE=\(.*\)$/\1/p' "$_launch" | head -n 1)"
		core_is_supported "$_exe" || continue

		_target="$SD_EMUS/$_tag.pak"

		# Somebody else's pak already overrides this system. Leave it be.
		if [ -e "$_target/launch.sh" ] && ! is_ours "$_target/launch.sh"; then
			log "skip $_tag - SD pak already present and not ours"
			continue
		fi

		mkdir -p "$_target"
		{
			echo "#!/bin/sh"
			echo "# $MARKER - delegates to $SYSTEM_PATH/paks/Emus/$_tag.pak"
			echo "# Remove with: install-stubs.sh uninstall"
			tail -n +2 "$STUB"
		} > "$_target/launch.sh"
		chmod 755 "$_target/launch.sh"

		echo "$_target" >> "$MANIFEST.new"
		_n=$((_n + 1))
		log "installed $_tag ($_exe)"
	done

	mv "$MANIFEST.new" "$MANIFEST"
	log "$_n stub(s) installed"

	report_uncovered
}

# Netplay-capable paks that already own their SD path. We will not modify them,
# so say so plainly rather than leaving the user wondering why a system that
# should support netplay does not.
report_uncovered() {
	_m=0
	for _pak in "$SD_EMUS"/*.pak; do
		[ -d "$_pak" ] || continue
		_launch="$_pak/launch.sh"
		[ -f "$_launch" ] || continue
		is_ours "$_launch" && continue

		_exe="$(sed -n 's/^EMU_EXE=\(.*\)$/\1/p' "$_launch" | head -n 1)"
		core_is_supported "$_exe" || continue

		[ "$_m" -eq 0 ] && log "not covered by stubs (pak owns its SD path):"
		log "  skip $(basename "$_pak" .pak) ($_exe)"
		_m=$((_m + 1))
	done
	[ "$_m" -gt 0 ] && log "run bind-mount.sh up to cover the $_m above"
	return 0
}

do_uninstall() {
	[ -f "$MANIFEST" ] || { log "nothing recorded"; return 0; }

	_n=0
	while IFS= read -r _target; do
		[ -n "$_target" ] || continue
		if is_ours "$_target/launch.sh"; then
			rm -f "$_target/launch.sh"
			# Only if we left it empty - never take a directory with other content.
			rmdir "$_target" 2>/dev/null
			_n=$((_n + 1))
			log "removed $(basename "$_target" .pak)"
		else
			log "skip $_target - not ours"
		fi
	done < "$MANIFEST"

	rm -f "$MANIFEST"
	log "$_n stub(s) removed"
}

do_status() {
	if [ ! -f "$MANIFEST" ]; then
		log "not installed"
		return 0
	fi
	while IFS= read -r _target; do
		[ -n "$_target" ] || continue
		if is_ours "$_target/launch.sh"; then
			log "ok      $(basename "$_target" .pak)"
		else
			log "MISSING $(basename "$_target" .pak)"
		fi
	done < "$MANIFEST"
}

case "$1" in
	install)   do_install ;;
	uninstall) do_uninstall ;;
	status)    do_status ;;
	*)         echo "usage: $0 install|uninstall|status" >&2; exit 2 ;;
esac
