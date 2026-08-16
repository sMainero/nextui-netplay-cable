# mGBA as a second paired core — groundwork

Gambatte gave us instanced link play for Game Boy. mGBA is the obvious next
target: it covers GBA as well as GB/GBC, and it is the emulator whose
multiplayer support actually works in the wild.

The received wisdom is that mGBA's linking lives in its Qt frontend and is
therefore unavailable to a libretro build. That is half right, and the half that
is wrong is the important half. What follows is verified against the checkout at
`cores/mgba-libretro` (`e31759b24`), not from memory.

## What is actually where

**The lockstep coordinator is core code, not Qt.**

```
src/core/lockstep.c                 generic state machine + threaded user
src/gb/sio/lockstep.c               Game Boy cable, 2 nodes
src/gba/sio/lockstep.c              GBA cable, up to 4 nodes
include/mgba/internal/gb/sio/lockstep.h
include/mgba/internal/gba/sio/lockstep.h
```

`MAX_GBS` is 2 and `MAX_GBAS` is 4. `GBSIOLockstep` holds the players and the
pending transfer bytes; `GBSIOLockstepNode` is a `GBSIODriver` that attaches to
one console. The API is `GBSIOLockstepInit`, `GBSIOLockstepNodeCreate`,
`GBSIOLockstepAttachNode`, `GBSIOLockstepDetachNode`. None of it includes
anything from Qt.

**The libretro build does not compile the drivers.**

`libretro-build/Makefile.common` includes `src/core/lockstep.c`, `src/gb/sio.c`,
`src/gba/sio.c` and `src/gba/sio/gbp.c` — but neither `src/gb/sio/lockstep.c`
nor `src/gba/sio/lockstep.c`. The generic state machine is compiled; the two
drivers that would use it are not in the build at all.

**The libretro frontend has no concept of a second console.**

`src/platform/libretro/libretro.c` owns exactly one core:

```c
static struct mCore* core;                       /* line 86 */
```

`retro_load_game_special` is a stub:

```c
bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info) {
	UNUSED(game_type); UNUSED(info); UNUSED(num_info);
	return false;                                 /* line 2250 */
}
```

and the string `lockstep` appears zero times in the file.

So the position is: the cable is written, it is good, and nothing in the
libretro layer reaches it.

## Why this is a better starting point than gambatte was

This is the part worth dwelling on, because it decides how much of the work we
did for Gambatte has to be repeated.

Gambatte's `SerialIO::send()` is **synchronous**. The clock-owning console
blocks inside `runFor()` until its peer answers. That single fact forced
everything else: two threads, a condition-variable coordinator, a frame barrier
so neither console could leave a frame mid-transfer, cycle-stamped requests so
two devices resolved the same transfer identically, and a long tail of deadlocks
and determinism bugs to go with it.

mGBA does not work that way. `GBSIOLockstepNode` schedules an `mTimingEvent` on
its own console's timing and its handler returns **how many cycles until it next
needs attention**:

```c
static void _GBSIOLockstepNodeProcessEvents(struct mTiming*, void* user, uint32_t cyclesLate);
node->nextEvent += LOCKSTEP_INCREMENT;
```

There is no blocking call in the driver. Coordination is expressed as cycle
deltas, which is exactly the shape a cooperative scheduler wants: advance
whichever console owes cycles, then the other.

And the blocking policy is not baked in. `struct mLockstep` is an interface:

```c
void (*lock)(struct mLockstep*);
void (*unlock)(struct mLockstep*);
bool (*signal)(struct mLockstep*, unsigned mask);
bool (*wait)(struct mLockstep*, unsigned mask);
void (*addCycles)(struct mLockstep*, int id, int32_t cycles);
int32_t (*useCycles)(struct mLockstep*, int id, int32_t cycles);
int32_t (*unusedCycles)(struct mLockstep*, int id);
```

`mLockstepThreadUser` — the thread-sleeping implementation, guarded by
`#ifndef DISABLE_THREADING` — is *one* implementation of that policy, the one Qt
uses. A single-threaded frontend can supply its own `wait`/`signal` that never
sleep a thread and instead hand control back to a scheduler.

If that holds, the paired mGBA core needs **no threads, no frame barrier and no
in-process cable of our own**. The three hardest problems from the Gambatte work
are already solved upstream, by the person who wrote the emulator.

## What still has to be true

Determinism is the requirement that killed us repeatedly with Gambatte, and
none of the above establishes it. Before writing a line of wrapper:

1. **Is the lockstep resolution a pure function of emulated state?** Gambatte's
   coordinator originally decided transfers by wall clock and thread arrival;
   ours had to be rewritten to decide by emulated cycles. mGBA's uses
   `mTimingEvent` and cycle counts, which is promising, but `signal`/`wait` and
   the `TRANSFER_*` phase machine need reading with the mirrored-replica question
   in mind: *given identical initial state and identical inputs, do two devices
   compute identical results?*
2. **Does mGBA's savestate cover the lockstep and SIO state?** Our paired
   checkpoint has to serialize both consoles *and* the cable. Gambatte needed
   fixes here (uninitialised `SaveState` PODs) before two devices could agree.
3. **Does the RTC read the host clock?** GBA cartridges have RTCs too, and we
   have just been through what that costs. Expect the same `TimeSource` shape.
4. ~~What does `DISABLE_THREADING` exclude?~~ **Answered.** The libretro build
   defines `-DDISABLE_THREADING -DMINIMAL_CORE=2`, so `mLockstepThreadUser` is
   compiled out entirely. There is no threaded fallback to retreat to: a paired
   libretro core *must* supply its own non-blocking `mLockstep` policy. That is
   the design we wanted anyway, and it is now forced rather than chosen. Worth
   checking what else `MINIMAL_CORE=2` removes.

## Shape of the work

Roughly in order, each step verifiable before the next:

1. **Build the drivers.** Add `src/gb/sio/lockstep.c` and
   `src/gba/sio/lockstep.c` to `libretro-build/Makefile.common`. Confirm the
   core still builds and behaves identically single-player — a no-op change that
   proves the files compile in this configuration.
2. **A host harness, before any pak integration.** Two `mCore`s, a
   `GBSIOLockstep`, two nodes, a cooperative scheduler, and a fixed input script
   — run it twice and hash both consoles' states. This is the equivalent of
   `cores/tests/bustest.cpp`, and the Gambatte experience says build it first:
   every determinism bug we found was found by a host harness, and every one we
   missed cost a round trip to the handhelds.
3. **Implement the cooperative policy.** `DISABLE_THREADING` means there is no
   threaded user to fall back on, so this is the only route: `lock`/`unlock`
   become no-ops in a single-threaded core, and `wait`/`signal` must yield to
   the scheduler rather than sleep. If the drivers turn out to require genuine
   blocking, the fallback is not mGBA's threaded user — it is our own thread
   pair, i.e. the Gambatte architecture. Establish which in step 2, on the host,
   before committing.
4. **Mirror the `retro_dual_*` ABI.** The shim already speaks it: version and
   capability negotiation, visible-console selection, per-console memory, paired
   checkpoint, targeted reset, clock epochs. A second core implementing the same
   ABI should need little shim work beyond core detection and manifest entries.
5. **Then the pak.** `inst_mgba` already exists in the settings file and is
   wired to `instanced_core_enabled()`; nothing in `select_compatibility()` is
   Gambatte-specific except the artifact name.

## What not to repeat

- Do not start with the device. Every Gambatte determinism bug was diagnosed on
  the host once a harness existed, and the ones we chased on hardware first cost
  hours each.
- Do not assume the frontend's savestate is the paired checkpoint. It is not;
  it deliberately excludes presentation and diagnostics so two devices can hash
  it.
- Do not let a search, scan or wait run unbounded inside a step the peer is
  blocked on. That produced a freeze indistinguishable from a crash.
- Write the test that reproduces the failure *before* the fix, and check that it
  fails — twice now a test that "passed" was only passing because it could not
  reach the bug.
