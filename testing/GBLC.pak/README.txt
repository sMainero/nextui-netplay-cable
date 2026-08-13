GBLC dual-instance Gambatte performance test

This experimental core runs two copies of the same GB/GBC ROM inside one
process and links them through memory. Both consoles are shown side by side;
only the first is audible. Controls normally go to both consoles. Hold L2 while
pressing controls to operate only the left console, or hold R2 to operate only
the right console. Use this to break identical-state symmetry and navigate each
copy independently into its multiplayer mode.

Screen Scaling is locked to Fullscreen for this diagnostic build so the two
side-by-side outputs use the entire A30 display.

Copy the package so its paths are:

  Emus/my282/GBLC.pak/
  Roms/Game Boy Link Cable (GBLC)/

Do not use save states. This build intentionally reports no save-state support
because a state containing only one of the linked consoles would be unsafe.
Normal SRAM for the visible console may still be written under the GBLC save
directory; use expendable/test saves.

After testing, retrieve:

  .userdata/my282/logs/GBLC.txt

Lines beginning with "GBLC perf" report average/maximum primary, secondary,
and paired core execution time. "capacity" is the implied maximum core FPS and
does not include all frontend work. Lines beginning with "GBLC serial" report
in-memory transfers, sender wait time, bounded send timeouts, simultaneous-clock
arbitrations, and harmless external-clock polls that found no pending transfer.
Reports appear after 60 frames and every 300 frames.
