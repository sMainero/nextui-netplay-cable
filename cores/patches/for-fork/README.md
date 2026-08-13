# Fixes waiting to land in the fork

Unlike `../superseded/`, these are **live** — they are not applied by any build
rule here and the shipped cores do not contain them until each one is committed
to `bmpriest/gambatte-libretro` and `GAMBATTE_REV` in the Makefile is moved
forward. A pak built against the current pin still has the bug.

They live here rather than in the fork's history alone because each one was
found from this side, against a specific on-device failure, and the reasoning is
worth keeping next to the evidence.

## `gambatte-serial-audio-overflow.patch`

Fixes a heap-buffer-overflow that aborts the process the first time two consoles
exchange a serial byte. Observed on the Brick as:

```
[Gambatte] GBLC serial exchanges=1 wait_avg=42us ... idle_polls=54109
realloc(): invalid old size
Aborted
=== process exited status=134 ===
```

`blipper_push_delta()` writes `taps` entries at `output_buffer[phase/decimation]`
and never bounds-checks. The buffer holds `BLIP_BUFFER_SIZE + taps` entries, so
about `BLIP_BUFFER_SIZE * decimation` = 98304 input samples may be pushed before
it walks off the end. The ordinary frame loop is safe because it drains whenever
the resampler is half full.

The serial service slices are not. They accumulate a whole frame of audio — a
`runFor` that asked for 32 samples may return two thousand, and a frame can hold
many slices — and the visible-A path pushed the entire accumulation in one call,
draining only afterwards. The visible-B path chunked but likewise drained only
at the end.

The patch adds `audio_render_bounded()` and routes both paths through it, so
both chunk and drain between chunks exactly as the frame loop does.

Reproduced under ASan against the pinned tree: survives 98000 samples, overflows
at 100000 in `blipper_push_delta` (`blipper.c:171`) past the 6272-byte region
from `blipper_new` (`blipper.c:141`). With the patch the same pattern survives
2,000,000.

**This does not make instanced link correct.** The coordinator it runs under is
still wall-clock dependent — timed `waitForService`, a 50ms `send()` timeout that
fabricates `0xFF`, mutex-order clock arbitration, and a frame-dupe gate driven by
the visible console's audio. Two devices cannot stay in step through any of
those. This patch only stops the crash.
