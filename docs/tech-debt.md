# Tech debt

Work identified but deliberately not done. Each entry says what it is, why it
was not done at the time, and what doing it involves, so it can be picked up
cold.

Entries are independent unless one says otherwise.

---

## 1. A separate pak for test builds (`netplay-testing.pak`)

**The want.** Keep the release installed and working under `Tools/<platform>/Netplay.pak`,
and run development builds from a second directory — `netplay-testing.pak` — so a
broken or instrumented build cannot cost you your normal Netplay.

**Why it is not done.** The code cannot run from another name. It compiles and
resolves paths from a hardcoded `Netplay.pak`, so a renamed pak silently reads and
writes the *release's* directory and state — which is the exact breakage the
convention exists to prevent. Doing this properly means changing shipped launcher
paths and the setup app, which is not a change to make casually or at the end of
a long session.

### What already works

`launcher/pak-launch.sh` — the pak's own entry point — already derives its
directory and exports it:

```sh
DIR="$(dirname "$0")"; cd "$DIR" || exit 1
NETPLAY_PAK="$(pwd)"; export NETPLAY_PAK
...
"$DIR/bin/$PLATFORM/netplay.elf" "$@"
```

So opening `netplay-testing.pak` already sets `NETPLAY_PAK` to itself and runs
*that* pak's binary. Nothing downstream honours it.

### The change list

| # | where | change | note |
|---|---|---|---|
| 1 | `app/netsetup.c:173` | read `NETPLAY_PAK` from the environment (already exported by `pak-launch.sh`) before falling back to the literal | keep the literal as the fallback so release behaviour is unchanged |
| 2 | `launcher/state-path.sh:8` | derive `NETPLAY_STATE` from `basename "$NETPLAY_PAK" .pak` | `Netplay.pak` yields `Netplay`, so no existing install moves |
| 3 | `launcher/install-stubs.sh:91` and `launcher/launch-stub.sh:33` | the installer substitutes its own `$NP` into the stub it writes, instead of the stub hardcoding the path | the stub's `$0` at runtime is the *Emus* stub, so it cannot derive its own pak — the path must be baked in at install time |
| 4 | `launcher/bind-mount.sh:53,61`, `wrap-pak.sh:35,39`, `wifi-watchdog.sh:33`, `gameswitcher-launch.sh:9` | derive `NP` from `$0` — all of these are executed, not sourced, so `$0` is reliable | `pre-launch.sh` and `state-path.sh` are *sourced*; their `$0` is the caller's, so they must be told the path, not derive it |
| 5 | the three `MARKER="Installed by Netplay.pak"` sites, `HOOK_TAG="Netplay.pak-on-boot"`, and the grep at `app/netsetup.c:1578` | make both name-derived, and keep the grep in step | otherwise the two paks uninstall each other's wrappers |

Roughly **10 edits across 7 files**, plus one C change.

### The constraint that cannot be engineered away

Both paks install wrappers over the **same** `Emus/<platform>/<TAG>.pak`. Only one
pak can own those at a time. So "both fully active at once" is impossible. The
workable model is: both installed, the test pak takes the wrappers while it is in
use, and hands them back (or is uninstalled) afterwards. The binaries, cores and
state stay separate, which is the part that actually protects the release.

### Verification

`launcher/test.sh` and `testing/test-cable.sh` both assert on the wrapper marker,
so both must be run afterwards, and the marker change must keep them green. The
`3` pre-existing `launcher/test.sh` failures are unrelated (see `AGENTS.md` §6).

### Until then

The safe equivalent is a **snapshot**: copy the pak directory and
`$SDCARD_PATH/.userdata/shared/Netplay/` aside before a test and restore them
after, and unmount the launcher wrappers before replacing a pak under them
(`mount | grep paks/Emus`, then `umount`) — they live outside the pak, so a
replaced pak leaves them pointing at staging that no longer exists.

---

## 2. The pak name is baked in

The inventory behind entry 1. Measured at `v3.2.6-cable`: **43 occurrences of the
literal `Netplay.pak` across 25 files**, of which **seven are genuinely
hardcoded** — the rest are already overridable defaults of the form
`${NETPLAY_PAK:-$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak}`:

```
app/netsetup.c:173                    compiled in
launcher/wrap-pak.sh:35               no override
launcher/wifi-watchdog.sh:33          no override
launcher/gameswitcher-launch.sh:9     no override
launcher/launch-stub.sh:33            no override
launcher/install-stubs.sh:20          no override
launcher/bind-mount.sh:53             no override
```

Plus, per-pak by name and so also not swappable: the wrapper markers listed in
entry 1, and the packaging (`Makefile:3`, the archive names, `pak.json`'s
`release_filename`).

`AGENTS.md` §5 used to claim "hardcoded 13 times", which was neither the right
number nor the right distinction.

---

## 3. Internet play: the manual-address row

**What is proven.** The transport works over the public internet. A guest on a
different network connected to a host on the home LAN through a public relay:
`connected as client (client_id 1)` in 287 ms, 60.2 fps both sides, zero dropped
packets across every run. The shim connects to one address from the session file
and never inspects an interface, so any routable path is already a working
transport. Full writeup: `docs/wan-play.md` (currently only on the branch
`parked/four-player-link`).

**What blocks it.** Discovery. The host advertises itself with a UDP **broadcast**
every 500 ms (`app/netsetup.c:3194-3221`) and the guest takes the host's address
from the **source address of that packet** (`app/netsetup.c:3316`). Routers do not
forward broadcast, so across any L3 link the guest's Join list stays empty and
there is nothing to select. The guest has no way to type an address.

**The change.** A manual peer row on the Join screen — the address is already the
only thing the transport needs, and `NS_arm()` already takes a `peer_ip`. Sizing
notes: there is **no text-input widget anywhere in the app** (no OSK, no typed
field), so a free-text entry is real UI work. The cheap version is a config knob
in the session file that synthesises one Join row. A four-octet picker built from
the existing pad-driven menu widgets is the middle option.

**Also needed for a real feature, not just a demo:** a WAN delay floor. The app
assigns `input_delay=10` to a `wifi` link and `1` to a cable; `input_delay=N` also
*pins* the value, so the shim cannot lower it. Measured on a relay with ~170 ms of
round trip, delivery was much worse than at delay 3 (35-37 packets per window
arriving more than 4 frames late, versus 0-1). A fourth link kind with its own
measured floor is the honest fix.

**Carrier caveats.** L3 overlays (WireGuard, Tailscale) carry the transport but
**not** discovery — broadcast does not cross them. An L2 overlay (ZeroTier in L2
mode, tinc, n2n, OpenVPN `tap`) does carry it. `CONFIG_IP_NF_NAT` is unset on
tg5040, so a device can never be a NAT router for another. The device ships no
`wg`, `tailscale`, `zerotier-cli` or `openvpn` binary, and its kernel has no
WireGuard at all.

---

## 4. Advance Wars two-console link deadlocks — cause not found

**Symptom.** In a two-console AW1 session over gpSP link, the host goes black
after pressing start and the guest sits on "wait until preparations are done".
The transport is healthy throughout: 600 packets per window each way, 0 dropped,
60.2 fps.

**Ruled out, each by measurement:**

| hypothesis | how it was eliminated |
|---|---|
| internet latency | deadlock reproduced at ~11 ms (direct LAN) exactly as at ~170 ms (public relay) |
| the relay / address transparency | the handshake carries only magic, protocol and slot — no addresses; byte-transparent |
| the transport | perfect on a plain LAN with nothing between the devices |
| `input_delay` | deadlocks at 10 and at 3 |
| core selection | same result with the compatibility core and with the `override/` core; both devices carry byte-identical cores |
| the four-player refactor | the devices were running `v3.2.5-cable`, which predates it |

**The one useful clue.** A working two-console AW1 session exists from
**2026-09-21** on the same hardware — the shim log shows real gameplay traffic
(packet counts varying `548…2243` per window) where today's runs show a frozen
`600` every window. So this worked at some point and does not now.

**Where to look next.** Diff the *running* environment between then and now, not
the source: what the pak on the device was, what the cores were, and what the
session file contained. `AGENTS.md` §7 item 1 already documents the same
behaviour for AW2 and parks it as a core-level gpSP issue; this may be the same
problem, and two-console AW1 may simply never have worked on this build.

---

## 5. Housekeeping

- **A `Netplay<ext>` sibling in `Tools/<platform>/` is a duplicate menu entry.**
  `Netplay.bak` was present in `Tools/tg5040/` on both test devices. NextUI strips
  extensions of 2-5 characters, so it shows as a second "Netplay" that *opens as a
  folder*. Keep backups outside `Tools/`. (See `AGENTS.md` §5.)
- **The devices and the repo are one release apart.** Both test devices run
  `v3.2.5-cable`; `main` (and `origin/main`) carry `v3.2.6-cable`, which includes
  the four-player work. Decide which is the definitive line before the next
  release.
- **`AGENTS.md` §5's "hardcoded 13 times"** is wrong and superseded by entry 2.
