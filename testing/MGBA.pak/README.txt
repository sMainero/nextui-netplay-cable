mGBA measurement and experimental paired-link pak
==================================================

What this is
------------
A standalone emulator pak that carries stock mGBA plus an experimental paired
GBA frontend. NextUI builds mGBA (pinned at 925f0f0b) but does not ship it on the
card, so without this there is no way to run it at all.

It carries its own copy of the core. It does not touch the system cores, the GBA
pak, or anything else; deleting the directory removes it completely.

Install
-------
Unzip MGBA-<platform>.pak.zip at the root of the SD card. Everything lands where
it belongs:

  Emus/<platform>/MGBA.pak/          the pak
  Roms/Game Boy Advance (MGBA)/      put ROMs here

An SD-card pak takes precedence over the system one, so the ordinary GBA.pak and
gpSP are left alone and still work as before.

Paired link testing
-------------------
When Netplay negotiates `instanced_mgba=1`, launch.sh selects
mgba_dual_libretro.so. That core runs two GBA consoles in one process through
mGBA's own GBASIOLockstepCoordinator; Wi-Fi carries delayed player inputs and
checkpoints rather than timing-sensitive SIO traffic. Ordinary launches continue
to select mgba_libretro.so. If paired bootstrap cannot be completed, the shim's
demotion marker selects the stock core on relaunch.

The paired frontend currently targets GBA only. It implements ABI v2 two-content
loading, per-console saves, visible-console selection, targeted resets, paired
checkpoints and fixed RTC epochs. BIOS selection, cheats, sensors, rumble and the
stock mGBA option surface are intentionally not part of this first wrapper.

First device result
-------------------
Two TrimUI Bricks completed a Mario Kart: Super Circuit multiplayer race with
audio and synchronized gameplay. The paired wrapper reported its same-rate
audio fast path on both devices:

  mGBA Dual audio active: output=65536 source=65536

The paired calls averaged about 16.25-16.29ms, just inside the roughly 16.74ms
frame budget. Core-side phase profiling later showed that figure includes the
frontend's presentation wait: two-console emulation plus the cable peaked near
10.8ms, audio near 0.9ms, and input below 0.1ms, leaving roughly 5ms of real
headroom. The remaining chop followed matching network-input stalls on both
devices rather than changes in core execution time. For the next ordinary-WiFi
test, current Netplay builds retain the ten-frame transport floor instead of
letting a couple of quiet startup RTT probes lower it. Ad hoc retains a
three-frame floor.

The race proves the implementation is playable, not yet that every long run is
bit-identical: retain logs containing periodic paired-state hashes when testing.

Measuring
---------
Two scripts ship inside the pak. Launch a ROM, then over SSH:

  sh "/mnt/SDCARD/Emus/<platform>/MGBA.pak/frametime.sh"    process-wide ms/frame
  sh "/mnt/SDCARD/Emus/<platform>/MGBA.pak/threadtime.sh"   per-thread breakdown

The pak routes either core through the netplay shim so the audio-rate experiment can
be switched without changing anything else in the path. That experiment has been
run, and the answer was no:

  target 65536 (mGBA's default)   12.63 ms/frame    <- keep this
  target 32768                    14.64 ms/frame

A30 at a pinned 1344MHz, Mario Kart Super Circuit, process-wide. The idea was
that mGBA upsamples GBA's native 32768Hz to 65536 for nothing. But 32768 is the
GBA's *reset* rate; games write SOUNDBIAS and then run at 65536 or higher, so
mGBA's default already matches what a real game produces and its resampler is
already skipped. Forcing 32768 switches that resampler on rather than off, which
is the ~2ms. minarch resamples unconditionally either way, so there was never a
second pass to collapse.

The default is therefore to decline and leave mGBA alone. The knob remains so the
measurement can be redone - a game that genuinely runs at 32768 would want it:

  echo 32768 > .../MGBA.pak/sample_rate.txt   force a rate
  rm           .../MGBA.pak/sample_rate.txt   decline (default)

Changing it only takes effect on the next launch - the rate is fixed when the
core loads. frametime.sh prints the rate the running instance actually
negotiated, so a stale arm cannot be mistaken for a null result.

The launcher logs which arm is active as "rate=0" or "rate=default".

What to look at
---------------
The debug HUD is on by default, but it cannot answer the cost question and you
should not try to read one off it. Its A: field is wall-clock flip-to-flip and
spans minarch's own pacing sleep, so it pins at ~16.7ms whenever the core keeps
up; and perf.cpu_usage has no my282 implementation, so the CPU percentage is
always 0 here. Use testing/frametime.sh, which measures process CPU time.

The number that matters for instanced link play is time per frame. Two consoles
have to fit inside one 16.7ms frame along with the frontend's own work.

Measured on an A30 at a pinned 1344MHz (CPU Speed=Performance), process-wide:

  mGBA, GBA content      12.7 ms
  mGBA, GB/GBC content    8.2 ms
  gambatte, GB content     7.3 ms

Those are frontend + core. The frontend alone -- blit, scale, GL, audio -- costs
about 5-6ms of the budget no matter which core is loaded, backed out of the
gambatte figure against the 2.23ms single-console core measurement in
docs/multi-instance.md. So the core-only costs are roughly:

  mGBA GBA    ~6.6-7.6 ms      paired serial ~19-20 ms   over 16.74
  mGBA GB/GBC ~2.1-3.1 ms      paired serial ~10-11 ms   fits
  gambatte    ~1.3-2.2 ms      paired serial   ~8 ms     fits

Because the frontend is paid once but the core is paid twice, the real bar for a
serially-paired core is not 8ms -- it is about (16.74 - frontend) / 2, or roughly
5.5ms per console. mGBA's GBA core misses that; its Game Boy core clears it.

For comparison, the paired Gambatte core costs 2.43ms for *both* consoles
together on this device (docs/multi-instance.md, A30 column) -- not per console,
as an earlier draft of this file claimed.

Worth measuring separately:

  GBA content   the demanding case, and the reason mGBA is interesting
  GB/GBC content  mGBA also emulates these, and this is the direct
                  comparison against Gambatte on the same hardware

A GB/GBC ROM placed in the MGBA folder will run under mGBA rather than
Gambatte, which is the comparison worth having: if mGBA's Game Boy is close to
Gambatte's, the lockstep cable it already has may be worth more than Gambatte's
speed.
