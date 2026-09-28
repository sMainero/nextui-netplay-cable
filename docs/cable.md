# The USB cable link

How Netplay.pak puts two handhelds on a point-to-point USB link with no WiFi
involved, what the stock kernel will and will not give us, and the failure
modes worth recognising. Everything here that has been measured, is measured;
everything that has not is labelled **not yet measured** and collected as a
device checklist at the end. Those items exist because no desktop can answer
them: the development machine has no USB controller, no `/dev/net/tun` and no
usbfs.

## Why bother

Shared-screen netplay is lockstep - a frame advances only when both sides have
each other's input for it - which makes it a latency problem. The measured cost
of a network hop is in [adhoc.md](adhoc.md): 4.3 ms average one hop, 26–43 ms
and sometimes 300 ms through an access point. The arithmetic ties out there and
the same window applies here: `input_delay` frames of buffer is
`delay × 16.7 ms`, and once RTT exceeds it the pipeline drains every frame.

A cable is the same idea taken to its end: one wire, no association, no beacon,
no channel survey, no second radio, and nothing to re-establish when a screen is
reopened. It is also the only transport that still works with both radios off.

The price is `adb`. A USB controller holds exactly one gadget, and on this
firmware the one holding it is the firmware's own - the gadget `Makefile`'s
`adb push` runs through. A cable session takes it and gives it back. Everything
the transport does about crashes follows from that one sentence.

## What the hardware actually is

|  | Trimui Brick (`tg5040`) |
|---|---|
| UDC | `sunxi-udc`, fixed endpoint table |
| bulk endpoint pairs | two (a third is gated behind `SUN50IW1`/`SUN8IW6`/`SUN8IW5`, so `sun50iw10` does not get it) |
| configfs | mounted, `usb_gadget` writable |
| functionfs | built in, listed in `/proc/filesystems` |
| `/dev/net/tun` | present |
| vendor OTG role nodes | `/sys/devices/platform/soc/usbc0/{otg_role,usb_host,usb_device}` |
| kernel | 4.9.170, stock |

**The kernel cannot give us an ethernet gadget.** `CONFIG_USB_CONFIGFS_ECM`,
`_NCM` and `_RNDIS` are all `is not set` on the shipped kernel, and the pak ships
userspace only, so there is no `usb0` to be had without a kernel rebuild. What
the kernel *does* have is FunctionFS: a userspace function, driven entirely
through configfs and a handful of file descriptors. That is the whole reason
this transport is a daemon rather than a network interface.

Three of the facts above are not true of every platform the pak supports, and
that is why they are **probed at run time** rather than compiled in. The app asks
four questions before it will offer a cable link at all, and the startup log
names the first one that failed:

| answer | what it means |
|---|---|
| `cable link: no USB controller` | `/sys/class/udc` is empty or absent — nothing can act as a device |
| `cable link: no /dev/net/tun` | the kernel has no TUN device, so there is nothing to bridge to |
| `cable link: configfs is not writable` | `/sys/kernel/config/usb_gadget` is missing or read-only |
| `cable link: this kernel has no functionfs` | `/proc/filesystems` does not list `functionfs` |
| `cable link: supported` | all four hold; the Host menu and the Join list offer the cable |

The answer is cached for the life of the process, so a device that gained a
controller is noticed at the next launch rather than mid-screen. The *peer*
question is deliberately not cached: "is there something on the other end of the
wire" is a live fact about live hardware.

## Which end is which

The **netplay host is the USB device**, and the **netplay client is the USB
host**. Three things force it:

- The client is the side that learns its peer. A USB host enumerating a device is
  exactly a side that discovers something; the netplay host's peer is invented
  from its own address, not discovered.
- The existing fixed-address precedent is the ad-hoc path, where the client is
  armed with `NS_HOTSPOT_HOST_IP` before anything is up. A cable client is armed
  the same way.
- The gadget side is the passive, already-proven one — `adbd` runs it today — and
  it is the side that serves the session.

Addresses are therefore fixed and known before the link exists:

| end | address |
|---|---|
| netplay host (USB device) | `10.77.0.1` |
| netplay client (USB host) | `10.77.0.2` |
| prefix | `/24` |

Deliberately not `10.0.0.0/24`, which is the ad-hoc subnet: both previous
transport additions in this repository produced an interface or address
collision within two days, and the cheapest defence is not to reuse the range.

## The shape of the link

A single **vendor-class interface** (`0xff`) with **two bulk endpoints** —
one IN, one OUT — and nothing else. That is the shape `adbd` already runs on this
device, which means the vendor controller's fixed endpoint table is already known
to map it with no kernel change; and it is the shape FunctionFS's descriptor
validator accepts, where a CDC functional descriptor (`0x24`) would be rejected
outright.

| | |
|---|---|
| idVendor / idProduct | `0x4e50` / `0x0001` |
| class | `0xff`, unassigned pair |
| endpoints | bulk IN `0x81`, bulk OUT `0x02` |
| packet size | 64 (full speed), 512 (high speed) |
| interface string | `netplay cable` |

Unassigned is the point. No kernel driver claims it, so the host side reaches the
interface through usbfs without detaching anything, and it cannot collide with
the firmware's gadget — which holds the controller whenever we are not bound.

The descriptors are written to `ep0` as a FunctionFS V2 blob, and the strings blob
in a second write; in 4.9 that second write is what makes the endpoint files
appear, and it is not optional. Both blobs are built by pure functions with no
ambient state, because they are the two things that fail invisibly: the kernel
validates them and refuses the whole gadget if a field is off.

The interface above them is a **TUN** device — layer 3, no packet information
header, `IFF_TUN | IFF_NO_PI` — brought up as `npc0` with MTU 1500. Layer 2 would
buy an ARP round trip before the first TCP SYN and a neighbour table to time out,
for no capability this transport uses: the payload is plain TCP on 55437, and the
manifest exchange is plain TCP on 55439.

Records on the wire:

```
[ length : 2 bytes big-endian ][ one IP packet ]
```

A bulk transfer ends at a short packet, so a record whose total length is an exact
multiple of the endpoint's packet size has no short packet to end it. 4.9's
`f_fs` appends nothing, so the writer must: a zero-length write on the gadget side,
`URB_ZERO_PACKET` on the host side. Only the aligned case needs it — 512 and 1024
bytes at high speed, 64 and 128 at full speed — and asking is a pure function so
both roles answer it the same way.

Reads are whole multiples of the largest packet size (2048 = 4 × 512 = 32 × 64),
because `f_fs` sizes a read request up to a maxpacket multiple and **drops**
anything the controller delivers beyond that: a request that is not a multiple can
only ever be one that ends in the middle of what the peer is sending.

## Bring-up

**Gadget role** (the netplay host). Strictly ordered, and the order is the point:

1. Put the port back in **device** mode if it is not already. A gadget bound to a
   port that is not presenting as a device is never enumerated. This is the one
   port write that owes no record, because device mode is where the firmware and
   `adb` already live.
2. Open `/dev/net/tun`, bring `npc0` up at `10.77.0.1`, MTU 1500.
3. Ensure configfs is mounted and has `usb_gadget`.
4. Build the gadget: directory, `idVendor`/`idProduct`, configuration, `MaxPower`,
   the `ffs.cable` function, and the link into the configuration.
5. Mount functionfs on `/dev/usb-ffs/cable` with source `cable`, open `ep0`, write
   the descriptor blob and then the strings blob.
6. Open `ep1` (IN, device → host) and `ep2` (OUT, host → device) against a
   deadline. There is **no `ready` attribute on this kernel to wait on** — that
   arrived in much later kernels — so the endpoint files appearing is the
   readiness signal, and the controller is written only once they exist.
7. **Record, then take**: write `usb_restore`, hand the previous owner's `UDC`
   attribute back to empty, then write our controller name. A failure at any step
   leaves the record in place, which is what makes the next launch able to repair.
8. Publish `state=wait`. The peer is expected later, and its absence is not a
   failure on this side.

**Host role** (the netplay client). Same file, different half:

1. Force the port to **host** mode, with the current mode recorded first. This is
   the one operation on this device that can put a process into uninterruptible
   sleep, so it is never done inline — see below.
2. Bring `npc0` up at `10.77.0.2`.
3. Find the peer on the usbfs bus: read its device and configuration descriptors,
   and accept it only if its interface is class `0xff` with exactly two bulk
   endpoints.
4. `USBDEVFS_CLAIMINTERFACE`, then two asynchronous URBs — one permanently in
   flight for receive, one submitted per record for transmit. Asynchronous
   because only one ioctl may be outstanding per file description at a time, and
   a bridge that serialised its two directions would not be a bridge.
5. Publish `state=up` once the peer is claimed and records are moving.

### The port's role is written by a disposable child

Which end of a C-to-C cable enumerates is a decision, not a consequence: two
dual-role ports negotiate CC with no way to prefer one, and two device-only sinks
produce nothing at all — the "cold socket" where CC sits outside the vRd window,
VBUS is 0 V and nothing enumerates.

So the host role has to say "host" out loud. The saying is the problem: the vendor
stack is reported to put a process that writes that attribute into uninterruptible
sleep until reboot, and **SIGKILL does not clear that state**. Any helper that is
waited on can therefore hang the daemon forever, and a `popen`/`pclose` that bounds
only the read hangs exactly the same way.

So the daemon never writes the attribute. It forks a child that writes it and
leaves a one-line marker; the parent polls the marker against a ~2 s deadline,
reaps with `WNOHANG`, and treats a missing marker as **"the port did not switch
(role not confirmed)"** — a sentence on screen, not a hang and not a crash.

## Teardown, and what a crash leaves behind

`NS_cableStop` sends `SIGTERM`, waits up to two seconds, escalates to `SIGKILL`,
reaps, and removes the pid and status files. The wait is worth it because a
graceful stop is how the daemon hands the controller, the firmware's function and
the port back.

A graceful stop unwinds in `cp_gadget_teardown`'s order, and the order is the
kernel's rather than ours:

1. the endpoint descriptors are closed;
2. the controller is released;
3. our function is unlinked and removed, and the firmware's is put back - its
   directory, its functionfs mount and the userspace that owns it;
4. the identity the takeover stamped over is written back, and the controller is
   bound to the gadget again;
5. the port mode is restored.

**Two of those steps are load-bearing on this vendor kernel, and both were found
by watching them fail on hardware.**

*The controller must be released before the function is touched.* Removing a
FunctionFS function from a gadget that is still bound corrupts the kernel's
functionfs instance: `ffs_release_dev` then calls through freed and poisoned
memory - `PC = 0x6b6b6b6b6b6b6b6b`, which is `SLAB_POISON` - and the oops takes
the daemon with it, leaving the repair half-done and undocumented. So the
release is retried for five seconds and, if it never succeeds, **nothing else is
done**: the gadget is left exactly as it is and the record is kept truthful for
the next launch or a reboot to finish.

*The release can only succeed once the peer lets go.* The write comes back
`ENODEV` while the other end still has the interface claimed over usbfs with
transfers outstanding, because the stop cannot complete mid-transfer. That is an
**ordering rule for a session between two devices**: the host end stops first -
closing its usbfs descriptor makes it release the interface, the gadget end sees
a disconnect, and its release then succeeds. Ending both ends in any other order
leaves the gadget end with a repair owed to a reboot.

A daemon that is **killed** hard leaves `usb_restore` behind, and that record is
the entire crash story. It names the gadget, the controller, and what of the
firmware's gadget the takeover displaced:

```
gadget=g1                              udc=5100000.udc-controller
function=ffs.adb                       config=c.1
mount=/dev/usb-ffs/adb                 exe=/bin/adbd -D
identity=0x18d1,0xd002,0x0409
role_node=usb_null                     role_value=null
```

- It is written **before** the controller is taken and **before** the port is
  switched, so a kill or an oops can never leave a mutation with nothing
  recorded. Every one of those values is *read* from the running system rather
  than assumed - including the identity, which is why a firmware build that
  chooses different ids is still restored exactly.
- Only a repair that **succeeded** removes it, the same policy as the radio's
  `wifi_restore`.
- The live repair is `NS_cableRecoverIfStranded`, run at app start beside the
  radio's pass. It stops a daemon that is still running with no session, then
  **releases the controller first**, swaps the firmware's function back in,
  rebinds the controller, and puts the port mode back - in that order, for the
  reasons above.
- `launcher/session-cleanup.sh` drops the record only on the **previous-boot**
  path, where a reboot has already made it moot: configfs is a RAM filesystem, so
  the firmware's gadget is rebuilt as it shipped, the controller is back with it,
  and the port's role is a register that resets.

Nothing in that record is a firmware or filesystem commitment, so the exposure if
nothing repairs it is `adb` not working until the next Netplay launch - or a
reboot, which is always the complete repair. That is the deliberate trade for the
one thing this design would not give up: a hard kill cannot strand the device.

## The session file

One new key, documented in `launcher/session.conf.example`:

```
link=wifi | adhoc | cable
```

It is the single place the transport is decided. Written by the app at arm time,
and read back by the launcher guards, the status line and the input-delay floor.

- A **cable** session writes `peer=10.77.0.1` and `input_delay=3`, and writes **no
  `adhoc_ssid`/`adhoc_psk`** — which is what keeps `adhoc-join.sh` and the WiFi
  watchdog out of it.
- A cable session **must not** write `wifi_restore`. If it did,
  `NS_wifiRecoverIfStranded` would believe the radio had been moved and would tear
  down a working connection on the next launch.
- Sessions armed by a build that predates `link=` have no key. Both the app and
  the shell fall back to the old rule — ad hoc when `adhoc_ssid` is present, wifi
  otherwise — so an update does not end an armed session.

`input_delay` is a **floor**, not a guess, and it is its own constant:
`NS_INPUT_DELAY_CABLE` is 3 frames (50 ms) for a single hop with
millisecond-scale latency. The shim may raise it from measured RTT and never
lower it, and both sides converge because each adopts the higher of the two
proposals. Measuring the cable and re-deriving the constant is a follow-up; the
ad-hoc measurements (1.7 / 4.3 / 21.5 ms min/avg/max) are the closest analogue and
the reason 3 is the starting point rather than 10.

## The status file, and the log

`cable.status` is the app's only view of a daemon it does not own and cannot call,
so its grammar is a contract between two processes. One `key=value` per line,
`state=` and `role=` always present, the rest omitted while empty, no escaping
anywhere — a value carrying a line break would forge a line, so such a value is
refused rather than written. It is published by `rename`, so a reader never sees a
half-written record.

```
state   idle | role | gadget | wait | up | failed
role    gadget | host
gadget  the gadget directory we created
udc     the controller we bound it to
iface   the point-to-point interface
ip      our address on it
peer    the far end's address
error   what went wrong, when state is failed
```

The app keeps its own copy of those six tokens and refuses to guess: a token it
does not know reads as "no state at all", and every wait it performs is a
deadline over states rather than a deadline over a process.

`cable.log` carries the repository's wall-clock stamp and its own tag —
`[HH:MM:SS.mmm] [netplay-cable] ` — and rotates one generation by size, with a
`NETPLAY_CABLE_LOG_MAX` override. It is not session state, exactly as
`broker.log` is not.

The four files the daemon owns, and how each ends:

| file | on a graceful end | on a crash |
|---|---|---|
| `cable.pid` | removed | left behind; the next start refuses to run and the app's recovery stops the stale process |
| `cable.status` | removed | left behind; it reads as a daemon that is `wait`ing, which is one of the ways a stale daemon is detected |
| `usb_restore` | removed **only once the repair succeeded** | kept, and repaired at the next app start |
| `cable.log` | kept | kept |

## The daemon's lifecycle

One binary, `bin/<platform>/netplay-cable.elf`, two roles selected by
`--role gadget|host`. The role is a command-line argument rather than something
read out of the session file, because on both ends the link has to be up *before*
the app arms the session — the same ordering the WiFi paths use.

Its lifecycle follows the broker's, deliberately and in detail:

- the pid file is created `O_EXCL` and is the single-instance lock;
- the status file is published as `<path>.tmp.<pid>` then renamed;
- the stop flag is `sig_atomic_t` set by a signal handler that does nothing else;
- one 100 ms poll on the data path, with the work that is not per-packet — the
  peer question, the lifetime check, the status publish — on every tenth tick;
- the daemon's lifetime is the session: it exits when the session file it has seen
  disappears, and it tolerates a window before the session exists, because the
  link legitimately comes up first;
- the starter ritual (`fork`, `setsid`, log on both descriptors, file-descriptor
  sweep, `execl`) is the app's, and readiness is *live pid **and** published
  status*, 30 × 100 ms with an early exit when the child is already gone.

The one place it deliberately departs from the broker is the poll set. On 4.9 the
FunctionFS endpoint files have **no `.poll`** — only `ep0` implements one — so
polling `ep1`/`ep2` would return immediately and forever. The receive direction is
therefore a blocking `read()` on its own thread, cancelled at teardown; the
transmit direction stays in the main loop as a non-blocking write, with the poll
shortened to 5 ms while one record waits. The host role's `{tun, usbfs}` set does
poll, because usbfs implements it.

## Failure catalogue

Each of these is a string something actually prints. Reading the daemon's own log
is almost always faster than reasoning about the screen.

| what you see | what it means | where it is said |
|---|---|---|
| no cable entry in the Host menu, no cable row in Join | one of the four capability facts is missing; the startup line names which | `app` log, `cable link: …` |
| `this device's pak has no cable support` | the ELF is missing from `bin/<platform>/` — a partial install | status line |
| `the port did not switch to host (role not confirmed)` | the vendor role write did not take effect inside ~2 s, or wedged | status line |
| `cannot read <node>, so the port cannot be switched safely` | the current port mode is unreadable, so there is nothing to record and the write is refused | status line |
| `state=wait` and nothing happens | our side is up and the peer is not there: no cable, the other end not hosting, or a cold socket | `cable.status`, `Cable: no other device on the wire` |
| `no cable peer - is the other device hosting?` | the join exhausted its attempts without the peer appearing | status line |
| `the peer is not presenting a cable link` | something enumerated, but its interface is not our vendor class with two bulk endpoints | daemon log |
| `cannot claim interface N: …` | usbfs refused the claim — usually another driver bound it, which the unassigned VID/PID is meant to prevent | daemon log |
| `the cable link lost its framing` | a transfer arrived that cannot be a record — a truncated write, or a peer that is not this protocol | daemon log |
| `dropping a N-byte packet the link cannot carry` | an IP packet larger than MTU + header, refused rather than truncated | daemon log |
| `cannot bind the gadget to <udc> (EBUSY)` | something still holds the controller — usually a previous daemon that was killed | status line |
| `the gadget never presented its endpoints` | the strings blob was accepted but the endpoint files never appeared | status line |
| `the kernel refused the <descriptors\|strings> blob` | the blob is wrong for this kernel; the fields to check are the eventfd offset and the embedded length | status line |
| `could not put the port back to <mode>` | the port's real mode differs from the record; the record is **kept** and retried next launch | app log |
| `adb` stops working mid-session | expected: the controller holds our gadget, and `adb`'s holds nothing | — |
| `adb` is still dead after the session ended | the repair record is still there; the next app start repairs it, or a reboot does | `<state>/usb_restore` exists |
| `Put the USB port back after a cable session.` | the repair pass ran and succeeded at startup | status line |
| `Could not put the USB port back - a reboot will finish the repair.` | the repair failed; the record is kept | status line |

## What is verified where

**On the host** (`testing/test-cable.sh`, part of `make test`): the descriptor and
strings blobs byte for byte, the framing codec and the terminator rule, the
configfs path spellings, the status grammar and its truncation property; every
shared name compared across the daemon, the app, the shell and the harness; the
launcher guards; and the session writer.

**On the device**: everything above, plus the two-device checklist below. The
daemon is cross-built and links `netsetup.c`, so there is no host build of it —
`app/makefile` compiles `app/usbnet.c` with `$(CROSS_COMPILE)gcc` and no other
rule, which is why the lifecycle half of the host suite is gated on a binary the
machine can actually run and why that gate has never opened. It is there so that
a host build, if one is ever added, is exercised without another change; until
then the daemon's lifecycle is covered by the device checklist.

### Device checklist (two Bricks, one USB-C cable)

**Not yet measured.** These are the questions no desktop can answer, and they
should be run before anything above is trusted:

1. Does a USB-C-to-USB-C cable between two Bricks enumerate at all, and in which
   direction? (Two dual-role ports are a DRP tie; a cold socket is the expected
   failure.)
2. Does writing `/sys/devices/platform/soc/usbc0/otg_role` actually flip the port —
   and does it wedge its writer in D-state, as reported?
3. Does the host side enumerate the peer's gadget when the port is sourced by the
   device itself?

Then, in order:

- arm a host by hand (`link=cable`, `role=host`, `port=55437`) and read the log:
  the gadget built, the controller taken with a record written, `npc0` up as
  `10.77.0.1`, `state=wait` in `cable.status`;
- arm the peer by hand (`link=cable`, `role=client`, `peer=10.77.0.1`) and watch
  for `state=up` on both sides;
- `ping` across `10.77.0.0/24` in both directions, then the shim's TCP transport on
  55437 across the cable;
- end the session with X on both devices: `cable.pid` and `cable.status` gone,
  `usb_restore` gone, the firmware's gadget bound to the controller again,
  `npc0` gone, and `adb devices` listing the device;
- `SIGKILL` the daemon instead: `usb_restore` must still be there, holding both
  key pairs, and opening the app must repair it and restore `adb`;
- confirm the daemon does not busy-poll: `top` near 0 % CPU with a peer attached
  and no traffic;
- unplug and re-plug: state must return to `wait` and then to `up` without the
  daemon exiting.

## Not in this design

Deferred deliberately, in the order they are most likely to be wanted:

- **gadgetfs + hand-written CDC-ECM**, the only path that would produce a real
  `usb0`. It depends on `CONFIG_USB_GADGETFS`, unverified on this BSP, and carries
  a 2016 linux-sunxi report of gadgetfs on `sunxi_usb_udc` starting and never
  moving host data.
- **RNDIS through FunctionFS** — the only ethernet class the validator technically
  permits, and it needs a full userspace RNDIS stack plus an interrupt endpoint on
  a controller whose interrupt endpoint shares silicon with its isochronous one.
- **PPP or SLIP over the vendor bulk pipe**, and the **`adb forward` tunnel**.
- **DHCP over the cable**: point-to-point static addresses, exactly as the ad-hoc
  host already uses one.
- **UDP discovery over the cable** (port 55438): a cable peer is found by USB
  enumeration, not by broadcast.
- **Tuning `input_delay` from measured cable RTT.**
- **A PC-as-USB-host harness.** The development machine is macOS, where
  `/dev/net/tun` does not exist and usbfs cannot be reached.
- **Any kernel, firmware, u-boot or rootfs change**, and any `.ko`.

## Files

| file | what it holds |
|---|---|
| `app/usbnet.h` | the vocabulary the daemon, its pure layer and the app share: identity, framing, status grammar, path builder declarations |
| `app/cableproto.c` | the pure layer: descriptor and strings blobs, framing codec, terminator rule, configfs paths, status grammar |
| `app/usbnet.c` | the daemon: lifecycle, TUN, gadget role, host role |
| `app/netsetup.h`, `app/netsetup.c` | `NS_LinkKind`, the `link=` key, `NS_cable*`, the recovery pass, the cable check row, `NS_linkIP` |
| `app/main.c` | the third Host entry, the Join cable row, the kind-aware status line |
| `launcher/state-path.sh` | `netplay_link_kind`, and the shared "session ended" file set |
| `launcher/pre-launch.sh` | the link-kind guard on the WiFi power-save and the ad-hoc rejoin |
| `launcher/session-cleanup.sh` | the previous-boot pass, which drops `usb_restore` |
| `launcher/session.conf.example` | the `link=` key, documented |
| `testing/cableproto-unit.c`, `testing/test-cable.sh` | the host suites |
| `app/makefile`, `Makefile` | the third product |
| `tools/netplay-harness.py` | deployment of the ELF, and sessions that name the medium |

## References

- [adhoc.md](adhoc.md) — the existing transport, its measurements, and the
  "nothing invoked from a screen may be unbounded" rule this transport follows.
- [shim-architecture.md](shim-architecture.md) — the transport and session-file
  contract, unchanged by this work: `NetLink_configure` reads `role`, `port` and
  `peer`, and ignores every other key.
- [gb-link-optimizations.md](gb-link-optimizations.md) — input delay as a
  per-transport floor.
- `Documentation/usb/functionfs.txt` and `drivers/usb/gadget/function/f_fs.c` in
  Linux 4.9 — the descriptor validator, the ep0 write state machine, and the
  reason reads are maxpacket-multiples.
- `Documentation/driver-api/usb/usb.rst` — usbfs, including "only one ioctl
  request can be made on one of these device files at a time", which is why the
  host role submits asynchronous URBs.
- `.rpiv/artifacts/designs/2026-09-27_19-02-58_usb-otg-transport.md` — the design
  record, including the full external source list.
