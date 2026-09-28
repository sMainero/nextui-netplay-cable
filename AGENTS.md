# AGENTS.md — nextui-netplay

Guidance for AI agents working in this repository.

Everything below was established by **measurement on two Trimui Bricks (tg5040)**
running NextUI-20260719-0, not by inference from source. Where a claim has
evidence, the evidence is quoted verbatim so it can be re-checked. If you find
something here that no longer holds, fix the document — several entries exist
because a plausible-looking assumption cost hours.

Start with `docs/cable.md` for the feature, `.rpiv/artifacts/` for the design,
research and plan behind it, and this file for the things that are true about
the hardware and this tree but are written down nowhere else.

---

## 1. What this repository is

`nextui-netplay` is the source of **Netplay.pak**: a session-setup app
(`app/`), launcher shell scripts (`launcher/`), a libretro shim
(`shim/`), pinned emulator cores (`cores/`) and the packaging (`Makefile`,
`pak.json`). `../NextUI` is the firmware it runs on, and its `workspace/` holds
the platform headers and (inside a container) the cross toolchain.

The feature this document is mostly about is the **USB-C cable transport**: a
third link kind beside same-WiFi and ad-hoc, in which one device presents a
USB gadget and the peer enumerates it, then plain IPv4 over two TUN interfaces
carries the shim's existing TCP transport unchanged.

Landed and working: the gadget takeover, the port-role switch, the link itself
(measured 4/4 ICMP both ways, ~0.56 ms average round trip), and a teardown that
restores the firmware's gadget. **Not yet diagnosed:** a game session that hung
on a "waiting for core…" screen (see §7).

---

## 2. Hardware facts that are not negotiable

### 2.1 This kernel allows exactly one USB gadget, ever

`mkdir /sys/kernel/config/usb_gadget/<anything>` fails while the firmware's `g1`
exists, and the error is a lie:

```
cannot create /sys/kernel/config/usb_gadget/netplay: Cannot allocate memory
```

`dmesg` says what it really is:

```
kobject_add_internal failed for android0 with -EEXIST, don't try to register
    things with the same name in the same directory.
... gadgets_make+0x260/0x2d0 ... configfs_mkdir ... SyS_mkdirat
```

The vendor's `gadgets_make` hardcodes a virtual device named **`android0`**
under `/sys/devices/virtual/android_usb/`, so the *second* gadget directory can
never be created. The `ENOMEM` is not memory pressure (the device has ~650 MB
free).

**Consequence:** the daemon does not create a gadget. It **takes the firmware's
over** — `cp_takeover_prepare` in `app/usbnet.c` discovers `g1`, records what it
displaces, unbinds it, swaps its function for `ffs.cable`, and puts everything
back on the way out. Anything that reintroduces a `mkdir` of a gadget directory
will fail exactly as above.

### 2.2 The bottom USB-C socket carries the UDC; the top socket is host-only

| end | role | socket |
|---|---|---|
| netplay host — `Host ▸ Cable (USB)` | USB **device** (presents the gadget) | **bottom** |
| netplay client — `Join ▸ USB cable peer` | USB **host** (enumerates the peer) | **top** |

Four independent facts, any one sufficient:

- `/sys/class/udc/` contains exactly one controller (`5100000.udc-controller`),
  and `sunxi_usb_udc` is bound to exactly that one.
- The device tree has exactly one `udc-controller` node
  (`udc-controller@0x05100000`).
- Only one UDC driver is built: `CONFIG_USB_SUNXI_UDC0=y`.
- `usbc1@0` — the node that would manage the top socket's USB1 if it were
  dual-role — is `status = okay` but has **no `compatible`**, so no driver binds
  it, and it carries none of the OTG properties `usbc0@0` has
  (`usb_det_vbus_gpio`, `usb_id_gpio`, `usbc-supply`, `usb_port_type`).

Two measurements agree: a flash drive in a top socket enumerates via
`5200000.ehci1` (USB1); a PC in a bottom socket puts the device in device mode
where it enumerates as `TRIMUI ADB` (USB0).

**`top ↔ top` is impossible** as configured and no role switching helps: there is
no UDC to bind, no manager node to switch and no second UDC driver to load.
Giving USB1 a device path needs a DTB/board-file edit and a reflash — the class
of change this design rules out. `bottom ↔ bottom` *is* possible (both ends have
the UDC) but is worse, for the charging reason in §3.

The socket rule is documented in `docs/cable.md` under "Which end is which".

### 2.3 The port's mode is changed by *reading* a node, not writing one

`otg_role` **reports** the mode — `null`, `usb_device` or `usb_host` — and
**rejects writes** (`EINVAL`). The switch is performed by the `show()` of a
sibling node whose name is the mode:

| `otg_role` reads | node to read to select it |
|---|---|
| `null` | `/sys/devices/platform/soc/usbc0/usb_null` |
| `usb_device` | `.../usb_device` |
| `usb_host` | `.../usb_host` |

`cp_role_trigger_name()` in the pure layer (`app/cableproto.c`) owns that
spelling so the daemon's force, the daemon's restore and the app's recovery
cannot disagree. The gadget role must switch to `usb_device` — the firmware
boots the port into `null`, and **a port in `null` mode is never enumerated**,
which is why a gadget end that does not switch presents nothing.

### 2.4 The UDC attribute's return code lies — verify by reading back

An unbind **and** a bind can complete and still return `ENODEV`. Verified: after
a teardown that logged `could not release the controller: No such device`,
`g1/UDC` read back **empty** — the release had happened. A retry loop that
trusts the return code can never observe success, because every write to an
already-unbound attribute fails the same way.

**Always read the attribute back.** `cp_gadget_teardown`, `cp_takeover_bind` and
the app's `ns_cable_release_controller` all do; `ns_cable_udc_restore` always
did. Same principle as `cp_iface_up`, which reads the interface address back
rather than trusting `ip`'s exit status.

### 2.5 Never remove a functionfs function while its mount is up

`rmdir functions/ffs.cable` with its ffs still mounted **oopses the kernel**:

```
Internal error: Oops - SP/PC alignment exception: 8a000000 [#1] PREEMPT SMP
PC is at 0x6b6b6b6b6b6b6b6b            ← SLAB_POISON
LR is at ffs_release_dev.isra.2+0x38/0x50
```

and takes the daemon with it (exit 139, `main()` never reaches its cleanup, the
repair is left half-done). The order that works, verified both ways round:

1. unlink the function from the configuration,
2. `umount2(<ffs_dir>, MNT_DETACH)`,
3. `rmdir` the function directory.

The same removal succeeds with the unmount first and oopses with it last. The
app's recovery (`ns_cable_function_restore`) does it in this order because it was
written from the manual repair; the daemon's `cp_ours_remove` did it in the wrong
order and is why the first hardware run died mid-restore.

### 2.6 A functionfs instance dies when its last file descriptor closes

Write the descriptors to `ep0`, then let the process exit, and:

```
ffs_data_put(): freeing
configfs-gadget 5100000.udc-controller: failed to start g1: -19
```

The instance is freed, the function has no descriptors, and the UDC write fails
with `ENODEV` — an error that reads like a controller fault and is not one. The
daemon holds `ep0`, `ep1`, `ep2` and the eventfd for its whole life and never
hits this; a helper that writes descriptors and exits always will. (This is also
the `failed to start g1: -19` the device logs at boot before anything is
attached.)

### 2.7 The composite is wrong even though it works

`cp_usb_parse_config` (the host role) takes the **first** interface with class
`0xff` and two bulk endpoints. With the firmware's `ffs.adb` still linked, that
first interface is **adb's** (`ff/42/01`) — the host would then claim adb's
interface and speak cable framing at `adb`, silently. So the takeover must
*replace* the firmware's function, not add to it. It is also why
`cp_takeover_prepare` refuses when the functions directory holds anything other
than exactly one function.

### 2.8 Charge current is not tunable

Every charge-control attribute on the PMIC is read-only in practice — mode
`0444` with no `store` callback, so a write returns `EPERM`:

```
axp2202-usb/input_current_limit           -r--r--r--
axp2202-battery/constant_charge_current   -r--r--r--
```

and none of the 24 regulators exposes a current limit (only `state` and
voltage). The only writable power nodes are VBUS *enable*, which the link
requires.

**There is no "share the cable power" feature to build.** See §3 for what to do
instead.

### 2.9 Opening an unguarded usbfs node blocks forever

`open()` on a `/dev/bus/usb/BBB/DDD` node runs a runtime-PM auto-resume of the
device. Open the wrong one and it never returns. Measured, from a daemon left in
state `D` that no signal could kill and which held `npc0` and its claimed
interface until a reboot:

```
syscall: 56 (openat)
  msleep -> ohci_rh_resume -> ohci_bus_resume -> hcd_bus_resume
  -> usb_resume_both -> usb_runtime_resume -> usb_autoresume_device
  -> usbdev_open -> chrdev_open -> do_sys_open
fds: 4 -> /dev/bus/usb   5 -> /dev/bus/usb/004
```

It hung on a **root hub**, reached while scanning (it held a bus directory fd)
and opening every node to ask its descriptors. `ohci_rh_resume` waits in `msleep`
for a resume that cannot complete because the device is gone.

**Consequence:** never open a usbfs node you have not already identified. The
peer's ids are in **sysfs** (`/sys/bus/usb/devices/*/idVendor`, `idProduct`,
`busnum`, `devnum`), so `cp_host_scan` matches there and opens only the node it
built from those numbers. A root hub's ids are never ours, and sysfs lists a
device only while it is enumerated — so a node left behind by an unplugged cable
is never opened at all. `cp_usb_is_peer()` — the open-then-ask-the-descriptors
version — is gone for this reason.

### 2.10 An unplug is `ECONNRESET`, and it is not fatal

The endpoint read returns `ECONNRESET` when a host goes away (`Connection reset
by peer`), and the write returns `ESHUTDOWN` (`Cannot send after transport
endpoint shutdown`). Both are the **normal** end of a cable session.
`cp_rx_worker` treats `ESHUTDOWN`/`EPROTO`/`ENODEV`/`EIO`/`ECONNRESET`/`EPIPE` as
a bounded pause — sleep a tick and read again, because the same descriptor is
valid once a host configures the interface again — and only the remaining errnos
are fatal. Missing `ECONNRESET` from that set is what made a bumped cable kill the
gadget end while the host end of the same cable simply waited.

---

## 3. Power: the socket rule decides who pays

The bottom socket is also the charge socket. That looks like a problem and is
not, because of how the roles fall:

- the **gadget end** (cable in bottom) is charged **through the socket the cable
  occupies** — the occupied socket *is* the charging path;
- the **host end** (cable in top) **sources** the 5 V that charges the other, and
  keeps its charge socket free.

So the end to put on a charger is the **host** end — `Join ▸ USB cable peer` — in
its free bottom socket. Measured on two Bricks with the link up, 90-second
window, both ends gaining:

```
host end,   cable in top,    charger in bottom   15% → 16%   3.757 → 3.769 V
gadget end, cable in bottom                     96% → 97%   4.161 → 4.166 V
```

The host end's VBUS comes from the top socket's supply, which is a fixed
regulator that is **always enabled** — `usb1-vbus`, bound to `reg-fixed-voltage`,
`state=enabled`, 5000 mV — so the cable carries power whenever the host end is
powered, session or not.

This is why `bottom ↔ bottom` is worse: it leaves neither charge socket free,
and the end that would lose its charger is the end doing the sourcing.

**Measurements to be careful with:** the fuel counter moves in 30-unit steps
(`charge_full` = 3000), so one step is 1% — a 90-second window can only show
0 or ±1%. For a rate, sample 10–15 minutes. And `axp2202-battery/status` has been
observed reporting `Charging` with `voltage_now` stubbed to 0 and nothing
plugged in at all, so treat `status` as a hint and the counters as the evidence.

---

## 4. Teardown: the ordering that must not regress

`cp_gadget_teardown` unwinds in this order, and each step is load-bearing:

1. close the endpoint descriptors;
2. **release the controller**, verified by reading `UDC` back;
3. **if it is genuinely still held, stop and touch nothing** — keep the gadget and
   the record. Removing a function from a bound gadget is the oops in §2.5;
4. unlink our function from the configuration, **unmount its ffs**, then `rmdir`
   it (§2.5);
5. rebuild the firmware's function: directory, ffs mount, link into the
   configuration, restart its userspace (`/bin/adbd -D`);
6. write the identity back (`idVendor`/`idProduct`/`bcdDevice` — read from the
   system at takeover, never assumed);
7. rebind the controller, verified by read-back;
8. restore the port mode.

The `usb_restore` record is written **before** anything is taken and cleared
**only** when the restore succeeded — the same policy as `wifi_restore`. A repair
that worked but is reported as failed keeps the record forever, which is why §2.4
matters.

**Across two devices the end order matters: tear the host end down first.** The
gadget end cannot release the controller while the peer still has the interface
claimed; ending in the other order leaves the gadget end with a repair owed to a
reboot (it logs `the controller is still held, so the gadget and the record are
left alone`).

---

## 5. Environment and tooling traps

- **`[ -w ]` lies when you are root.** It reads the file mode, so a sysfs file
  at `0644` looks writable while the kernel refuses the write. Test by writing,
  not by testing the bit. (This cost a wrong conclusion about §2.8.)
- **macOS has no `timeout(1)`.** A long ssh command hangs the caller with no
  bound. Use a background-and-kill wrapper; there is one at `/tmp/np/rshbounded`
  in the author's sessions but it is scratch — rewrite it if needed.
- **The device has no `scp`, `sftp` or `sftp-server`** — only `tar` and `base64`.
  `tools/netplay-harness.py` therefore moves files **over the ssh channel**
  (`ssh … "cat > file"` up, `tar -C … -cf - | tar -xf -` down). It used `scp`,
  which macOS's OpenSSH 9 turned into SFTP and which cannot work against
  dropbear. `require_programs("ssh", "tar")`, not `scp`.
- **`ControlPath` must be short.** It is a unix socket, and `sun_path` is 104
  bytes; `tempfile.gettempdir()` is `/var/folders/<hash>/T` on macOS, which with
  the 40-character `%C` overflows and makes ssh refuse to start — so *every*
  harness subcommand reported "device unreachable". Fixed: the socket lives under
  `/tmp/netplay-harness-<pid>/`.
- **After a reboot the SSH server is gone.** dropbear is a pak you start by hand
  (Tools ▸ SSH Server, press A). A reboot does **not** bring it back, so plan for
  one human action per reboot.
- **A reboot restores the firmware's gadget** — configfs is RAM, so `g1` comes
  back exactly as it shipped, with `ffs.adb`, `adbd` and `otg_role = null`. It
  does **not** clear `usb_restore` unless a session file is present: the boot hook
  drops the record only on the previous-boot path with a session to compare
  against. So a stale record and a stale `cable.pid` routinely survive.
- **The pak folder name is hardcoded 13 times** (`app/netsetup.c:157` builds
  `%s/Tools/%s/Netplay.pak`, plus the launcher scripts, `minarch.elf`,
  `bind-mount.sh`, `install-stubs.sh`, `wrap-pak.sh`, `wifi-watchdog.sh`). It must
  stay `Netplay.pak`; renaming breaks the app, the launch stubs and the mounts.
- **A `Netplay<ext>` sibling in `Tools/<platform>/` becomes a duplicate menu
  entry.** NextUI derives the display name by stripping extensions of 2–5
  characters and types a non-`.pak` directory as a ROM folder, so `Netplay.bak`
  shows as a second "Netplay" that *opens as a folder*. Keep backups outside
  `Tools/`.
- **Scratch goes under `.rpiv/tmp/`** (outside the git repos) or `/tmp` on the
  device, and is deleted when the command that needed it is done.

---

## 6. Build, deploy and test

**Cross-build** (needs the toolchain image; `build-platforms` is absent on the
author's machine, `ghcr.io/loveretro/tg5040-toolchain` is not):

```sh
docker run --rm -u "$(id -u):$(id -g)" \
  -v "$PWD/nextui-netplay":/work -v "$PWD/NextUI":/opt/nextui-src \
  -e PREFIX_LOCAL=/work/.pkgprefix -w /work \
  ghcr.io/loveretro/tg5040-toolchain:latest bash -c '
    cd /opt/nextui-src/workspace/tg5040/libmsettings && make build   # build-platforms does this normally
    cd /work/app  && make PLATFORM=tg5040 NEXTUI=/opt/nextui-src     # netplay.elf, netplay-broker.elf, netplay-cable.elf
    cd /work/shim && make PLATFORM=tg5040'                           # netplay_shim.so
```

Host-only, no container: `make -C nextui-netplay/app proto-test` builds and runs
the pure layer's unit test. `testing/test-cable.sh` runs the vocabulary, launcher
and harness checks.

**Deploy** — do **not** hand-copy files; `tools/netplay-harness.py deploy` already
uploads to `/tmp`, backs up outside the pak
(`.userdata/shared/Netplay/netplay-harness/backups/<stamp>/`), renames into place
so a running binary is never half-written, and verifies the checksum:

```sh
DEVBRIDGE_PASSWORD=<pw> python3 tools/netplay-harness.py deploy \
  --host <ip> --client <ip> --platform tg5040 --no-auto-jump \
  --artifact netplay-cable --file bin/tg5040/netplay-cable.elf --target both
```

Artifacts: `netplay-cable` (the daemon), `netplay-app` (`netplay.elf`),
`netplay-broker`, `netplay-shim`, `netplay-launcher`.

**Packaging:** `make PLATFORMS=tg5040 MGBA_PLATFORMS=tg5040 dist-base` produces
`dist/Netplay.pak.zip`. The **base** pak omits `cores/compatibility/`; the FULL
variant adds it (and that dir has to be harvested from a card or a device, since
`make compatibility` needs `cores/build-compat.sh`, which is not in this tree,
and a `my282` toolchain image that is not published).

**Expected test state:** `proto-test` ok, `test-cable.sh` PASS, harness suite OK.
**`launcher/test.sh` fails 3 assertions at HEAD** (`owned mount not recognised`,
`armed switcher leaked normal recents`, `game leaked into armed switcher`) —
pre-existing, unrelated, and not yours to fix by accident.

---

## 7. Open problems

**Fixed since this document was first written** (kept here because the symptoms
are worth recognising):

- *A bumped cable killed the gadget end and required re-arming the session.*
  Missing `ECONNRESET` in `cp_rx_worker`'s pause set — now §2.10.
- *A bumped cable wedged the host daemon in state `D`.* The scan opened every
  usbfs node including a root hub — now §2.9.
- *Noticeable input lag.* `NS_INPUT_DELAY_CABLE` was 3 (50 ms), borrowed from the
  ad-hoc jitter before the cable had been measured. Measured cable RTT is
  **0.329 / 0.557 / 0.661 ms**, so the floor is now **1** (16.7 ms, the lowest the
  shim's parser accepts). `docs/cable.md` carries the measurement.

All three were then **confirmed in play** on two Bricks: a deliberate cable bump
left both daemons alive and the link came back without re-arming, and the delay
change removed the lag that had been reported. The remaining item below has *not*
been executed.

Still open:

1. **A launch with an armed session and no link waits ~30 s and then reports
   `Peer unavailable.`** The shim has a timeout, so it is not an infinite hang —
   but the message reaches the player only after half a minute of a
   "Starting instanced link…" overlay, and the state that produced it (an armed
   session whose peer is gone) is silent until then. The earlier "waiting for
   core…" report is almost certainly this same class. Worth a shorter path to the
   same sentence.
2. **The app's crash recovery has never been executed.** `NS_cableRecoverIfStranded`
   (its controller-release and unmount/rmdir ordering fixed, its role restore
   converted to node reads) has only ever been reasoned about. A deliberately
   killed session is the way to test it. Its **consequence if wrong is bounded**
   and worth knowing before shipping: a hard kill mid-session leaves the firmware's
   gadget unbound, so `adb` is dead until the next Netplay launch runs the repair —
   or until a reboot, which always restores it. Nothing about gameplay or the
   device's firmware is at risk, which is why this was accepted untested.
3. **The plan's automated criteria are stale.** `.rpiv/artifacts/plans/2026-09-28_01-18-40_usb-otg-cable-transport.md`
   still describes writing `otg_role`, creating a `netplay` gadget and a 4-key
   record. `docs/cable.md`'s "Bring-up", "What is verified where" and "Failure
   catalogue" are partly stale for the same reason; its teardown and socket
   sections are current.
4. **`MaxPower` is the firmware's 500 mA**; the design wanted 100 and argued for
   it explicitly. Not changed, because the takeover deliberately leaves the
   firmware's configuration alone; it needs two more record keys to restore.
5. **No low-battery interlock.** The host end can be asked to source 5 V at 1%,
   which is how a session ends in a dead device rather than a message.
6. **The UI says nothing about the socket rule or the charger rule.** The
   progress text is the only place a user would look, and "wrong socket" is
   silent (§2.2).

---

## 8. Conventions this tree already has

Respect them; they are deliberate.

- **The pure layer stays host-buildable.** `app/cableproto.c` and `app/usbnet.h`
  compile and are unit-tested on a developer's machine, with no kernel headers
  and no includes from the rest of the tree (`make -C app proto-test`). Path
  spellings, the descriptor blobs, the framing codec and the status grammar all
  live there precisely so they can be tested off-device.
- **Platform facts live in `CP_Facts`, each with an `NP_CABLE_*` environment
  override**, so a test can put configfs, the UDC list, the TUN device or the
  role nodes in a fake tree.
- **The record discipline.** `usb_restore` is written before a mutation, and
  removed only by a repair that succeeded.
- **Comments explain *why*, and quote measurements.** The style throughout is
  "what was observed, and what it forces"; several comments cite a specific log
  line or register dump. Match it.
- **Nothing here changes the kernel, firmware, u-boot or rootfs.** Everything the
  feature touches is RAM — configfs and a port-mode register — and the firmware
  rebuilds both at boot. That is what makes a hard kill unable to strand the
  device, and it is the property to protect.

---

## 9. Rules for an agent working here

- **Ask before writing to a device.** Read-only inspection is always fine;
  changing a port mode, a gadget, a pak file or the power state is not.
- **A reboot is a human action** (SSH must be re-enabled by hand), so batch work
  and avoid needing one.
- **Never rename the pak**, and never leave a `Netplay<ext>` sibling inside
  `Tools/`.
- **Do not create a USB gadget.** Take the firmware's over (§2.1).
- **Verify kernel writes by reading back** (§2.4), and never remove a functionfs
  function with its mount up (§2.5).
- **Keep `proto-test`, `testing/test-cable.sh` and the harness suite green.** The
  3 `launcher/test.sh` failures are pre-existing.
- **Read `docs/cable.md` before changing the transport**, then update it if you
  change a fact it states. Several of its statements are load-bearing.
