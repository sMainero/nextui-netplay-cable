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

The maintained core is pinned from `bmpriest/gambatte-libretro`; the pin lives
in one place, `GAMBATTE_REV` in the Makefile, rather than being restated here
where it goes stale. Its versioned extension ABI keeps
the core responsible for two emulators and the local serial coordinator while
the Netplay shim owns devices, transport, identity, input lockstep, and policy.

The Netplay integration now:

- detects ABI v2 and its required capabilities at runtime;
- selects console A on the host and console B on the guest;
- loads the selected cartridge into both local consoles, or one cartridge per
  console when the two devices launched different linked games;
- exchanges each device's raw SRAM/RTC into the peer-owned logical console;
- exchanges both devices' wall clocks and gives each console the same pair of
  cartridge-clock epochs, so an RTC game shows each player their own time
  without the two replicas disagreeing about it;
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

## Fixed: the first serial byte aborted the host

A paired build crashed the moment the two consoles actually linked - not on
startup, not under load, but on the first byte that crossed the in-process
cable, which is why it survived every single-device test:

```
[Gambatte] GBLC serial exchanges=1 wait_avg=42us ... idle_polls=54109
realloc(): invalid old size
Aborted
```

`blipper_push_delta()` writes `taps` entries at `output_buffer[phase/decimation]`
without a bounds check, so a caller must drain before pushing a bufferful. The
frame loop does. The serial service slices - which only run when there *is*
serial traffic - accumulated a whole frame of audio and pushed it in one go.
Fixed in the fork (`693068a`, merged `533aab3`) by chunking and draining both
service paths the way the frame loop does; see
`cores/patches/superseded/gambatte-serial-audio-overflow.patch` for the ASan
trace and the arithmetic.

## Fixed: the core serialized uninitialised memory

The two replicas disagreed on the *first* comparison of every session - frame 0,
before a single frame had been emulated - with the transfer provably intact:

```
Brick (host):  PAIRED DESYNC at frame 0 (ours 442bc3cb, peer 60e19e23)
A30  (guest):  PAIRED DESYNC at frame 0 (ours 60e19e23, peer 442bc3cb)
```

Each side received the other's hash correctly, so the states really did differ.
Exactly **4 bytes** of the 119316-byte payload were responsible, at the same two
relative offsets inside each console's state: `sachenLockCount` and
`sachenOuterMask`.

gambatte declares `SaveState` on the stack and only default-initialises it.
Nearly every member is then written by the save path, but those two are written
only by the Sachen mapper, so for any other cartridge the serialized bytes
carried stack residue - `0x00/0x82` in one capture, `0x91/0x2f` and `0x8a/0x3f`
in the next two. `savestate.h` already called them "Unused (and zero) for every
non-Sachen cartridge"; nothing made them zero.

Invisible locally, because the loading mapper ignores those fields. Fatal here,
because it made `saveState()` not a function of emulator state: the same machine
serialized differently on consecutive calls, and two devices always differed.
Fixed upstream by value-initialising at the declaration sites.

**A correction worth recording.** This was first diagnosed as `saveState`
deliberately mutating - `CPU::saveState` does rebase `cycleCounter_` and fold
`hf1` into `hf2` - and the measured "ten serializes give a period-3 cycle" was
read as proof. That mutation is real but benign; the cycling was stack reuse.
The wrong diagnosis produced a real fix (the host now adopts its own checkpoint,
so both devices end on the same operation) which did not solve the problem,
because the problem was never the sequence. What exposed it was replaying each
device's bootstrap in *separate processes* and diffing the payloads byte for
byte, rather than simulating both roles in one process where the stack made them
agree by luck.

The shim keeps its symmetry discipline anyway: the state size is queried once at
the same protocol point on both sides, the host adopts its own checkpoint, and a
hash round is never skipped on one device only.

## CPU scaling is pinned for the duration of a session

A paired session emulates two consoles per device, and on the A30 that was being
done at 648MHz of an available 1344MHz.

NextUI's default CPU Speed is "Auto", which on my282 means the conservative
governor between 648 and 1344MHz with `up_threshold=80` over a 10ms window. A
frame-paced emulator never looks busy to that: it computes for two or three
milliseconds and waits for the next frame, so measured load sits around 12-20%
and the governor stays at its floor indefinitely. Under four full-load spinners
the same device ramps to 1344MHz in about two seconds and drops straight back,
so the headroom is real - nothing in a well-behaved emulator ever asks for it.

That also made every cross-device measurement misleading. The Brick runs pinned
at 2GHz on the performance governor, so "the A30 is the slower device" was
measured with one device at full clock and the other at 48% of its own ceiling.

The shim now pins every cpufreq policy to performance while a session is armed
and restores the previous governor and floor at teardown. It is done from the
frame loop rather than at startup because minarch applies its own
`minarch_cpu_speed` option *after* the core is loaded, so anything set earlier is
overwritten; and it is re-asserted every `PACING_INTERVAL` frames so a menu round
trip cannot hand the device back to the conservative governor mid-session. A
launch with no session armed is left alone entirely.

`NETPLAY_CPUFREQ_ROOT` overrides the sysfs root, which is what lets
`shim/test/run.sh` assert the pin, the restore, and the disarmed case against a
temporary tree instead of the real one.

## Fixed: the cable is resolved by emulated cycles

`NetplayLocalSerialBus` decided everything on wall-clock terms - `check()` was a
true poll answering from the peer's live state, `send()` gave up after 50ms of
real time and fabricated an `0xFF`, and two consoles clocking at once were
resolved by whichever won the mutex. On device the two handhelds did not even
agree on how many exchanges had happened: 385 on one and 0 on the other over the
same window, with a `send_timeouts=1` on one side only.

Decisions are now made on emulated cycles. `SerialIO` gained `advance(cc)`,
called from the CPU loop on every pass so a busy console still reports progress;
each endpoint has its own cycle-stamped request slot, so two consoles clocking
at the same cycle is a symmetric exchange rather than an arbitration and
requests at different cycles never pair; the 50ms deadline became four transfer
periods of cycles; and the 250us service poll became a blocking wait.

Three things had to be right beyond the design, each found by a reproduction
rather than by reading:

- **"Running" and "active" are different states.** A console that has finished
  its frame stops running but stays available to answer. Conflating them
  deadlocks: both finish, both wait for the other to go inactive, and neither
  can, because that happens after the wait.
- **Equal positions need a defined order.** Both consoles start a frame at
  position 0, and "wait until the peer is strictly ahead" made each wait for the
  other to move first. Endpoint 0 now acts first at any given cycle; the order
  is arbitrary, that it is fixed is the point.
- **Permitting an order is not enforcing it.** With the wait corrected the
  totals matched across runs while *which* check consumed a request still varied
  by arrival - the same rule had to govern what a check may take.

`cores/tests/run.sh` drives the bus directly with a fixed script under hostile
timing and asserts the answers are reproducible: 7 of 8 runs disagreed before,
40 of 40 identical after, with all 200 transfers completing every run where the
old bus completed 139-144 and a different set each time. It needs no ROM, no
device and no frontend, which matters because every failure here was otherwise
only reachable by getting two handhelds into a linking game.

`NETPLAY_BUS_STALL_MS` makes a stuck wait describe the whole bus state once.
Two threads asleep on a condition variable leave nothing to interrogate, and it
is what found the deadlock above.

On device this held for Tetris DX: 11,400 paired frames, 42,929 exchanges, no
send timeouts, 60.0 fps, no stalls. Super Mario Bros. Deluxe still desynced
almost immediately - see the frame barrier below.

## Fixed: a frame ends when the emulation says so, not when a thread exits

Tetris DX and SMB Deluxe fail differently, and the difference is the diagnosis.
Tetris DX answers every clock it is sent. SMB Deluxe clocks into a partner that
is often not listening - eleven unanswered sends in three hundred frames - and
an unanswered send was the one outcome the bus still decided by wall clock.

`send()` ended when the peer went inactive, and a console went inactive when its
thread finished its frame. So whether a transfer read the peer's byte or a
fabricated `0xFF` came down to which of the two threads reached the end of the
frame first. On top of that, `beginFrame()` rebases both consoles' positions,
so a request still outstanding across the boundary was suddenly being compared
against a position measured from a new origin.

Three changes, each with the same shape - replace a wall-clock question with an
emulated-state one:

- **A frame barrier.** `leaveFrame()` holds a console at the end of a paired
  frame until the peer is also ready to leave and nothing is left on the cable.
  It returns `false` when the peer has a request to service, so the caller
  emulates a slice and comes back; the wrapper does that on both sides before
  dropping activity.
- **A response is collected before anything else.** `send()` tested the peer's
  request slot before its own response slot, so a peer that answered and then
  immediately clocked a transfer of its own left the response stranded - and
  with it, a frame barrier that never opened. A delivered response is
  unconditionally ours.
- **`check()` honours the sender's deadline.** It would take a request its
  sender had already given up on, or was about to; which of the two happened was
  a thread race. `liveRequest()` is now the single definition of a request worth
  answering, shared by `check()`, `waitForService()` and `leaveFrame()`.

The test grew a second scenario for this - one console clocking into a peer that
never listens, across four frame boundaries - because the cooperative script
cannot reach the bug. Neither can an unloaded machine: both scenarios passed 20
of 20 runs before the fix and failed 34 of 60 under eight competing spinners,
which is now how the test is run.

On device this held. Two sessions run back to back on the A30/Brick pair, with
every state hash compared between the two handhelds:

| | SMB Deluxe | Tetris DX |
| --- | ---: | ---: |
| paired frames | 8,700 | 5,700 |
| agreement checkpoints | 30 of 30 | 20 of 20 |
| exchanges | 46,616 | 21,153 |
| send timeouts | 64 | 0 |
| frame rate | 60.0 fps | 60.0 fps |

The counters are the clearest evidence. `exchanges`, `send_timeouts` and
`idle_polls` were bit-identical on the two devices; `wait_avg`, `wait_max` and
claimed capacity were not, and the A30's worst wait was seven times the Brick's.
Every emulated-state quantity agreed and every wall-clock quantity differed,
which is the separation the whole rewrite was for. The 64 send timeouts are SMB
Deluxe clocking into a partner that is not listening - the case that used to be
decided by whichever thread exited first, now abandoned at the same emulated
cycles on both devices.

## Fixed: each cartridge keeps its owner's clock

A cartridge with an RTC - Pokémon Gold/Silver/Crystal, the Zelda Oracle pair,
Harvest Moon - asks the host what time it is, and the answer becomes emulated
state the moment the game latches it. Two handhelds are rarely set to the same
second; this pair was nine seconds apart, which is a different day counter and a
different savestate.

An earlier fix started a paired build from a fixed epoch, which covered power-on
only: `rtc.cpp` and `huc3.cpp` still called `std::time(0)` on every latch.

The clock is now injected rather than read. `gambatte::TimeSource` is a
per-console interface plumbed the same way `SerialIO` already was, and left
unset - every ordinary build, every ordinary session - the cartridge reads
`std::time(0)` exactly as before.

For a paired session:

- Each device declares its own wall clock in the session identity it already
  exchanges, so both sides learn both clocks.
- Both compute the same pair - console A the host's clock, console B the
  client's - and hand it to the core through `retro_dual_set_clock_epochs`.
  Each cartridge therefore shows its own owner's time of day.
- Time advances from those epochs by *emulated frames*, not by either device's
  clock, so both replicas compute the same second.
- The frame count and both epochs ride in the paired checkpoint header, which
  grows from 16 to 40 bytes. Without them a resync would leave the two devices
  agreeing on console state while disagreeing about what time it is - hidden
  until the next latch.

Setting the epochs is allowed with content already loaded, which is the normal
case: the peer's clock only arrives once the handshake has run. Each console's
cartridge clock is rebased by the amount its epoch moved, so elapsed time is
untouched - at power-on that turns the fixed epoch into the player's real clock
and changes nothing else, and a cartridge whose save carries a base time is
about to have it overwritten by that save anyway.

A paused game now has a stopped clock, which real hardware would not do. That is
the price of an answer two devices can both reach, and it is a much smaller
oddity than the two of them disagreeing about the date.

The fake core in the shim tests folds the epochs into its paired state the same
way the real core folds them into its checkpoint, so a shim that handed each
device its own reading instead of the agreed pair would fail the link test on a
state hash.

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
