# Fixes waiting to land in the fork

**Currently empty.** Nothing here is pending.

This directory is a staging area, not an archive. A patch sits here only while a
core fix found from this side has not yet been committed to
`bmpriest/gambatte-libretro` and picked up by `GAMBATTE_REV`. While a patch is
here, the shipped cores **do not contain it** — nothing in this repository
applies these, so a pak built against the current pin still has the bug.

The lifecycle is:

1. Find the fault from this side, against a specific on-device failure.
2. Write the patch here with the evidence, so the reasoning survives.
3. Commit it to the fork on a branch, open a PR, merge it.
4. Move `GAMBATTE_REV` in the Makefile to the merged commit.
5. `make cores` — which re-clones the pinned tree, so the artifacts in `dist/`
   correspond to a real fork revision rather than a local working copy.
6. Move the patch to `../superseded/` with a line naming the fork commit.

Step 5 matters more than it looks. Building a core from a locally patched
`.cache` tree produces something `GAMBATTE_REV` does not describe, and nothing
downstream can tell the difference. Changing the pin makes the Makefile discard
that tree and start again, which is what keeps the two honest.

See `../superseded/gambatte-serial-audio-overflow.patch` for a worked example.
