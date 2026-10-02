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
restores the firmware's gadget. Also landed since: a **session-scoped mGBA core**
(the pak's build is used only while a session is armed, so the device's own mGBA
pak is no longer replaced — `launcher/minarch.elf`, `launcher/mgba-manage.sh
ensure`) and **H700 support**, which runs the same pak on Anbernic H700/BaseOS
devices and is **experimental**: repeated sessions freeze that vendor kernel, for
the reasons and with the recovery in §2.11. The remaining open problems, including
that one, are in §7.

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

**It is a tg5040 rule.** H700 handhelds (RG34XX and friends) have a *single*
USB-C port that is both charge and OTG, so there is no host-only top socket and
nothing to choose: the H700 end presents the gadget on its only port, and the
other end takes it in its top socket. Everything else about the H700 port — the
vendor UDC, the `usbc0` role nodes at the same paths, `otg_role` booting as
`usb_device` — matches the Brick; the differences are the socket count and the
kernel bug in §2.11.

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

### 2.11 On H700 the takeover corrupts the kernel's own heap — and that is not ours to fix

Measured on an Anbernic RG34XX (BaseOS 1.2.1, kernel **4.9.170**): the cable link
works — the same vendor stack, the UDC comes up, games play at full speed over it
(59.5 fps, ~0.8 ms round trip). What does not work is doing it *repeatedly*: after
a couple of arm/stop cycles the device locks up hard. Hold power ~10 s to recover;
the next boot may stall on the BaseOS splash (power-cycle once more) and an
unclean shutdown can leave the card dirty (§5).

Three freezes were captured on the kernel's own log, and the shape is the same
every time: **a corrupted slab allocator, faulting wherever the next allocation
happens.** In one it was `kernfs_fop_open` (`nextui.elf` opening a sysfs file), in
another `kbase_context_mmap` in `mali_kbase` (the GPU driver's mmap), in the third
`ion_cma_allocate` (the app allocating a video buffer at startup):

```
Unable to handle kernel paging request at virtual address ffc03cd640000000
CPU: 2 PID: 10445 Comm: netplay.elf Tainted: G           O    4.9.170 #19
PC is at kmem_cache_alloc+0xd0/0x1a0
LR is at ion_cma_allocate+0xec/0x20c
```

Different subsystems, one cause: the gadget takeover itself — creating and
destroying configfs/functionfs objects to swap the firmware's `ffs.adb` for this
pak's function, then putting them back — is unsafe in this vendor kernel. There is
no source for it, so it cannot be fixed here, and there is no ordering or
verification that avoids it: the pak now verifies every attribute write by reading
it back, retries the controller bind for up to five seconds, and waits for the
functionfs instance's endpoint files before offering the controller at all, and
those changes **did** remove the observable failures from every captured cycle
(`failed to start g1: -19`, `sunxi_udc_dequeue: driver is null` — the bind now
succeeds first try) **without** stopping the corruption.

Two markers are red herrings, worth naming so nobody chases them again:

- `configfs-gadget 5100000.udc-controller: failed to start g1: -19` at **~2 s
  uptime** is the firmware's *own* boot-time bring-up. It happens on every boot,
  including healthy ones, and does nothing.
- `sunxi_udc_dequeue: driver is null` accompanied every *crash window* but is a
  consequence of the churn, not the cause — it disappeared once the writes were
  ordered and retried, and the heap still corrupted afterwards.

H700 is therefore documented as **experimental** (`README.md`, "USB-C cable on
H700 (experimental)", and the changelog), with the guidance *one or two sessions
per boot, then reboot*. The two candidate fixes, neither written:

1. **One takeover per boot.** Take the gadget over once and hold it for the whole
   boot; sessions then only control the TUN and the data path. `adb` is
   unavailable for as long as it is held, until a reboot or an explicit hand-back.
2. **Composite.** Link this pak's function *alongside* the firmware's `ffs.adb`
   once and leave both in place, so nothing is ever swapped; `adb` keeps working
   throughout. This needs the joining end to identify our interface specifically
   instead of taking the first vendor-class interface with two bulk endpoints
   (§2.7), and it changes what every host sees on that port.

Both reduce how often the buggy path is exercised rather than eliminating it —
option 1 still does one cycle per boot, option 2 still creates the function and
link once. **tg5040 has never shown any of this**, across months of use; the
freezes are specific to this vendor kernel's gadget code.

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

**The host end switches its port only when the peer is not otherwise visible.**
Forcing the port to host is what brings up the bottom socket's controller, and in
host mode that socket *sources* 5 V rather than accepting a charger — so an
unconditional switch makes "cable in the top, charger in the bottom" ask one
socket to be both. `cp_host_start` scans first and switches only if nothing
answers, and if it did switch and the peer then appears on the top socket's
controller the socket is given back (`cp_bus_is_top_host`, keyed on
`5200000.ehci1`), because the switch is provably not what made the peer visible.
Measured on the host end with the cable in the top socket: the peer sat on bus 1
all session while buses 3 and 4 — `5101000.ehci0`/`ohci0`, the bottom socket's, up
only because of the switch — held nothing. A hard kill makes it moot: the port
mode and the record both go back on the next launch or a reboot, and until then
the count is costing the charge socket, not the device.

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
- **`make test` on macOS skips one check rather than failing it.** `shim/test/dual.sh`
  gives the client a clock nine seconds off the host's by interposing `time(2)`
  through `LD_PRELOAD`, which Darwin ignores; both roles then read one machine
  clock, so the *differ* check cannot hold there. It now reports that as an
  explicit skip on Darwin (`uname -s`), while Linux still runs it in full — the
  check that both sides install the *same* pair runs everywhere.
  `DYLD_INSERT_LIBRARIES` with `DYLD_FORCE_FLAT_NAMESPACE` does not interpose a
  `time` call there either (tried), and `__interpose` was not worth a mach-o-only
  branch for a Linux product. `shim/test/link.sh` — including `CLIENTS=3` — does
  run natively, which is the suite that covers the transport.
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
- **macOS cleans `/tmp`.** Anything kept there disappears mid-session — including
  a running `cat /dev/kmsg` capture, which is how a crash trace was lost after a
  three-hour capture was killed by the cleanup. Long-lived tooling and its logs
  belong in `.rpiv/tmp/np/`, which survives. It cost a whole crash cycle to learn;
  the same cleanup also removes a background `nohup` that was started from there.
- **A crash can leave the card dirty, and the kernel then remounts it read-only.**
  The symptom is every write failing at once — `Tools` stops saving, the app will
  not arm, and a deploy reports `Read-only file system` — plus, sometimes, the next
  boot stalling on the BaseOS splash. This is not a dead card; the FAT is being
  protected:

  ```
  FAT-fs (mmcblk0p7): Volume was not properly unmounted. Some data may be corrupt. Please run fsck.
  FAT-fs (mmcblk0p7): error, fat_free_clusters: deleting FAT entry beyond EOF
  FAT-fs (mmcblk0p7): Filesystem has been set read-only
  ```

  Repair it from a PC with `fsck_msdos -y <data partition>`, then verify and check
  that it is writable again. `~/rg34xx-backup-20261001/fix-card-fat.sh` does exactly
  that (it identifies the card by its `s7` offset, so it cannot hit the wrong
  disk). The same directory holds a full copy of the card's data and
  `baseos-rg34xx-1.2.1.img` for a re-flash, which is the fallback if a repair does
  not hold. **Keep that backup current before any test loop** — repeated hard
  power-offs are what dirty the FAT, and every forced power-off after a freeze is
  one more chance.
- **A read-only card does not block deployment.** The pak's binaries are ordinary
  files: deploy them from the Mac while the card is out (copy into
  `Tools/<platform>/Netplay.pak/bin/<platform>/`, then compare md5s). This is
  faster and more reliable than the network path even when the card is fine.

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

**Building for h700** needs its own toolchain image and its own NextUI tree
(`workspace/h700` is not in the LoveRetro checkout):

```sh
# the h700 tree, at the commit the device firmware was built from (BaseOS ships
# its own; the RG34XX card said 7de90a16)
git clone --branch h700 https://github.com/pvaibhav/NextUI.git .rpiv/tmp/NextUI-h700

docker run --rm -u "$(id -u):$(id -g)" \
  -v "$PWD/nextui-netplay":/work -v "$PWD/.rpiv/tmp/NextUI-h700":/opt/nextui-src \
  -e PREFIX_LOCAL=/work/.pkgprefix -w /work \
  ghcr.io/loveretro/h700-toolchain:latest bash -c '
    cd /opt/nextui-src/workspace/h700/libmsettings && make build
    cd /work/app && make PLATFORM=h700 NEXTUI=/opt/nextui-src'
```

**`make dist` does not build binaries — it packages whatever is in `bin/`.** A
release was once cut with a stale `bin/tg5040` (only h700 had been rebuilt), so the
pak shipped a daemon and app that did not contain the changes its own changelog
described. Before any release, rebuild **every** platform in `pak.json`, and then
prove it from the packaged artefact rather than the build log — extract the zip and
look for a string that only the new code has:

```sh
unzip -q -o dist/Netplay.pak.zip 'Netplay.pak/bin/*' -d /tmp/check
strings /tmp/check/Netplay.pak/bin/tg5040/netplay-cable.elf | grep -c 'took %d ticks to accept'
```

**Releases** are one pre-release per version, with three assets (base pak, FULL
pak, compatibility cores) whose names are fixed by `pak.json`:

```sh
make PLATFORMS="tg5040 h700" MGBA_PLATFORMS="tg5040 h700" dist
gh release create v<version>-cable --prerelease --title "…" --notes-file <notes> \
  dist/Netplay.pak.zip dist/Netplay-Full.pak.zip dist/compatibility-cores.zip
```

Then verify the upload rather than trusting it: `gh release view <tag> --json
assets` reports a `sha256` digest per asset, which must equal `shasum -a 256` of
the local file. Notes are written as a file and kept out of the repo
(`.rpiv/tmp/np/notes-v<version>.md`) so they can be re-edited with `gh release
edit`.

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
change removed the lag that had been reported. The items below have not been
executed or observed.

Still open:

1. **Four-player play works, and is shipped as `v3.2.6-cable`** (the transport
   half; AW2's link remains unplayable and is documented below).
   The transport is complete: a four-slot peer table, the host
   assigning each guest its console number and sending it in the greeting
   (protocol **14**, bumped - an older build refuses the greeting rather than
   mis-attributing packets), every frame carrying its sender's id, and `CMD_DATA`
   relayed to every other guest while the control conversation stays
   host-to-guest. The app accepts up to three guests; capacity is the shim's
   decision per mode, so shared-screen play is still 1:1 with `CORE_BUSY` for
   extras. `CLIENTS=3 sh shim/test/link.sh` passes: four instances on one machine,
   ids, every direction of flow, and the relay.

   **On hardware (Bricks `.45`/`.52` + h700 `.53`, ad-hoc link, input delay 3):**
   AW1 multi-player played fine. AW2 (`AW2P`) reaches the point of confirming
   Multi-Pak multiplayer and then sits on a black screen with the music playing,
   while the host's log shows a textbook session - peers 1 and 2 connected,
   60.0-60.2 fps, ~2400 packets per 10 s, `0 dropped`, `0 frames peer-paused`, for
   the two minutes it was left. So the transport is exonerated; the *game* is
   waiting for something. Two candidates, and the cheap test separates them:
   (a) a player-count mismatch in the game itself (a 4-player Multi-Pak game with
   three consoles present waits forever for the fourth, and the consoles that are
   present still chatter), or (b) gpSP's AW2 protocol only ever having been
   validated with two consoles - `docs/implementation.md` records 213 s unbroken
   for AW2, but that was a two-Brick session, and `serialaw_net_receive` indexes
   its peers by the sender's id, so the three-console path is genuinely unproven.
   Next: run AW2 with exactly two consoles (should reproduce the 213 s result),
   then with the number of players the game was set up for. If it persists,
   `serial_proto.c` has the trace for it - `SERIALPROTO_DEBUG` is a
   commented-out `#define` in `.cache/cores/gpsp/serial_proto.c`, and a core
   built with it logs every AW packet state and command to stderr, which lands in
   `.userdata/<platform>/logs/GBA.txt` next to the shim's pacing lines.
   `docs/cable.md` § Four players has the design.

   **What three devices then measured (Bricks `.45`/`.52` + h700 `.53`, ad-hoc
   link, AW2 `AW2P`, ~90 s sessions):** the link layer is not the problem. The
   core's own accounting reported `we are client 0/1/2`, `netpacket_connected:
   peer 1`, `peer 2`, `2 client(s) now, max 3` on the host and `peer 0` on each
   guest; the per-packet trace showed every console's send count arriving at
   *both* other consoles - `.53` sent 44486 and `.52` received 44317 of them,
   `.52` sent 44811 and `.53` received 44810, the host's 19073 arriving as
   19072/19077 - so the relay is confirmed on hardware, not just in the harness.
   Zero drops everywhere, 60 fps throughout, and all three games listed each
   other (three players, no empty fourth slot). The deadlock is above the
   transport: all three screens black, music playing, packets still flowing.
   The suspicion that fits is gpSP's own AW path discarding a packet shape it
   does not expect - `serialaw_net_receive` only buffers when `cnt >= 2` and
   `len == cnt*2 + 8`, silently in the INTERSYNC case, and AW2 is the one game
   that differs here (`PACK_TAIL_SZ` is 2 for AW2, 1 for AW1).

   **AW2 also hangs with two consoles**, with the third device idle: so it is not
   a multi-peer problem, not the relay, and not this refactor's per-peer work -
   the two-console link path is behaviourally identical to what preceded it
   (host `start(0)` + `connected(1)`, guest `start(1)` + `connected(0)`). That
   leaves timing. The sessions tested so far all ran the **ad-hoc** link, whose
   app-chosen floor is 3 frames (50 ms); house Wi-Fi's floor is 10 (166 ms) and
   the cable's is 1 (16.7 ms), and neither of those has been tried for AW2.
   AW1 tolerating a 3-frame link while AW2 does not fits every observation:
   AW2 syncs, passes its connection screen, and then waits for a packet that
   arrives a few frames later than it will accept. Next test is therefore AW2 on
   the **house Wi-Fi** link kind (10-frame floor), then over the cable if that
   fails - not another instrumentation run.

   **Refuted: ROM, save and region.** All three devices carry the identical AW2
   ROM (`ada32bcc…`), the identical save (`e533c45e…`, 64 KB) and the same
   `AW2P` gamepak code, so a region or save mismatch cannot explain it. (gpSP maps
   both `AW2P` and `AW2E` to `mul_aw2`, so a mismatch would have synced and then
   diverged - worth ruling out, and now it is.)

   **The one question left: regression or pre-existing?** Build the shim from the
   released commit (`git -C nextui-netplay worktree add --detach <path> 7e2fc6c`,
   then build `shim/` for the platform inside the toolchain container - it mounts
   the whole worktree, because the makefile writes to `../bin`). That shim is
   protocol 13 where the current one is 14, so both ends of a test session must
   run it. It is the cheapest way to ask whether the multi-peer work broke AW2's
   two-console path or whether AW2 never worked here: run AW2 on two Bricks with
   the released shim and the same untraced cores.

   **AW1 works, AW2 does not**: identical transport, identical settings, identical
   three devices. Whatever AW2's problem is, it is inside the emulation of that
   game's link, not in the link.

   **Parked by the operator** (Oct 2026): AW2 stays unplayable on the gpSP link
   for now, and mGBA's instanced pairing is the route to a two-console AW2 game.
   Reopening it means either building the released shim
   (`shim_v3.2.5_tg5040.so`, kept in `.rpiv/tmp/np/`) to answer regression-vs-pre-
   existing, or taking it upstream to gpSP, whose AW2 path is what is failing.
   Please instrument only once behaviour is known to be wrong without it: three
   runs were spent on a traced core whose per-packet SD writes were themselves a
   plausible cause.

   **Refuted: input delay as the cause.** House Wi-Fi's 10-frame floor hangs
   exactly as ad-hoc's 3 does, with three consoles and with two. More slack is
   not what AW2 wants.

   **Confound to remove before the next run:** every timing test above ran on the
   traced core, and the trace writes a line to the SD card *per packet on the
   emulation thread* - `serialaw_net_receive` is called straight from the shim's
   delivery path, so each packet pays a synchronous FAT write, proportional to
   the packet rate. That is a real perturbation for a handshake this tight. The
   untraced core has only ever been tried at ad-hoc's floor of 3, so
   **untraced + Wi-Fi (delay 10) is the one untested cell**, and it is the
   configuration the 213 s AW2 record was most likely made in. Restore the
   originals from `.userdata/shared/Netplay/backup-compat-core-<platform>/`
   before retrying, and do not deploy a traced build for a timing question again:
   instrument only once the behaviour is known to be wrong without it.

   **Refuted: silent packet drops in the core.** A traced build that logs every
   packet `serialaw_net_receive` refuses showed 21569 refusals, all of them
   `state 0 cnt 0 len 8` - the zero-payload state heartbeat, which is consumed as
   a state update by design and never buffered. Not one packet with `cnt >= 1`
   and not one length mismatch: AW2's payload packets are accepted exactly as
   AW1's, so `PACK_TAIL_SZ` is not the difference. What remains is the core's
   AW2 serial emulation itself or AW2's own requirements from the SIO registers -
   `SLAVE_IRQ_CYCLES_2P` in `serial_proto.c` is a slave serial-IRQ cadence whose
   name admits it was calibrated for two players, and `serial.c` builds SIOCNT's
   device-id/parent-child bits from `netplay_client_id`, which is the other thing
   AW2 can inspect about the bus. Neither is a transport question.

   **Trace tooling, ready to reuse:** build the compatibility core with
   `SERIALPROTO_DEBUG` uncommented in `.cache/cores/gpsp/serial_proto.c`
   (plus the `[np]` lines in `libretro/libretro.c` for client ids and peer
   counts). A debug build lands at `gpsp_libretro.so` for
   `make platform=tg5040` (aarch64) and runs on the Bricks *and* the h700. It
   goes on each device as
   `Tools/<platform>/Netplay.pak/cores/compatibility/aarch64/gpsp_libretro.so`
   because `force_compatibility` (and the installed-cores-differ fallback) means
   that is the core a session actually wraps; the originals are on-card at
   `.userdata/shared/Netplay/backup-compat-core-<platform>/`. **All three must
   carry the identical build** - the shim compares core fingerprints and refuses
   a peer whose build differs, so a partial deploy breaks the session.

   To watch logs while a session runs, arm the **house Wi-Fi** link kind rather
   than ad-hoc: the ad-hoc network moves the guests off the LAN, so they are
   unreachable from a laptop until they leave the session.
2. **H700 freezes after a couple of cable sessions, and the cause is the vendor
   kernel rather than the pak.** §2.11 has the three captures, the two red-herring
   log lines, and the two candidate fixes (one takeover held for the boot; or this
   pak's function linked alongside the firmware's instead of swapped). Until one is
   written and validated, H700 cable is documented as experimental with the
   guidance *one or two sessions per boot*. Validating either candidate costs crash
   cycles on hardware that has already taken three hard freezes and one filesystem
   corruption, so budget for that and keep the card backed up (§5) — and note that
   both only *reduce* how often the buggy path is exercised.
3. **A launch with an armed session and no link waits ~30 s and then reports
   `Peer unavailable.`** The shim has a timeout, so it is not an infinite hang —
   but the message reaches the player only after half a minute of a
   "Starting instanced link…" overlay, and the state that produced it (an armed
   session whose peer is gone) is silent until then. The earlier "waiting for
   core…" report is almost certainly this same class. Worth a shorter path to the
   same sentence.
4. **The app's crash recovery has still not been observed doing its job.** Its two
   controller writes were rewritten in v3.2.5 to be retried and read back
   (`netsetup.c`: `ns_cable_release_controller`, `ns_cable_udc_restore`), and both
   now wait for the functionfs instance's endpoints first — but every stale
   `usb_restore` seen so far was cleared by the **boot hook**, not by this pass, so
   the rewritten path is itself unexercised. Its consequence if wrong is bounded
   and known: a hard kill mid-session leaves the firmware's gadget unbound and
   `adb` dead until the next Netplay launch or a reboot.
4. **The plan's automated criteria are stale.** `.rpiv/artifacts/plans/2026-09-28_01-18-40_usb-otg-cable-transport.md`
   still describes writing `otg_role`, creating a `netplay` gadget and a 4-key
   record. `docs/cable.md`'s "Bring-up", "What is verified where" and "Failure
   catalogue" are partly stale for the same reason; its teardown and socket
   sections are current. `docs/cable.md` and this file also predate the
   session-scoped mGBA core and the H700 findings — those currently live in
   `README.md` and the changelog, and belong here.
5. **`MaxPower` is the firmware's 500 mA**; the design wanted 100 and argued for
   it explicitly. Not changed, because the takeover deliberately leaves the
   firmware's configuration alone; it needs two more record keys to restore.
6. **No low-battery interlock.** The host end can be asked to source 5 V at 1%,
   which is how a session ends in a dead device rather than a message.
7. **The UI says nothing about the socket rule or the charger rule.** The
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
