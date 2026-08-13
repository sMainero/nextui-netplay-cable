# Netplay feature catalogue

This is the product-level catalogue for Netplay.pak. It records what a feature
means, how it relates to the current architecture, and what remains before it
can be called complete. Protocol correctness and recovery work are tracked in
[implementation-plan.md](implementation-plan.md); system/core limitations are
tracked in [game-systems.md](game-systems.md).

Status vocabulary:

- **Implemented** — present in the pak, though real-device coverage may remain.
- **Partial** — a usable part exists, but the described feature is incomplete.
- **Planned** — intended behavior has been agreed but implementation has not
  started.
- **Frozen** — deliberately visible or retained, with execution disabled.
- **Needs decision** — product behavior is ambiguous and must not be guessed.

## Core play modes

### 1. Shared-screen netplay — Implemented, hardening in progress

Both devices run the same ROM and compatible core. The host and guest exchange
frame-numbered controller input and advance only when the required input is
available. The host is authoritative for initial state, SRAM/RTC, desync
recovery, process rejoin, and shared-screen reset.

Required invariants:

- Identical canonical ROM content; filenames and local paths may differ.
- Matching protocol mode, core behavior, serialized-state geometry, save-memory
  geometry, and negotiated input delay.
- Both devices perform state hashing/serialization symmetrically.
- The guest never persists host SRAM/RTC or frontend save states.

Current recovery includes five-second agreement checks, peer-confirmed host
checkpoints, host-authoritative resync, and host/guest process rejoin. Remaining
race, timeout, and failure-reporting work is P0 in the implementation plan.

### 2. Link-cable/RFU play — Partial for gambatte and gpSP

Each handheld runs its own game and machine state; the shim carries the core's
native serial or RFU packets. Link identity requires the same compatible link
protocol/core but deliberately permits different ROM hashes, because paired
games may legitimately differ.

On connection loss, local gameplay continues and best-effort reconnect remains
available. Reset affects only the local device. The pak must not attempt to
authoritatively copy emulator state in this mode. Packet transport works, but
the link-specific preflight that proves matching protocol/core while permitting
different ROM hashes is not implemented yet.

The packaged Gambatte build uses deadline-bound complete two-byte transfers,
`TCP_NODELAY`, and periodic exchange-latency diagnostics. Unrelated core-option
changes do not restart its Game Link socket. A live peer may remain in its menu
without a fixed timeout; transport disconnect detection releases the other
device if that peer actually disappears.

### 3. Continue solo — Partial

When a peer is absent long enough to make progress impossible, present an
explicit choice rather than freezing forever:

- keep waiting/retrying;
- continue this game solo;
- exit the current game.

Continuing solo applies to the current emulator process. The durable session
stays armed by default for later launches, and save-state/guest-persistence
restrictions remain in force until the process exits; changing persistence
policy halfway through a running game is unsafe.

The shared-screen shim now times out an unavailable or input-silent peer and
offers Wait/retry, Continue solo, and Exit game from its failure overlay. Left
and Right select, A confirms, and B is a direct Wait/retry shortcut. Exiting the
game deliberately leaves the durable session armed: the shim cannot safely
restore an ad-hoc radio or remove mounts owned by the setup process. Equivalent
link-mode UX and an explicit end-session action remain planned.

## Setup, discovery, and platform support

### 4. Ad-hoc network creation — Implemented, hardening in progress

An AP-capable host creates `nextui-XXXX`; a guest finds it by SSID scan and
joins using the fixed session password. The host is `10.0.0.1`, currently with
`udhcpd` assigning the guest address. LAN announcements and direct SSID scans
are merged into one Join list without losing the visible SSID.

The implementation captures the original supplicant command before moving the
guest radio, persists a recovery breadcrumb, restores original Wi-Fi on session
end/failure, and runs a detached watchdog if the AP disappears. Remaining work
includes reconstructing AP state after an app crash or relaunch. Static guest
addressing may eventually remove the `udhcpd` dependency.

### 5. Persistent hosting/session broker — Planned

Hosting should outlive the Hosting screen and setup-app process. A small
detached broker, started when a host arms, should own:

- LAN announcements and ad-hoc session metadata;
- compatibility negotiation;
- game offers and replies;
- peer admission, stable peer identity, capacity and `BUSY` replies;
- broker status for Netplay.pak and a future NextUI/MinArch integration;
- clean shutdown when the session ends.

The AP already survives the setup app and its beacon overhead already exists.
A small UDP announcement and idle listener add negligible load; lifecycle,
pairing/authentication, and stale-session cleanup are the real risks.

Pressing B on Hosting leaves the screen without ending the session. Hosting
continues advertising where possible. Selecting Host for an already-hosting
session re-enables advertising if necessary and displays connected peers.

### 6. Compatibility cores — Implemented

The locally installed core remains the first choice. During arming, devices
compare manifests; if installed builds are compatible they use them. If not,
and **Use compatibility cores** is enabled (default), both select the same
packaged compatibility build for the session.

For device testing, **Force compatibility cores** defaults off. If either peer
enables it, both select every mutually available packaged compatibility build
even when their installed builds already match. The override refuses to arm if
a core present in either manifest has no common packaged build; it never forces
only one side.

Compatibility cores live under `cores/compatibility/<architecture>/`. Release
artifacts are:

- `Netplay.pak.zip` — app/shim plus dedicated gambatte and gpSP link cores;
- `compatibility-cores.zip` — compatibility cores by architecture;
- `Netplay-Full.pak.zip` — the pak with compatibility cores included.

The choice is transparent but visible: while the ROM continues starting, the
shim displays `Starting with compatibility core...` for two seconds. If the
installed builds differ and no common fallback is selected (including when the
setting is disabled), both devices retain their installed build and display
`Core builds differ. Desyncs may occur...` instead.

Negotiation still needs non-blocking accepted sockets, continuous broker
ownership, and broader cross-platform determinism testing—especially
pcsx_rearmed. Unresolved installed-build differences deliberately launch with
the visible desync-risk warning rather than failing closed.

### 7. Executable core sharing — Frozen

The **Share cores** setting remains visible and defaults to false, but all code
that transfers or adopts executable core files is compiled out. Unauthenticated
binary transfer is not an acceptable compatibility mechanism.

Reconsider only with an authenticated, signed manifest and a clear trust/update
model. Packaged compatibility cores are the supported replacement.

## Game selection and lightweight guest UX

### 8. Open-game invitation — Planned

Either paired device can choose a shared-screen game and offer it to the other:

> Host wants to play  
> Streets of Rage 2 — MD  
> B Cancel · A Play

The request carries a bounded display title, stable system identifier,
canonical content SHA-256, offer/session nonce, and mode—not a path, shell
command, ROM, or executable. The receiver finds a verified local match and
launches only its own local path and emulator pak.

Responses include accepted, declined, missing, busy, expired, and launch
failed. Simultaneous offers need deterministic resolution. A device already in
a game receives a non-destructive notification rather than being switched
automatically.

This requires a shared canonical content-hash helper and a persistent local ROM
index keyed by system and hash. It must handle raw ROMs, ZIP contents, CUE/BIN,
and M3U sets consistently with the runtime shared-screen identity check.

The Netplay.pak UI can present an offer itself. Receiving/offering from the
NextUI menu requires a narrow frontend IPC/modal and local-launch hook; the
current `queueNext()` mechanism is internal to NextUI.

### 9. Simple client — Needs decision

The existing setting and UI copy describe an appliance-like guest that stays in
Netplay.pak while the other device chooses a game. That UX is compatible with
the broker and open-game invitation: the guest owns a matching local ROM and
core, accepts the offer, and launches without browsing its library manually.

Earlier design notes also described the host supplying the ROM and executable
core. That is a materially different feature with copyright, authentication,
storage, and arbitrary-code-execution implications. It is not part of the
current plan. Unless explicitly redesigned, **Simple client means simplified
local matching/launch, not ROM or executable transfer**.

### 10. Interactive netplay status and controls — Partial

Shim-owned Starting/Waiting/Recovery overlays already poll standard RetroPad
input. The shared-screen failure overlay now provides explicit wait/retry,
continue-solo, and exit-game actions. These controls cannot observe arbitrary
unmapped physical buttons or remain interactive after MinArch enters its own
menu, because MinArch stops calling `retro_run()` there.

A panel beside the MinArch menu therefore needs a narrow frontend integration
for broker/shim status, raw frontend input, and actions. Do not run a competing
button-test process or read `/dev/input` behind the frontend's back.

## Controller topology

### 11. Swappable controller ports — Planned

Negotiate a stable emulated controller slot independently of network role. A
two-device session may map the guest to port 1, 2, 3, etc., and may transfer or
swap those assignments through an explicit synchronized action.

The selected slot is part of session identity. Every device applies the same
multitap/controller options. Switching occurs at a frame barrier so one physical
input cannot control two ports or disappear for different frames on each peer.

This is the prerequisite for multi-controller sessions and is much smaller than
multi-peer transport.

### 12. Three or more simultaneous players — Planned, substantial refactor

Shared-screen play becomes a host-hub topology:

- host maintains a peer table rather than one global connection;
- each guest receives a stable client/controller ID;
- inputs identify controller slot and frame;
- host relays the complete input set;
- hash/checkpoint acknowledgement and recovery are tracked per guest;
- disconnect policy decides whether a slot stalls, becomes neutral, or opens
  for a replacement;
- cores requiring Four Score/multitap/controller-device configuration receive
  identical options on every device.

Until this exists, the protocol must explicitly admit one gameplay peer and
reject extras with `BUSY` rather than leaving them queued ambiguously.

Link-mode capacity follows emulated hardware and core support. GB link remains
two-player. GBA RFU can potentially support more, but NetLink must first retain
and route libretro client IDs/broadcasts instead of collapsing everything to one
peer.

### 13. Hotseat/drop-in sessions — Planned

“Hotseat” here means controller occupancy may be left and rejoined without
ending the host's game—not traditional pass-the-handheld play. It builds on the
broker, swappable ports, multi-peer identity, and authoritative shared-screen
rejoin.

A vacant shared-screen slot has an explicit policy (neutral input, AI/game
behavior, or host-selected pause). A returning/replacement guest receives the
host's authoritative current state and begins contributing only after a commit
barrier. The host remains authoritative; this is not host migration.

## Local multi-instance mode

### 14. Instanced cores — Partial; Gambatte implemented

For Gambatte, each handheld runs a paired core (`gambatte_dual_libretro.so`)
holding both logical consoles and the cable between them. One core call per
frontend frame advances the pair; Wi-Fi carries only frame-numbered controller
inputs through the existing input-delay buffer, so no emulated serial exchange
costs a network round trip. Only the locally visible console produces video and
audio and only its save persists — the peer's console lives in-process and is
discarded at teardown, exactly as a second handheld would be.

At startup each device sends its own console's raw SRAM/RTC, the peer adopts it
into the replica, and the host establishes one paired checkpoint so both devices
hold bit-identical copies of both consoles before the first synchronized input
frame.

Different linked cartridges are supported — Red with Blue, Seasons with Ages —
provided *both* cartridges are installed on *both* devices, because no ROM ever
crosses the network. Both sides search for the peer's cartridge by size and
SHA-256 and exchange a verdict; if either cannot host the pair, the game
relaunches once on the network-serial core, which needs only one cartridge per
device. Matching Gambatte builds are still required either way.

A lost link is recoverable: the same Wait / Continue solo / Exit overlay as
shared-screen sessions, with a reconnect re-pairing from scratch rather than
resuming a timeline the peer cannot vouch for. Continue solo keeps the paired
core and leaves the abandoned console on neutral input.

gpSP and mGBA remain planned. This is core-specific orchestration, not a generic
toggle over the existing one-core shim; their entries are visibly marked
planned and cannot currently be enabled.

## Saves, reset, and authority

### 15. Shared-screen save policy — Implemented

- Frontend save/load/autosave/auto-resume are unavailable during the session.
- Host SRAM/RTC is authoritative and remains persistent through the frontend.
- Authoritative state bundles carry raw host SRAM/RTC to the guest.
- Guest uses that memory for play but exposes no persistent SRAM/RTC to its
  frontend and redirects core-managed save files to a process-local directory.
- Continue-solo does not change these rules midway through a running process.

Link-mode games keep independent local saves because they are independent
consoles.

### 16. Reset policy — Implemented

In shared-screen mode, a host reset becomes an explicit authoritative recovery
transaction at a frame barrier: the host resets, sends its post-reset state,
and both peers remain paused until the guest adopts it and the host commits the
new timeline. A guest reset request is rejected with a clear message. In link
mode and solo play, reset affects only the local console.

### 17. PSX guest memory card slot 2 — Needs decision

Current safety policy forces PCSX-ReARMed card 1 to libretro management and
disables card 2 for the session, preventing core-managed files from bypassing
guest volatility. The proposed feature conflicts with that implementation.

One possible replacement is:

- host card 1 remains the shared authoritative card sent to the guest;
- guest's own local card is mounted as card 2 for in-game exchange;
- only the guest's local card-2 writes persist on the guest;
- host state/checkpoints never overwrite that local card;
- both sides negotiate identical card geometry/options.

Before implementation, decide whether guest card 2 is persistent, whether it is
ever copied to the host, and how authoritative resync treats a game that has
cached card state internally. Until then, card 2 remains disabled.

## Platform and release quality

### 18. Five-platform builds — Implemented

The app and shim target my282, my355, h700, tg5040 and tg5050 through the pinned
build-platform environments. Dedicated cores are per platform; compatibility
cores are shared at the ARMv7/AArch64 architecture level where verified.

### 19. Compatibility/determinism qualification — In progress

Every supported core/platform pairing needs an explicit result for load,
initial sync, five-second agreement, authoritative resync, host/guest crash
rejoin, SRAM/RTC geometry and representative performance. Architecture-specific
cores may be supported only within an architecture if deterministic equivalence
cannot be established.
