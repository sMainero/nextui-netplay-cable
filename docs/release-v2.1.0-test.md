# Netplay v2.1.0-test — notes for testers

Instanced link play for Game Boy is the headline. Both devices emulate *both*
Game Boys, the link cable stays inside each device, and Wi-Fi carries only
controller inputs. That removes a Wi-Fi round trip from every cable exchange,
which is the difference between trading feeling responsive and feeling broken.

This is a test build. It has been run for minutes, not hours — please read
"What is not proven" before assuming anything.

## What to install

| File | Where it goes | Who needs it |
| --- | --- | --- |
| `Netplay-Full.pak.zip` | extract over `/mnt/SDCARD/Tools/<platform>/` | everyone |
| `GBLC-my282.pak.zip` | extract over `/mnt/SDCARD/` | only for the standalone A30 test pak |

The archive contains `state/` as an empty directory, so your existing settings,
session config and ROM-scan cache survive an upgrade.

**Both devices must be on this build.** The link protocol changed (12 → 13) and
a mismatched pair will not pair.

## Trying instanced link

1. Turn on **Instanced link** in Settings, on both devices.
2. Both devices need **both** cartridges installed. No game file is ever sent
   between devices — each device loads the peer's cartridge from its own card.
3. Launch the game and pair as usual.

Zipped ROMs are fine. The peer's cartridge is located by hash inside your
archives; nothing needs unzipping.

Different linked cartridges work — Gold against Silver, Seasons against Ages.
They do not have to be the same game.

If either device can't satisfy a condition, the session restarts automatically on
the ordinary link-cable path and plays as it always did. You should see a notice
saying which.

## Known good

A completed Pokémon trade, Silver against Crystal, on an A30 and a Brick:
13,500 paired frames, 46 of 46 state checkpoints matching, no desync, 59 fps.
Both cartridges have real-time clocks, and the two devices' own clocks were six
hours apart.

Tetris DX and Super Mario Bros. Deluxe have each run several thousand frames
without divergence.

## What is not proven

Please treat these as untested rather than working:

- **Long sessions.** Nothing has run for more than a few minutes. Drift, leaks
  and heat are all unmeasured.
- **Rejoining after a crash**, and checkpoint persistence across one.
- **Menu pause, reset and disconnect** during a paired session.
- **Repeated launches** without rebooting between them.
- **SRAM ownership over a long session.** A trade writes to both cartridges, but
  only the visible console's save is persisted. This is the one on the list that
  could cost you save data — back up `Saves/` before a long session.
- **Oracle of Seasons/Ages linking.** The pair loads and pairs, but neither save
  had progressed far enough to reach the in-game link, so the game's own link
  protocol has never actually run.

## Two things worth setting up first

**Check both devices' clocks agree.** Each console keeps its own owner's time by
design, so if the two devices disagree by six hours, one player's Pokémon world
will be in daylight and the other's at night. That is the feature working, but
it looks like a bug.

**The first different-cartridge session on each device costs about 1.6 seconds**
while it indexes your Game Boy folders. It is cached afterwards, and the cache
survives upgrades.

## If something goes wrong

The useful file is the host's session log:

```
/mnt/SDCARD/.userdata/<platform>/logs/netplay-games/<game>/<session>/*.log
```

It records how the cartridge lookup went, both cartridge clocks, and a state
hash every 300 frames — enough to say whether the two devices diverged and
roughly where. Please include it, and say which two cartridges were involved.

If a session falls back to the link cable unexpectedly, the reason is in that
log too: a cartridge missing on one device, a core build mismatch, or the search
running out of time on a slow card are each reported separately.
