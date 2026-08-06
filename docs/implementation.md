# Implementation review

Status of the shim-based rewrite, what is proven, what is assumed, and what is
left. For how the shim works, see [shim-architecture.md](shim-architecture.md).

Written after the first working GB and GBA link sessions between a TrimUI Brick
(tg5040) and a Miyoo A30 (my282).

## Where the original pak stood

`Netplay.pak` shipped three patched binaries — `minarch.elf`, `gambatte` and
`gpsp` — built against one exact NextUI build. Every NextUI release meant six
rebuilt binaries and a merge against a moving `minarch`. It offered two
features:

| Feature | Screens | Cores |
|---|---|---|
| **Netplay (LAN)** | shared, inputs synced | any — FBNeo, FCEumm, Snes9x, Supafaust, PicoDrive, PCSX, gpSP, Gambatte |
| **Link** | independent | GB/GBC (gambatte), GBA (gpsp) |

## Where we are

**`minarch.elf` no longer needs patching.** A shim sits between minarch and the
core, forwarding all 25 libretro entry points and intercepting only what
multiplayer needs. With no session armed it is a pure passthrough and the game
behaves exactly as it would without it.

**gambatte still has to ship patched**, but only for a build flag: NextUI builds
it without `HAVE_NETWORK`, so Game Link is compiled out and no configuration can
reach it. Same upstream commit, one flag, plus a serial receive timeout we added.

**gpSP works unpatched**, for the games it supports — four reverse-engineered
link protocols plus generic wireless-adapter (RFU) emulation.

### Proven on hardware

- Passthrough on stock NextUI, both architectures, with `core.name`, config and
  SRAM paths identical to stock.
- GBA link (gpSP, Advance Wars 2, `mul_aw2`): a 213-second unbroken session,
  ~20k packets each way.
- GB link (gambatte, Tetris): connects and plays; latency-bound, see below.
- Two concurrent emulator instances per device — see [Dual instance](#dual-instance-explored-deferred).

### Verified host-side only

- Three test suites, all with regressions that were confirmed to fail against
  the bug they cover.
- tg5050 builds but **has never run** — no hardware.

### Not tested at all

- Tier 2 bind-mount (`wrap-pak.sh`) — never run on a device, so FBN, SUPA, 32X,
  SEGACD, GG, SMS, SG1000 and FDS are uncovered.
- Five of the seven stub-covered systems have never been launched.

## What is missing

### 1. Shared-screen netplay does not exist

Half the original feature set. NES, SNES, Mega Drive, PS1 and FBNeo have no link
cable — their support in the original pak is the *shared-screen* mode: both
devices run the same game in lockstep, each player owning a controller port.

This is not an unknown, it is unbuilt. It needs the same input-sync layer as
everything else: shared-screen is one instance with synced inputs; GB link is two
instances with synced inputs.

**It covers seven systems rather than two, and it is the more tractable half.**

### 2. There is no user interface

Sessions are hand-edited config files. Joining a game means typing an IP as
twelve separate single-digit core options. The original pak had host/join menus,
UDP discovery, hotspot creation, an on-screen keyboard and QR codes.

Session setup belongs in the pak app, because the shim cannot reach minarch's
`GFX_`/`PAD_` (see shim-architecture.md, "What does not survive"). That app does
not exist yet.

### 3. Two gpSP fixes from the original pak are not applied

The pak ships gambatte but no gpSP, so NextUI's stock core is used and these are
missing:

- `001-rfu_disconnect_fix` — unsticks the RFU state machine on disconnect
- `002-rfu_queue_size` — packet queue 4 → 16, for TCP delivering bursts faster
  than the game drains

The second is directly relevant to observed GBA link stutter. `make cores`
already builds a patched gambatte; extending it to gpSP is small.

### 4. Determinism and state sync are unexamined

Prerequisite for any input-synced design, and untested:

- **Determinism** — identical results required on an A53 and a Cortex-A7,
  different compilers, especially in floating-point audio paths.
- **RTC cartridges** — Pokémon G/S/C read a real clock; divergence is immediate
  unless pinned and synced.
- **Save data** — simulating a peer's cartridge needs their SRAM, which for
  trading games is the entire point of playing.
- **Divergence detection** — without periodic state hashing a desync is silent.

This is the likeliest thing to sink the dual-instance plan. CPU is not.

### 5. Housekeeping

- WiFi power save reverts on reboot; the pak should manage it for a session.
  Measured impact: 307 ms → 12 ms average RTT.
- Nothing is committed; work spans two repos.

## Network findings

Latency is **airtime contention, not topology**. Two hops measured no slower
than one:

| Path | min | avg | max |
|---|---|---|---|
| A30 → AP (one hop) | 7.6 | 36.5 | 109 |
| Brick → AP (one hop) | 7.0 | 41.3 | 143 |
| A30 → Brick (two hops) | 7.4 | 31.6 | 130 |

26 access points on 2.4 GHz, four on the same channel and nine on the
overlapping channel 6. Neither radio supports 5 GHz.

An ad hoc link would help by getting a **clear channel**, not by removing the
router — and it matters far less under an input-synced design, where serial
never crosses the network. Input delay absorbs latency that blocking serial
cannot.

## Why GB link is latency-bound

gambatte's `NetSerial` is asymmetric by design:

- **master** (`send`) — write, then a **blocking** read: one full round trip per
  serial transfer, on the emulator thread.
- **slave** (`check`) — `ioctl(FIONREAD)`; if nothing has arrived, return and
  carry on.

Identical bytes each way; only the master waits. At 35 ms average RTT the master
is capped near 25 transfers/second against a 16.7 ms frame budget, which is the
stutter. Confirmed on device: the lag follows whichever unit initiates the match,
not the hardware.

The master/slave role is set by the game writing bit 0 of the Game Boy `SC`
register. It is unrelated to which end is the TCP server, so config cannot swap it.

## Why GBA link only covers some games

gpSP does not emulate the link cable. From `serial_proto.c`:

> Since real link cable emulation is very hard, here we partially emulate the
> other devices (GBAs) and using some fake data and real data recreate some
> serial protocols.

So there are two different things:

- `mul_poke`, `mul_aw1`, `mul_aw2` — per-game shims, each reverse-engineered
- `rfu` — generic wireless-adapter emulation, works for any adapter-aware game

Auto-detection matches the ROM header and covers **only the Pokémon family**;
Advance Wars modes must be selected explicitly. Games using raw multi-player SIO
— Mario Kart Super Circuit among them — cannot work, and no netcode changes that.

## Dual instance (explored, deferred)

Run both consoles locally and network only controller inputs. Serial becomes an
in-process link at zero latency; input delay absorbs network latency.

Measured, headless, Advance Wars 2 / Tetris:

| | one instance | two concurrent |
|---|---|---|
| gambatte, A30 | 915 fps (15×) | 884 + 904 fps |
| mGBA, Brick | 291 fps (486%) | 267 + 267 fps |
| mGBA, A30 | 141 fps (234%) | 142 + 138 fps |
| gpSP, A30 (reference) | 483 fps (805%) | — |

**Capacity is not the constraint.** Even the A30 runs two mGBA instances at
~2.3× realtime, at full clock. gambatte's save state is 26,882 bytes, so
rollback buffers are trivial.

mGBA is the interesting target because it has *real* cycle-accurate SIO for every
mode (`GBA_SIO_MULTI`, `NORMAL_8/32`, `UART`, `JOYBUS`) in `src/gba/sio/lockstep.c`
— 1,100 lines, already shipping in its Qt and SDL frontends as "local (same
computer) link cable support". Its README lists *networked* link as a planned
feature, and the libretro adapter exposes **neither** — zero references to
lockstep.

So the missing piece is a libretro adapter that creates two instances and joins
them to a lockstep coordinator. The hard parts exist on both sides. But it is a
real fork of a core, with real maintenance, and determinism (§4) decides it.

**Deferred.** Interesting, not near-term.

## Durability: when does a frozen pak stop working?

The architecture trades "breaks every NextUI release" for "breaks only if one of
these four assumptions changes".

| # | Assumption | If it changes | Fails |
|---|---|---|---|
| 1 | Emu paks invoke `minarch.elf` **unqualified**, so PATH resolves it | our wrapper is never reached | silently, safely — stock behaviour |
| 2 | `getEmuPath` prefers SD `Emus/<PLATFORM>/<TAG>.pak` over system paks | stubs never found | silently, safely |
| 3 | `core.name` = `basename` truncated at last `_`, feeding `config_dir` and `states_dir` | staged shim yields a different name | **silently, and relocates save states** |
| 4 | The libretro core ABI — 25 `retro_*` entry points | a newly required symbol is missing from the shim | loudly, at core load |

Also load-bearing but lower risk: path layout (`.system/<platform>/bin`,
`Tools/<platform>`, `Emus/<platform>`), and the toolchain ABI (the shim needs
only libc/libdl/libpthread; gambatte adds libstdc++).

Degradations rather than breakages: a new platform ships no shim; new systems
fall outside the core allowlist; a `launch.sh` format we cannot parse installs no
stubs.

**Assumption 3 is the only one that can hurt user data**, and it is the one worth
defending. Save states are otherwise safe: our gambatte and NextUI's are the same
upstream commit and `HAVE_NETWORK` guards contain no serialize code, so states
interchange freely.

### Recommended defence

Make the pak **verify its assumptions when arming** and refuse rather than
proceed silently:

- confirm the target `launch.sh` invokes `minarch.elf` unqualified
- confirm the SD override path actually wins
- confirm a staged shim name yields the expected `config_dir`/`states_dir`
- record the NextUI build validated against, and warn on mismatch

That converts three silent failure modes into one clear message, and is far
cheaper than the alternative.

## Suggested order

Scope discipline: ship the working half before extending.

1. **Apply the two gpSP patches** — small, already-built machinery, fixes real
   observed stutter.
2. **Session UI in the pak app** — host/join, discovery, no hand-edited configs.
   The gate on releasing anything.
3. **Arm-time assumption checks** — cheap, and turns silent breakage into a
   message.
4. **Shared-screen netplay** — seven systems, reuses the input-sync layer.
5. **Hotspot / clear channel** — helps GB link, matters less after 4.
6. **Dual instance** — deferred; determinism decides it.

The risk to watch: the project has drifted toward the hardest, most novel corner
(link over a network) while the broader, more tractable half — shared-screen
netplay for seven systems — has not been started.
