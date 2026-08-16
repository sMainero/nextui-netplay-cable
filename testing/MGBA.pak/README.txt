Baseline mGBA for my282 (Miyoo A30)
===================================

What this is
------------
A standalone emulator pak that runs mGBA on the A30, so its cost can be
measured before any instanced-link work is attempted. NextUI builds mgba for
my282 (pinned at 925f0f0b) but does not ship it on the card, so without this
there is no way to run it at all.

It carries its own copy of the core. It does not touch the system cores, the
GBA pak, or anything else; deleting this directory removes it completely.

Install
-------
Copy to:      /mnt/SDCARD/Emus/my282/MGBA.pak
ROMs go in:   /mnt/SDCARD/Roms/Game Boy Advance (MGBA)/

An SD-card pak takes precedence over the system one, so the ordinary GBA.pak
and gpSP are left alone and still work as before.

What to look at
---------------
The debug HUD is on by default. It shows measured fps against requested fps,
their ratio, audio buffer depth, and per-core CPU load.

The number that matters for instanced link play is time per frame. Two consoles
have to fit inside one 16.7ms frame along with the frontend's own work, so a
single instance needs to cost well under 8ms. For comparison, the paired
Gambatte core costs about 2.4ms per console on this device and reaches 60fps
with room to spare.

Worth measuring separately:

  GBA content   the demanding case, and the reason mGBA is interesting
  GB/GBC content  mGBA also emulates these, and this is the direct
                  comparison against Gambatte on the same hardware

A GB/GBC ROM placed in the MGBA folder will run under mGBA rather than
Gambatte, which is the comparison worth having: if mGBA's Game Boy is close to
Gambatte's, the lockstep cable it already has may be worth more than Gambatte's
speed.
