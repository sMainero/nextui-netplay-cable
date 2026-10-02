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

### Transports

The play modes below are about *what the two devices agree on*. Orthogonally,
they run over one of three transports, and the session file names which:

| transport | `link=` | what it moves |
|---|---|---|
| same network | `wifi` | nothing; both devices were already associated |
| ad hoc | `adhoc` | the radio — the host serves a network, the guest joins it |
| USB-C cable | `cable` | nothing on the radio; a point-to-point USB link and a TUN interface on each end |

A cable session writes no `adhoc_ssid`, which is what keeps the ad-hoc rejoin
and the WiFi watchdog out of it; it takes the USB controller from the
firmware's own `adb` gadget for the life of the session and gives it back
afterwards, which is why it leaves a repair record behind if it is killed. See
[cable.md](cable.md). Transport and mode are independent: an instanced-core
session and a link-cable session both run over any of the three.

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
end/failure, and runs a detached watchdog if the AP disappears. After its
failure grace period, a recovered guest session ends automatically once the
original WiFi is back; persistent host/hot-seat sessions remain armed. A reopened app
and the broker reconstruct the active AP interface and SSID from durable session
metadata and hostapd configuration. Static guest addressing may eventually
remove the `udhcpd` dependency.

### 5. Persistent hosting/session broker — Partial

Hosting outlives the setup-app process. A small detached broker, started when a
host arms, owns:

- LAN announcements and ad-hoc session metadata;
- compatibility negotiation;
- peer admission, stable peer identity, capacity and `BUSY` replies;
- broker status for Netplay.pak;
- clean shutdown when the session ends.

The broker admits one compatibility peer for the current two-player transport,
actively rejects a different address with `BUSY`, publishes its PID/network and
guest table atomically, and is restarted by a reopened host UI if needed. The
main menu reads durable session state: hosts see the network and connected
guests, guests see the network and host, and Host/Join remain hidden until X
ends the session.

Sessions carry the kernel boot identity that created them. On a later boot the
boot hook, game pre-launch path, or setup UI removes the leftover active-session
record and its broker/Wi-Fi breadcrumbs while retaining the installed passthrough
bindings. Guest count is never a staleness signal: an empty host remains a valid
hot-seat lobby for as long as that boot and session continue. Older sessions
without a boot identity are preserved rather than removed speculatively.

Game offers/replies, authenticated pairing, authorization tied to gameplay
sockets remain. A future NextUI/MinArch integration can consume the same status
file rather than owning another network listener.

The setup UI uses NextUI's normal menu CPU profile instead of retaining the
performance setting used to launch paks. Gameplay remains separate: the shim
pins performance only for the lifetime of an armed emulator process and restores
the prior policy at teardown.

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

## Game selection and lightweight guest UX

### 7. Open-game invitation — Planned

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

### 8. Simple client — Needs decision

The existing setting and UI copy describe an appliance-like guest that stays in
Netplay.pak while the other device chooses a game. That UX is compatible with
the broker and open-game invitation: the guest owns a matching local ROM and
core, accepts the offer, and launches without browsing its library manually.

Earlier design notes also described the host supplying the ROM and executable
core. That is a materially different feature with copyright, authentication,
storage, and arbitrary-code-execution implications. It is not part of the
current plan. Unless explicitly redesigned, **Simple client means simplified
local matching/launch, not ROM or executable transfer**.

### 9. Interactive netplay status and controls — Partial

Shim-owned Starting/Waiting/Recovery overlays already poll standard RetroPad
input. The shared-screen failure overlay now provides explicit wait/retry,
continue-solo, and exit-game actions. These controls cannot observe arbitrary
unmapped physical buttons or remain interactive after MinArch enters its own
menu, because MinArch stops calling `retro_run()` there.

A panel beside the MinArch menu therefore needs a narrow frontend integration
for broker/shim status, raw frontend input, and actions. Do not run a competing
button-test process or read `/dev/input` behind the frontend's back.

## Controller topology

### 10. Swappable controller ports — Planned

Negotiate a stable emulated controller slot independently of network role. A
two-device session may map the guest to port 1, 2, 3, etc., and may transfer or
swap those assignments through an explicit synchronized action.

The selected slot is part of session identity. Every device applies the same
multitap/controller options. Switching occurs at a frame barrier so one physical
input cannot control two ports or disappear for different frames on each peer.

This is the prerequisite for multi-controller sessions and is much smaller than
multi-peer transport.

### 11. Three or more simultaneous players — link-cable implemented, shared-screen planned

**Link-cable play carries four consoles today.** The transport keeps a four-slot
peer table, the host assigns each guest its console number and sends it in the
greeting, frames keep their sender's id, and the host relays `CMD_DATA` to every
other guest while the control conversation stays host-to-guest. The shim decides
capacity by mode, so only link-cable play asks for three guests. Which cores can
use it is a separate question with a separate answer: gpSP's Advance Wars
protocol is written for four players, while the Game Boy link is two-player
hardware and stays that way. See `docs/cable.md` § Four players.

**Shared-screen play is still the planned piece, and is a host-hub topology:**

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
two-player, and the transport's four slots are a GBA multi-play ceiling rather
than a promise every core can meet. The client ids and broadcasts this item
needed for link play - a peer table, a console number per guest, and routing
that keeps them - are the ones now implemented; extending the same to
shared-screen mode is the work described above.

### 12. Hotseat/drop-in sessions — Planned

“Hotseat” here means controller occupancy may be left and rejoined without
ending the host's game—not traditional pass-the-handheld play. It builds on the
broker, swappable ports, multi-peer identity, and authoritative shared-screen
rejoin.

A vacant shared-screen slot has an explicit policy (neutral input, AI/game
behavior, or host-selected pause). A returning/replacement guest receives the
host's authoritative current state and begins contributing only after a commit
barrier. The host remains authoritative; this is not host migration.

## Local multi-instance mode

### 14. Instanced cores — Partial; Gambatte and mGBA implemented

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

mGBA uses the same mirrored-replica protocol and dual ABI, but its paired core
drives two GBA `mCore`s cooperatively through mGBA's own cycle-based SIO
lockstep coordinator. Builds cover tg5040, my282 and h700; paired hardware play
is validated on tg5040. Testing found that my282 cannot sustain the current
same-thread dual implementation, and h700 remains compile-tested only. Two
Bricks completed Mario Kart: Super Circuit races with
audio over both ad hoc and ordinary Wi-Fi. Phase profiling shows roughly 5ms of
real frame headroom; the enclosing 16.2ms call includes presentation pacing. gpSP stays
on its existing Wi-Fi link implementation: its multiplayer protocols are
latency-tolerant enough that duplicating the core locally has no useful payoff.
This is core-specific orchestration, not a generic toggle over the existing
one-core shim.

mGBA is disabled by default and managed as an optional installation. Netplay
bundles an ordinary `MGBA.pak` for tg5040, h700 and my282 under
`cores/mgba/<platform>/MGBA.pak`, while the paired core remains under
`cores/override/<platform>`. Enabling it backs up any installed pak with a
collision-free `.bak` name and copies saves/states into shared Netplay userdata
before installing matched paks for every supported platform directory on the
card. A per-platform SHA-256 check detects later core replacement; declining a
reinstall is remembered only for that exact installed/expected hash pair.
Disabling it—or turning off Netplay—offers independent pak, save and state
restoration, preserving every overwritten destination file in a timestamped
collision directory.

## Saves, reset, and authority

### 14. Shared-screen save policy — Implemented

- Frontend save/load/autosave/auto-resume are unavailable during the session.
- Host SRAM/RTC is authoritative and remains persistent through the frontend.
- Authoritative state bundles carry raw host SRAM/RTC to the guest.
- Guest uses that memory for play but exposes no persistent SRAM/RTC to its
  frontend and redirects core-managed save files to a process-local directory.
- Continue-solo does not change these rules midway through a running process.

Link-mode games keep independent local saves because they are independent
consoles.

### 15. Reset policy — Implemented

In shared-screen mode, a host reset becomes an explicit authoritative recovery
transaction at a frame barrier: the host resets, sends its post-reset state,
and both peers remain paused until the guest adopts it and the host commits the
new timeline. A guest reset request is rejected with a clear message. In link
mode and solo play, reset affects only the local console.

### 16. PSX guest memory card slot 2 — Needs decision

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

### 17. Five-platform builds — Implemented

The app and shim target my282, my355, h700, tg5040 and tg5050 through the pinned
build-platform environments. Dedicated cores are per platform; compatibility
cores are shared at the ARMv7/AArch64 architecture level where verified.

### 18. Compatibility/determinism qualification — In progress

Every supported core/platform pairing needs an explicit result for load,
initial sync, five-second agreement, authoritative resync, host/guest crash
rejoin, SRAM/RTC geometry and representative performance. Architecture-specific
cores may be supported only within an architecture if deterministic equivalence
cannot be established.
