#!/bin/sh
# Install and restore the mGBA pak paired with Netplay's instanced core.
#
# All mutable metadata lives below .userdata/shared/Netplay.  The installed pak
# carries a matching marker as a second ownership check: restore/remove never
# destroys a directory merely because it happens to be named MGBA.pak.

set -u

SDCARD_PATH=${SDCARD_PATH:-/mnt/SDCARD}
NETPLAY_PAK=${NETPLAY_PAK:-"$SDCARD_PATH/Tools/${PLATFORM:-tg5040}/Netplay.pak"}
. "$NETPLAY_PAK/launcher/state-path.sh"
MGBA_STATE="$NETPLAY_STATE/mgba"
MANIFEST="$MGBA_STATE/install.manifest"
IGNORE_DIR="$MGBA_STATE/ignored-hashes"
SAVE_BACKUP="$NETPLAY_STATE/mgba-save-backup"
COLLISIONS="$NETPLAY_STATE/mgba-restore-collisions"

say() { printf '%s\n' "$*"; }
hash_file() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }

platform_dirs() {
	for _d in "$SDCARD_PATH"/Emus/*; do
		[ -d "$_d" ] || continue
		_p=${_d##*/}
		[ -d "$NETPLAY_PAK/cores/mgba/$_p/MGBA.pak" ] || continue
		printf '%s\n' "$_p"
	done
}

managed_platforms() {
	if [ -f "$MANIFEST" ]; then
		sed -n 's/^platform=//p' "$MANIFEST"
	else
		platform_dirs
	fi
}

manifest_value() {
	_key=$1
	[ -f "$MANIFEST" ] || return 1
	sed -n "s/^${_key}=//p" "$MANIFEST" | tail -n 1
}

backup_path_for() { manifest_value "backup.$1"; }

status() {
	_any=0; _mismatch=0; _ignored=1; _missing=0
	for _p in $(platform_dirs); do
		_expected="$NETPLAY_PAK/cores/mgba/$_p/MGBA.pak/mgba_libretro.so"
		_installed="$SDCARD_PATH/Emus/$_p/MGBA.pak/mgba_libretro.so"
		if [ ! -f "$_installed" ]; then
			_missing=1; _ignored=0
			say "platform.$_p=missing"
			continue
		fi
		_any=1
		_eh=$(hash_file "$_expected"); _ih=$(hash_file "$_installed")
		if [ -n "$_eh" ] && [ "$_eh" = "$_ih" ]; then
			say "platform.$_p=ok"
		elif [ -f "$IGNORE_DIR/$_p" ] &&
		     [ "$(cat "$IGNORE_DIR/$_p" 2>/dev/null)" = "$_ih:$_eh" ]; then
			_mismatch=1
			say "platform.$_p=ignored"
		else
			_mismatch=1; _ignored=0
			say "platform.$_p=mismatch"
		fi
	done
	[ -f "$MANIFEST" ] && _managed=1 || _managed=0
	say "installed=$_any"
	say "managed=$_managed"
	say "missing=$_missing"
	say "mismatch=$_mismatch"
	say "mismatch_ignored=$_ignored"
	_date=$(manifest_value installed_at 2>/dev/null || true)
	say "backup_date=$_date"
}

next_backup() {
	_base=$1.bak; _candidate=$_base; _n=1
	while [ -e "$_candidate" ]; do
		_candidate="$_base.$_n"; _n=$((_n + 1))
	done
	printf '%s\n' "$_candidate"
}

copy_tree() {
	_src=$1; _dst=$2
	[ -d "$_src" ] || return 0
	mkdir -p "$_dst" || return 1
	cp -Rp "$_src"/. "$_dst"/
}

install() {
	mkdir -p "$MGBA_STATE" "$IGNORE_DIR" "$SAVE_BACKUP" || exit 1
	_stamp=$(date '+%Y-%m-%d_%H-%M-%S')
	_human=$(date '+%Y-%m-%d %H:%M')
	_tmp_manifest="$MANIFEST.tmp.$$"
	if [ -f "$MANIFEST" ]; then
		# Reinstallation repairs the managed copy but must never replace the
		# user's original pak/data snapshot with the already-installed copy.
		cp "$MANIFEST" "$_tmp_manifest" || exit 1
		_reinstall=1
	else
		_reinstall=0
		_snapshot="$SAVE_BACKUP/$_stamp"
		mkdir -p "$_snapshot" || exit 1
		copy_tree "$SDCARD_PATH/Saves/MGBA" "$_snapshot/Saves/MGBA" || exit 1
		copy_tree "$SDCARD_PATH/.userdata/shared/MGBA-mgba" "$_snapshot/States/MGBA-mgba" || exit 1
		{
			say "version=1"
			say "installed_at=$_human"
			say "snapshot=$_snapshot"
		} > "$_tmp_manifest" || exit 1
	fi

	_installed=""
	for _p in $(platform_dirs); do
		_src="$NETPLAY_PAK/cores/mgba/$_p/MGBA.pak"
		_dst="$SDCARD_PATH/Emus/$_p/MGBA.pak"
		_tmp="$SDCARD_PATH/Emus/$_p/MGBA.pak.netplay.tmp.$$"
		_backup=""
		if [ "$_reinstall" = 1 ] && [ -f "$_dst/.netplay-installed" ] &&
		   grep -q '^netplay-mgba-v1$' "$_dst/.netplay-installed"; then
			rm -rf "$_dst"
		elif [ "$_reinstall" = 1 ] && [ -e "$_dst" ]; then
			# A third-party replacement made after installation is not the user's
			# original backup and not ours to delete. Preserve it separately.
			_extra="$COLLISIONS/reinstall-$_stamp/$_p/MGBA.pak"
			mkdir -p "${_extra%/*}" || exit 1
			mv "$_dst" "$_extra" || exit 1
		elif [ -e "$_dst" ]; then
			_backup=$(next_backup "$_dst")
			mv "$_dst" "$_backup" || exit 1
		fi
		rm -rf "$_tmp"
		if ! mkdir -p "$_tmp" || ! cp -Rp "$_src"/. "$_tmp"/; then
			rm -rf "$_tmp"
			[ -n "$_backup" ] && mv "$_backup" "$_dst"
			exit 1
		fi
		_expected=$(hash_file "$_src/mgba_libretro.so")
		_actual=$(hash_file "$_tmp/mgba_libretro.so")
		if [ -z "$_expected" ] || [ "$_expected" != "$_actual" ]; then
			rm -rf "$_tmp"
			[ -n "$_backup" ] && mv "$_backup" "$_dst"
			exit 1
		fi
		{
			say "netplay-mgba-v1"
			say "platform=$_p"
			say "core_sha256=$_actual"
			say "installed_at=$_human"
		} > "$_tmp/.netplay-installed" || exit 1
		mv "$_tmp" "$_dst" || exit 1
		if [ "$_reinstall" = 0 ] || ! grep -q "^platform=$_p\$" "$_tmp_manifest"; then
			say "platform=$_p" >> "$_tmp_manifest"
			say "backup.$_p=$_backup" >> "$_tmp_manifest"
			say "installed_sha.$_p=$_actual" >> "$_tmp_manifest"
		fi
		rm -f "$IGNORE_DIR/$_p"
		_installed="$_installed $_p"
	done
	[ -n "$_installed" ] || { rm -f "$_tmp_manifest"; exit 1; }
	mkdir -p "$SDCARD_PATH/Roms/Game Boy Advance (MGBA)" \
	             "$SDCARD_PATH/Saves/MGBA" \
	             "$SDCARD_PATH/.userdata/shared/MGBA-mgba" || exit 1
	mv "$_tmp_manifest" "$MANIFEST" || exit 1
	say "ok=1"
}

# Install, but only on a device that has no mGBA pak at all.
#
# This is the arm-time path, so it runs without the player asking for anything
# and therefore may only ever add. If any platform this build has an mGBA pak
# for already carries an Emus/<p>/MGBA.pak - stock, third-party, or a copy
# netplay installed earlier - nothing is touched, because install() would move
# that pak aside as a backup first, and that is a device mutation nobody asked
# for. The half-state (one platform missing, another present) is deliberately
# left to the explicit mGBA screen, where installing is a choice the player
# makes.
#
# Says ok=1 when it installed (install()'s own line), otherwise skipped=1 with a
# reason, so a caller can report which happened without parsing prose.
ensure() {
	_src_any=0
	for _p in $(platform_dirs); do
		_src_any=1
		if [ -e "$SDCARD_PATH/Emus/$_p/MGBA.pak" ]; then
			say "skipped=1"
			say "reason=mGBA pak already present on $_p"
			return 0
		fi
	done
	if [ "$_src_any" = 0 ]; then
		say "skipped=1"
		say "reason=this build ships no mGBA pak"
		return 0
	fi
	install
}

ignore() {
	mkdir -p "$IGNORE_DIR" || exit 1
	for _p in $(platform_dirs); do
		_e="$NETPLAY_PAK/cores/mgba/$_p/MGBA.pak/mgba_libretro.so"
		_i="$SDCARD_PATH/Emus/$_p/MGBA.pak/mgba_libretro.so"
		[ -f "$_e" ] && [ -f "$_i" ] || continue
		printf '%s:%s\n' "$(hash_file "$_i")" "$(hash_file "$_e")" > "$IGNORE_DIR/$_p"
	done
	say "ok=1"
}

preserve_and_copy() {
	_src=$1; _dst=$2; _kind=$3; _stamp=$4
	[ -d "$_src" ] || return 0
	find "$_src" -type f | while IFS= read -r _file; do
		_rel=${_file#"$_src"/}
		_target="$_dst/$_rel"
		if [ -e "$_target" ]; then
			_safe="$COLLISIONS/$_stamp/$_kind/$_rel"
			mkdir -p "${_safe%/*}" || exit 1
			mv "$_target" "$_safe" || exit 1
		fi
		mkdir -p "${_target%/*}" || exit 1
		cp -p "$_file" "$_target" || exit 1
	done
}

restore() {
	_restore_pak=${2:-1}; _restore_saves=${3:-1}; _restore_states=${4:-1}
	[ -f "$MANIFEST" ] || { say "ok=1"; return 0; }
	_stamp=$(date '+%Y-%m-%d_%H-%M-%S')
	# Read once: backup_path_for()/manifest_value() would otherwise re-open and
	# re-sed the whole file for every platform below.
	_manifest_content=$(cat "$MANIFEST" 2>/dev/null)
	_snapshot=$(printf '%s\n' "$_manifest_content" | sed -n 's/^snapshot=//p' | tail -n 1)
	if [ "$_restore_pak" = 1 ]; then
		for _p in $(printf '%s\n' "$_manifest_content" | sed -n 's/^platform=//p'); do
			_dst="$SDCARD_PATH/Emus/$_p/MGBA.pak"
			_backup=$(printf '%s\n' "$_manifest_content" | sed -n "s/^backup.$_p=//p" | tail -n 1)
			# Only remove a pak that carries our ownership marker. A user replacing
			# it after installation is never treated as disposable.
			if [ -f "$_dst/.netplay-installed" ] &&
			   grep -q '^netplay-mgba-v1$' "$_dst/.netplay-installed"; then
				rm -rf "$_dst"
			fi
			if [ -n "$_backup" ] && [ -e "$_backup" ] && [ ! -e "$_dst" ]; then
				mv "$_backup" "$_dst" || exit 1
			fi
		done
	fi
	[ "$_restore_saves" = 1 ] && preserve_and_copy \
		"$_snapshot/Saves/MGBA" "$SDCARD_PATH/Saves/MGBA" saves "$_stamp"
	[ "$_restore_states" = 1 ] && preserve_and_copy \
		"$_snapshot/States/MGBA-mgba" "$SDCARD_PATH/.userdata/shared/MGBA-mgba" states "$_stamp"
	if [ "$_restore_pak" = 1 ]; then
		mv "$MANIFEST" "$MGBA_STATE/restored-$_stamp.manifest"
		rm -rf "$IGNORE_DIR"
	fi
	say "ok=1"
}

case ${1:-status} in
	status) status ;;
	install|reinstall) install ;;
	ensure) ensure ;;
	ignore) ignore ;;
	restore) restore "$@" ;;
	*) say "usage: $0 status|install|reinstall|ensure|ignore|restore [pak saves states]" >&2; exit 2 ;;
esac
