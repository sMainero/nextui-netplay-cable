# Dual-instance link play

## Status

Dual-instance link play is experimental. The shipping Game Boy implementation
currently loads two copies of the patched Gambatte libretro core, links them over
localhost TCP, and sends only controller inputs between handhelds. It is correct
enough to establish and sustain a Game Link session, but it is not fast enough on
the tested A30/Brick pair.

The replacement described here is under development. It will remain a separate
prototype core until it passes correctness, performance, state-bootstrap, menu,
and disconnect tests. The existing Gambatte core remains the fallback.

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

## Current Gambatte implementation

The current implementation lives primarily in `shim/shim.c` and in three tracked
Gambatte patches:

- `gambatte-platforms.patch` adds the NextUI cross-build targets.
- `gambatte-network-hardening.patch` makes GameLink's two-byte TCP protocol safer,
  adds `TCP_NODELAY`, complete reads/writes, timeouts, and pacing diagnostics.
- `gambatte-local-instance.patch` adds private `Local Server` and `Local Client`
  modes and a short client rendezvous.

At launch, the shim copies the selected Gambatte shared object to `/tmp` so that
`dlopen` creates a second set of the core's file-scope globals. It loads the same
ROM into both cores, gives the visible and hidden instances distinct inputs, and
runs the hidden instance on a persistent worker thread. The two instances still
use Gambatte's `NetSerial` implementation over `127.0.0.1`.

The host's visible instance is the local server and its hidden instance is the
local client. The guest reverses those roles. Thus console identity is mirrored:
the same logical console has the same GameLink role on both handhelds.

### Bootstrap

The devices exchange core identity and initial visible-console state. Each peer's
visible state becomes the other peer's hidden state. Once both local pairs contain
the same console-A and console-B states, frame-tagged inputs are scheduled with the
negotiated delay and both local simulations advance.

### Persistence

Only the locally visible console may expose persistent SRAM to the frontend. The
hidden console uses a temporary save/system directory and must not overwrite the
visible console's files. Host SRAM may be supplied to a guest for the duration of
the session, but guest-side copies do not persist after the session.

Save states are not supported during netplay. Paired recovery checkpoints are an
internal protocol mechanism, not user save states.

## Measurements from the A30/Brick test

The persistent worker removed per-frame thread creation but did not make the
current approach playable:

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
matching ROM hashes. Each logical console's ROM identity must nevertheless match
its mirror on the other handheld:

```text
device A visible ROM == device B hidden ROM
device B visible ROM == device A hidden ROM
```

The first prototype may load the same ROM twice because that matches the current
launcher flow. Supporting different linked ROMs requires negotiating both ROM
identities and providing the peer's ROM content or locating an installed match;
that is a separate feature from the scheduler itself.

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

The initial Netplay integration now:

- detects ABI v1 and its required capabilities at runtime;
- selects console A on the host and console B on the guest;
- loads the same selected ROM into both local consoles;
- exchanges each device's raw SRAM/RTC into the peer-owned logical console;
- has the host establish one authoritative paired checkpoint before play;
- maps host input to logical A and guest input to logical B on both replicas;
- advances one paired core call per frontend frame, with no second DSO or
  shadow-core worker;
- encodes a link-console reset in the delayed input timeline so both replicas
  reset only the requesting player's logical console on the same frame;
- persists only the locally visible console through the standard libretro
  memory API; peer memory remains in-process and disappears at teardown;
- falls back to the ordinary network-serial Gambatte core when the requested
  paired artifact or ABI is unavailable.

The release package carries `gambatte_dual_libretro.so` per platform beside the
ordinary Gambatte and gpSP implementation cores. These are implementation
artifacts, not compatibility-core fallbacks.

Still required before treating the path as complete:

- periodic paired-state agreement checks and host-authoritative correction;
- checkpoint persistence and correct host/guest process-crash rejoin;
- explicit Continue Solo semantics for an in-process cable;
- different-ROM discovery/loading; until then that case uses network serial;
- device testing of menu pause, reset, disconnect, repeated launch, SRAM
  ownership, and thermal/power behavior on ARMv7 and AArch64.
