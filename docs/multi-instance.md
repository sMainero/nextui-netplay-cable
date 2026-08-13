# Dual-instance link play

## Status

Dual-instance link play is experimental but is now built on the paired core:
`gambatte_dual_libretro.so` holds both logical consoles and the cable between
them, and one `retro_run` advances the pair. Wi-Fi carries only delayed,
frame-indexed controller inputs.

It is selected only when both devices agree at arm time and both can host both
cartridges at launch; otherwise the session runs ordinary network serial. It has
not yet had the device testing listed at the end of this document.

The earlier implementation - two copies of the core linked over localhost TCP -
is retired. Its measurements are below because they are the argument for the
current design, and the code is retained, disabled, in `shim/shim.c`.

## User-visible goal

Each handheld should emulate both Game Boys locally while showing and accepting
input for only its assigned Game Boy:

```text
Handheld A                         Handheld B
+-------------------------+       +-------------------------+
| visible console A       |       | hidden console A        |
| hidden console B        |       | visible console B       |
| local link A <-> B      |       | local link A <-> B      |
+-------------------------+       +-------------------------+
             \                         /
              +-- frame-tagged input -+
```

Wi-Fi carries inputs, session control, identity, and recovery data. It does not
carry Game Boy serial traffic. Both devices independently reproduce the same
link-cable transaction from the same two emulated states and input streams.

This design is only for link-cable mode. Shared-screen netplay continues to run
one authoritative simulation with synchronized controller inputs and checkpoints.

## Why two instances

An actual link game contains two independent consoles. Each console can run a
different ROM, own different SRAM, enter or leave the multiplayer area, and use
the link clock differently. Treating link play as two simulations preserves those
semantics and avoids putting individual serial-byte round trips on Wi-Fi.

Mirroring both simulations on both handhelds also means normal wireless jitter is
absorbed by the existing input delay rather than stalling every serial transfer.
The cost is that every handheld must have enough CPU for two emulator instances.

## Retired: the two-copy implementation

The first implementation copied the selected Gambatte shared object to `/tmp` so
`dlopen` would give it its own file-scope globals, loaded the same ROM into both
copies, gave them distinct inputs, ran the hidden copy on a persistent worker
thread, and linked them with Gambatte's own `NetSerial` over `127.0.0.1`. The
host's visible copy was the local server and its hidden copy the local client;
the guest reversed those roles, so the same logical console held the same
GameLink role on both handhelds.

Everything it needed from the core - the cross-build targets, the GameLink
network hardening, and the private `Local Server`/`Local Client` modes - now
lives in the pinned `bmpriest/gambatte-libretro` fork rather than in tracked
patches. The patches that introduced them are kept for reference under
`cores/patches/superseded/`; no build rule applies them.

The shim-side code is kept, disabled behind `#if 0`, next to the live paired
implementation in `shim/shim.c`. It is the shape a future core that cannot host
two consoles itself would need again.

### Persistence

Only the locally visible console exposes persistent SRAM to the frontend. In the
paired core that is enforced by the core itself: `retro_get_memory_*` resolves to
the visible console, and the peer console's memory exists only in-process and is
discarded at teardown. Each device keeps its own save, as it would with a real
cable.

Save states are not supported during netplay. Paired checkpoints are an internal
protocol mechanism, not user save states.

## Measurements from the A30/Brick test

These are the two-copy numbers. The persistent worker removed per-frame thread
creation but did not make that approach playable:

| Device | Visible role | Visible core | Hidden role | Hidden core | Pair | Speed |
| --- | --- | ---: | --- | ---: | ---: | ---: |
| A30 | client | about 18 ms | server | about 33 ms | about 33 ms | 22-23 fps |
| Brick | server | about 43 ms | client | about 8 ms | about 43 ms | 22-23 fps |

Local GameLink reads themselves averaged only about 0.10-0.25 ms and recorded no
timeouts. Wi-Fi input stalls were near zero on the Brick. The expensive execution
follows the GameLink server/clock-owner role; rendering and audio add roughly 10 ms
when that role is also visible. Consequently, further TCP tuning is unlikely to
recover the missing performance by itself.

## What mGBA does differently

mGBA's working multi-instance support is implemented by its native Qt frontend,
not by its libretro frontend. Each console owns a persistent `mCoreThread`. A
shared in-memory lockstep coordinator exchanges serial data and controls how far
each emulator may advance in emulated cycles. A console sleeps only when it reaches
a meaningful serial synchronization point and wakes when its peer catches up.

For Game Boy, mGBA supports two nodes and dynamically assigns link-clock ownership
to whichever console initiates an internal-clock transfer. For GBA, it supports up
to four nodes, timestamped event queues, mode changes, attach/detach events, and
periodic hard synchronization.

Neither `mgba-master` nor `mgba-libretro` exposes this through libretro. Both
libretro implementations own one global `mCore`, and both implement
`retro_load_game_special()` as a stub returning false. Reusing mGBA for NextUI
would therefore require a new dual-instance libretro wrapper or a standalone
runner.

The key lesson is not merely "replace sockets with memory." mGBA coordinates at
emulated serial events and can yield/resume each instance mid-frame. Gambatte's
current `SerialIO::send()` is synchronous, while `SerialIO::check()` is polled by
the external-clock console. That relationship can leave the clock owner blocked
inside `GB::runFor()` until the peer advances.

## Gambatte replacement design

The proposed core is a separately named netplay-only libretro build:

```text
gambatte_dual_libretro.so
  +-- Gambatte GB A
  +-- Gambatte GB B
  +-- input getter A (libretro port 0)
  +-- input getter B (libretro port 1)
  +-- in-memory SerialIO endpoints
  +-- paired scheduler
  +-- visible video/audio selector
  +-- paired diagnostic counters
```

The pinned Gambatte wrapper already contains a dormant `DUAL_MODE` technology
demo. It creates two `GB` objects and loads the same ROM into both, but currently:

- gives both instances the same input;
- does not connect their serial interfaces;
- advances them sequentially;
- renders them side by side;
- exposes SRAM, cheats, audio, and state for only the first instance.

That code is the initial scaffold, not a completed multiplayer implementation.
The prototype build must be separately named and must not replace the shipping
`gambatte_libretro.so` until validated.

### Phase 1: wrapper and transport prototype

1. Enable the dormant second `GB` only in the prototype build.
2. Give each instance a distinct cached input state from libretro ports 0 and 1.
3. Replace localhost TCP with two in-memory `SerialIO` endpoints sharing a bounded
   coordinator.
4. Advance the two instances concurrently with persistent workers.
5. Display/audio-output only the selected local console; discard hidden output.
6. Add timings for each instance, paired execution, serial waits, rendezvous
   misses, and produced-frame skew.

This phase answers whether socket removal and tighter ownership are sufficient.
It does not assume they are.

### Phase 2: cooperative serial scheduling

If the clock owner remains slow, change the wrapper/engine boundary so a serial
wait yields control instead of blocking an OS thread. The scheduler can then run
the peer until it reaches the matching serial event and resume the initiator.

Likely engine changes are:

- include emulated timing information in the serial callback;
- let `GB::runFor()` report a serial-wait yield;
- retain and resume the interrupted execution state;
- track per-instance cycle allowance and frame production;
- resolve simultaneous internal-clock attempts deterministically.

These should be targeted changes around Gambatte's existing serial-event
scheduler, not a rewrite of its CPU, cartridge, video, or audio emulation.

## Libretro contract for the prototype

- Port 0 is console A and port 1 is console B.
- The shim maps local and remote input onto those ports according to negotiated
  console identity.
- Only the locally selected console produces frontend video and audio.
- `retro_get_memory_*` exposes only the locally visible console's temporary or
  persistent memory according to session role.
- Reset must identify the target console. Link-mode reset remains local-device
  only by policy, so the wrapper resets the visible console unless explicitly
  instructed otherwise.
- User save-state commands remain unavailable during the session.
- Recovery state must contain both emulator states plus coordinator state. A
  single-console state is insufficient once a serial transfer is in flight.

## ROM identity

Link mode requires matching link protocol and core build, but does not require
matching ROM hashes - Red links to Blue, Seasons links to Ages. Each logical
console's ROM identity must nevertheless match its mirror on the other handheld:

```text
device A visible ROM == device B hidden ROM
device B visible ROM == device A hidden ROM
```

No ROM crosses the network, so this holds only when both cartridges are already
installed on both devices. That is what the runtime agreement below establishes.
When it does not hold, the pairing is declined and the session runs ordinary
network serial instead, which needs only one cartridge per device.

## Correctness requirements

Before replacing the current implementation, the prototype must demonstrate:

- stable boot and link establishment on ARMv7 and AArch64;
- byte-correct transfers in both clock-owner directions;
- no deadlock when either console finishes a video frame before a transfer;
- deterministic mirrored state hashes at regular checkpoints;
- correct pause/menu behavior when either frontend stops calling `retro_run()`;
- clean peer departure and Continue Solo behavior;
- no writes from the hidden instance to persistent storage;
- clean reset, unload, and repeated-launch teardown;
- bounded frame skew and no unbounded input or serial queues.

## Performance gates

The first target is sustained 59.7 fps on a single A30 with two local DMG
instances and no Wi-Fi. Network testing should begin only after that local gate is
met. Diagnostics must separate:

- visible engine time;
- hidden engine time;
- frontend video callback time;
- frontend audio callback/resampling time;
- time blocked on the in-memory serial coordinator;
- time waiting for remote input;
- frame skew between local instances.

If two unlinked Gambatte instances cannot meet the local gate, scheduler work will
not fix the device CPU budget. If unlinked instances pass but linked instances do
not, the serial scheduling boundary is the optimization target.

## Risks and fallback

- The dormant dual mode is a technology demo and has incomplete lifecycle/state
  handling.
- A condition-variable transport may still reproduce the synchronous clock-owner
  stall even after removing TCP.
- Allowing one instance to run ahead to service serial traffic can consume future
  input or overwrite its latest video frame unless explicitly bounded.
- Two emulator instances increase CPU use and thermal/power load on weaker
  handhelds.
- Paired recovery is larger and must be atomic across both emulators and the link
  coordinator.

Until all gates pass, negotiation must be able to select the existing GameLink
core. Failure of the experimental core must never prevent ordinary link mode or
single-player Gambatte from launching.

## Standalone GBLC feasibility pak

`make gblc-pak` builds `dist/GBLC-my282.pak.zip`. The archive expands at the SD
card root into:

```text
Emus/my282/GBLC.pak/
Roms/Game Boy Link Cable (GBLC)/
```

This is a single-device CPU and serial-scheduler test, not yet a two-device
netplay launcher. It loads the selected ROM into both instances and presents
them side by side. `GBLC_MIRROR_INPUT=1` normally operates both copies; holding
L2 directs controls only to console A and holding R2 directs them only to console
B. Independent steering is necessary because identical ROM, state, and input can
make both games claim the internal link clock simultaneously. It still cannot
test different linked cartridges.

The core writes cumulative diagnostic lines to
`.userdata/my282/logs/GBLC.txt` after 60 emulated frames and every 300 frames:

- `GBLC perf` separates primary, secondary, and paired average/maximum execution
  time, estimates core-only frame capacity, and reports worker creation failures;
- `GBLC serial` reports completed transfers, average/maximum synchronous sender
  wait, bounded send timeouts, simultaneous-clock arbitrations, and nonblocking
  external-clock idle polls.

The prototype advertises no save-state support. Until a state atomically contains
both GB instances and the coordinator's in-flight transaction, loading or
auto-resuming a one-console state would silently invalidate the test. Visible
console SRAM remains available, but test saves should be treated as expendable.

The wrapper must also reattach its private serial endpoints after every Gambatte
core-option refresh. Gambatte's stock option handler owns `gb.setSerialIO()` and
otherwise replaces the private endpoint with `NULL` whenever its ordinary
network-link option is disabled, silently disconnecting the local cable after
startup.

The phase-one coordinator tracks whether each core is actively inside
`GB::runFor()`. `checkSerial()` itself remains nonblocking. When one core reaches
its frame boundary first, it stays available while its peer is still running;
each late peer request causes a 32-audio-sample service slice. This permits the
finished core to answer serial traffic without running and discarding another
complete video frame. It is still a bounded wrapper scheduler, not the final
cycle-aware yield/resume design.

## Production frontend contract (`netdual7`)

The maintained core is pinned from `bmpriest/gambatte-libretro` at commit
`70f86206f3a903f91892fb873220253a1b718d26`. Its versioned extension ABI keeps
the core responsible for two emulators and the local serial coordinator while
the Netplay shim owns devices, transport, identity, input lockstep, and policy.

The Netplay integration now:

- detects ABI v1 and its required capabilities at runtime;
- selects console A on the host and console B on the guest;
- loads the selected cartridge into both local consoles, or one cartridge per
  console when the two devices launched different linked games;
- exchanges each device's raw SRAM/RTC into the peer-owned logical console;
- has the host establish one authoritative paired checkpoint before play;
- maps host input to logical A and guest input to logical B on both replicas;
- advances one paired core call per frontend frame, with no second DSO or
  shadow-core worker;
- pins `gambatte_gb_link_mode` to `Not Connected` so the core's ordinary network
  Game Link never opens a socket beside the in-process coordinator;
- encodes a link-console reset in the delayed input timeline so both replicas
  reset only the requesting player's logical console on the same frame;
- persists only the locally visible console through the standard libretro
  memory API; peer memory remains in-process and disappears at teardown;
- treats a lost link as recoverable, offering the same Wait / Continue solo /
  Exit choice shared-screen sessions use, and re-pairing from scratch on
  reconnect rather than resuming a timeline the peer cannot vouch for.

The release package carries `gambatte_dual_libretro.so` per platform beside the
ordinary Gambatte and gpSP implementation cores. These are implementation
artifacts, not compatibility-core fallbacks, and the launcher says so: the
startup notice reads "dual-instance core" for the paired build and "net-enabled
core" for the packaged network-serial builds, leaving "compatibility core" to
mean what it says.

### Agreeing to pair

Instanced play cannot be one-sided - a device running the paired core against a
device running network serial has nothing to talk to - so it is agreed twice.

At arm time the setup app already exchanges core manifests on `NS_CORE_PORT`.
That exchange now also carries whether each device has Gambatte instancing
switched on *and* has `gambatte_dual_libretro.so` staged. `instanced_gambatte=1`
is written into both session files only if both say yes, so the common mismatch -
one player has the setting off, or an older pak - never reaches a running game.

At launch the two shims exchange cartridge identity, including ROM size. If the
peer's cartridge differs from ours, each side searches its own `Roms` tree for a
file of that exact size and then confirms the SHA-256; the size comes over the
wire precisely so this is a directory walk and a hash or two rather than hashing
a library. Archives are not opened, so a zipped copy of the peer's cartridge does
not count. Each side then sends a verdict and both must say yes:

```text
A can host B's cartridge  AND  B can host A's cartridge  ->  instanced
otherwise                                                ->  network serial
```

A no is not a failure. The shim writes the file named by `NETPLAY_SERIAL_FALLBACK`
and asks the frontend to shut down; `launcher/minarch.elf` sees the marker,
relaunches the same game once with `NETPLAY_DUAL_DISABLE=1`, and the ordinary
network-serial core takes over. Once, deliberately: the second attempt cannot ask
for a third.

### Paired-state agreement

Both replicas hold the same two consoles, so both hash them - there is no
authority here the way there is in shared screen, and nothing to recover to
either: an in-process cable has no resync protocol. Every `HASH_INTERVAL`
frames each device serializes the pair, hashes it, and sends the value; the
comparison happens whenever the peer's hash for that frame arrives, so a late
hash is not itself a source of divergence. The first check is at frame 0, which
is the assertion that the bootstrap checkpoint actually equalised both devices.

A mismatch ends link play with `Consoles have diverged` and the ordinary
Wait / Continue solo / Exit overlay. That is worth having even though it cannot
repair anything: before it existed a divergence was silent, and both players
simply watched the other's console do things it had never done.

## Known broken: the coordinator is not deterministic

Mirrored replicas require the paired core to be a pure function of (initial
state, input sequence). `NetplayLocalSerialBus` is not, in four places:

| Where | Why it diverges |
| --- | --- |
| `waitForService(ep, 250)` | a timed wait; whether a service slice runs at all is thread scheduling |
| the service-slice loop | runs `runFor(..., 32)` a wall-clock-dependent number of times, so consoles advance different cycle counts for the same frame |
| `send()` 50ms timeout | fabricates an `0xFF` into the emulated serial stream on one device only |
| simultaneous-clock arbitration | "first claimant" is whoever wins the mutex |

A fifth sits in the wrapper: `retro_run`'s frame-dupe gate returns without
advancing either console, and `libretro_samples_count` is fed only by the
*visible* console - console A on the host, console B on the guest - so the two
devices skip on different frames while the shim advances `dual_frame` on both.

This is fine for the single-device GBLC feasibility pak, where there is one
simulation and nondeterminism is invisible. It cannot work for two devices, and
it is why Tetris DX desynced immediately on an A30/Brick pair: the two SoCs
never make the same scheduling choices. Fixing it is the phase-two
cycle-aware yield/resume design, not a patch.

### Measured: the CPU was never the problem

From the same failing session, per paired frame:

| | Brick (tg5040) | A30 (my282) |
| --- | ---: | ---: |
| core: primary / secondary / pair | 1.38 / 1.37 / 1.59 ms | 2.23 / 2.20 / 2.43 ms |
| core: claimed capacity | 631 fps | 412 fps |
| shim: measured paired call | 16.3 ms | 16.4 ms |
| observed | 53.6 fps, 11% stalls | 48.0 fps, 20% stalls |

Emulating both Game Boys costs 1.6-2.4ms. The other ~15ms is the two threads
waiting on each other: `idle_polls` climbs at a steady 60 per frame and each
poll is a `waitForService` that times out at the full 250us. 60 x 250us = 15ms,
which accounts for the whole deficit on both devices.

So both handhelds have six to ten times the headroom two consoles need, and the
22-23fps of the two-copy implementation and the 48-53fps of this one were never
CPU limits. They are the wrapper scheduler polling. The performance gate and the
determinism requirement have the same fix.

Still required before treating the path as complete:

- a deterministic serial coordinator (the above);
- checkpoint persistence and correct host/guest process-crash rejoin;
- device testing of menu pause, reset, disconnect, repeated launch, SRAM
  ownership, linked-cartridge pairs, and thermal/power behavior on ARMv7 and
  AArch64.
