# Shim architecture

Netplay currently ships patched `minarch.elf`, `gambatte_libretro.so` and
`gpsp_libretro.so` for one exact NextUI build, so every NextUI release needs six
new binaries and a merge against a drifting `minarch`. This is the replacement:
nothing is patched, and nothing tracks a NextUI build.

## Why the cores never needed rebuilding

The core "patches" are barely patches:

- **gambatte** — a Makefile stanza adding `tg5040`/`tg5050` with `HAVE_NETWORK=1`.
  Zero source changes. GB link is upstream libretro.
- **gpsp** — two small fixes to `rfu.c`/`libretro.c` (disconnect handling, packet
  queue 4→16). GBA RFU netplay is upstream.

Both link only against libc/libm/libstdc++/libgcc and export exactly the standard
libretro symbols. Nothing in either binds to a NextUI build — they are `dlopen`'d
over the stable libretro C ABI. They need rebuilding when the core sources or the
toolchain change, not when NextUI ships.

## The shim

`shim/shim.c` is a libretro core that wraps another libretro core. minarch loads
it; it `dlopen`s the real core (`RTLD_LOCAL`, so the two sets of `retro_*` symbols
do not collide) and forwards every entry point. Sitting in between gives us the
seams netplay needs:

| Seam | Replaces |
|---|---|
| `retro_set_environment` | answering `SET_NETPACKET_INTERFACE` ourselves — GB/GBA link on a stock frontend, no `ma_environment.c` patch |
| `retro_set_input_state` | `Netplay_getPlayerButtons` in `ma_input.c` |
| `retro_run` | frame gating; returning without running the core is a legal frame skip that keeps the frontend responsive |
| `retro_serialize` / `retro_unserialize` | rollback, plus detection of frontend-initiated loads (rewind, load state) that would silently desync |
| `retro_get_variable` | `minarch_get/setCoreOptionValue`, `forceCoreOptionUpdate`, `saveConfig` |
| `retro_unload_game` / `retro_load_game` | `minarch_reloadGame` |

Network I/O belongs on a shim-owned background thread, which also removes the
reason `minarch_hdmimon` / `beforeSleep` / `afterSleep` / `Netplay_pollWhilePaused`
existed — 26 call sites whose only job was keeping the link alive while minarch's
loop was busy.

With no session armed the shim is a pure passthrough.

### The filename constraint

minarch derives `core.name` from the core's **path**, not its reported
`library_name` (`ma_core.c`):

```c
void Core_getName(char* in_name, char* out_name) {
	strcpy(out_name, basename(in_name));
	char* tmp = strrchr(out_name, '_');   // truncate at last underscore
	tmp[0] = '\0';
}
```

and `core.name` feeds both:

```c
sprintf(core.config_dir, USERDATA_PATH        "/%s-%s", core.tag, core.name);
sprintf(core.states_dir, SHARED_USERDATA_PATH "/%s-%s", core.tag, core.name);
```

So the shim **must** be loaded under the real core's filename. Loaded as
`netplay_shim_libretro.so` it would yield `core.name = "netplay_shim"` and
silently relocate every save state. `launcher/minarch.elf` stages a copy at
`<pak>/cores/<realcore>_libretro.so` for exactly this reason, and
`launcher/test.sh` asserts it.

(`strrchr`'s result is used unchecked, so a core filename with no underscore
crashes minarch. `*_libretro.so` always has one.)

## Getting the shim loaded

Emu paks invoke `minarch.elf` **unqualified**, so it resolves through `PATH`
(`MinUI.pak/launch.sh`: `export PATH=$SYSTEM_PATH/bin:...`). That is the hook.

### Tier 1 — launch stubs (default)

`getEmuPath` (`utils.c`) checks the SD card before the system paks:

```c
sprintf(pak_path, "%s/Emus/%s/%s.pak/launch.sh", SDCARD_PATH, PLATFORM, emu_name);
if (exists(pak_path)) return;
sprintf(pak_path, "%s/Emus/%s.pak/launch.sh", PAKS_PATH, emu_name);
```

So for a pak living under `$SYSTEM_PATH`, we drop a stub at the free SD path. It
prepends our launcher dir to `PATH` and delegates to the original, which runs at
its real location and therefore derives `EMU_TAG` and `CORES_PATH` correctly. The
original is never modified. The stub is identical for every system — it takes its
tag from `$0` — so `install-stubs.sh` writes the same bytes to each.

Both link cores are in this tier:

| Class | Paks | Cores |
|---|---|---|
| SYSTEM (free SD path) | FC, SFC, MD, PS, **GBA, GB, GBC** | fceumm, snes9x, picodrive, pcsx_rearmed, **gpsp, gambatte** |
| EXTRAS (owns its SD path) | FBN, SUPA, 32X, SEGACD, GG, SMS, FDS, SG1000 | fbneo, supafaust, picodrive |

### Tier 2 — staged copy + bind mount (opt-in)

An EXTRAS pak already occupies the SD path, so there is nothing to override. We
do not modify paks we do not own. `wrap-pak.sh` instead stages a copy under our
own directory and bind-mounts it over the original:

```
$NP/wrapped/<TAG>.pak/
├── launch.sh       ours - puts the shim on PATH, then runs launch.sh.old
├── launch.sh.old   the original, verbatim
└── ...             everything else the original contained
```

```sh
mount --bind "$NP/wrapped/FBN.pak" "$SDCARD_PATH/Emus/$PLATFORM/FBN.pak"
```

The original bytes on disk are never touched, and `down` (or a reboot) restores
everything. Passthrough is just running `launch.sh.old`.

The copy has to include the pak's other files, because the mount replaces the
whole directory and the original derives its paths from `$0` — which, under the
mount, resolves back into our staged copy. Only two netplay-capable paks bundle
a core (`CORES_PATH=$(dirname "$0")`):

| Stages as | Paks |
|---|---|
| `launch.sh` only, a few KB | 32X, SEGACD, GG, SMS, SG1000, FDS |
| plus the bundled core | FBN, SUPA |

Because the mount replaces the directory, `EMU_TAG` and `CORES_PATH` still
resolve correctly — `launcher/test.sh` asserts both by presenting the staged
directory at the original path.

Two things not to do instead:

- **Shadowing only the pak's `launch.sh`** hides the original the stub needs to
  delegate to. Stashing the original at a second path does not save it: it then
  runs with a different `$0`, so `CORES_PATH=$(dirname "$0")` points somewhere
  without the core — breaking exactly FBN and SUPA.
- **Shadowing `minarch.elf`** covers every pak with one mount, but if anything
  is wrong *every* game launch breaks rather than one system. The staged-copy
  approach keeps the blast radius per pak.

The staged copy goes stale if the pak is updated underneath it, so `sync`
fingerprints the source and `up` refuses to mount a stale copy. Mounts do not
survive a reboot; run `sync` then `up` from a `boot.d` hook to persist.

## What does not survive

- **The in-game netplay menu.** `api.c` is compiled into `minarch.elf`, which is
  built without `-rdynamic` — 0 of its 347 dynamic symbols are `GFX_`/`PAD_`, so
  nothing outside the executable can call in. Session setup moves to the pak app,
  which already compiles its own `api.c`. In-game the shim can only draw into the
  core's framebuffer.
- **Blocking FF/rewind/save states.** The shim cannot stop the frontend. It does
  not need to for FF (frame gating neutralises it) and detects rewind/load-state
  via unserialize provenance. Save state only calls `retro_serialize` and should
  be harmless once network I/O is off the main loop.

Unbinding the shortcuts via config is not a fix: `Config_readControls` reads
`default_cfg` then `user_cfg`, so the user's bindings win. And forcing a config
sandbox via `DEVICE` or a different reported core name would relocate
`states_dir` — same save-state loss as above.

## Layout

```
shim/
├── shim.c              the wrapper
├── include/libretro.h  vendored, so the shim needs no NextUI checkout
├── makefile            make PLATFORM=tg5040 | make native
└── test/run.sh         fake core + a harness that mimics Core_open
launcher/
├── minarch.elf         PATH shadow; decides stock vs shim, stages the copy
├── launch-stub.sh      per-system stub (identical for all)
├── install-stubs.sh    install | uninstall | status
├── wrap-pak.sh         sync | up | down | status
└── test.sh             fake SD tree
```

Both test scripts are host-only and need no device.
