# Superseded Gambatte sources

Nothing here is applied by any build rule. These files date from when the
Gambatte work lived in this repository as patches against upstream
`libretro/gambatte-libretro`. It now lives in the pinned fork
`bmpriest/gambatte-libretro`, which the Makefile clones at `GAMBATTE_REV`, and
every change below is present there — verified by locating each patch's
distinctive symbols in the pinned tree:

| File | Where it went |
| --- | --- |
| `gambatte-platforms.patch` | fork `Makefile.libretro` — tg5040/tg5050/my282 targets |
| `gambatte-network-hardening.patch` | fork `net_serial.{cpp,h}` — `TCP_NODELAY`, `MSG_NOSIGNAL`, complete reads/writes, deadlines, `reportStats` |
| `gambatte-local-instance.patch` | fork `libretro.cpp` — `SERIAL_LOCAL_SERVER`/`SERIAL_LOCAL_CLIENT`, `local_link` rendezvous |
| `gambatte-dual-wrapper.patch` | fork `libretro.cpp`, `Makefile.common`, `Makefile.libretro` — `NETPLAY_DUAL_INSTANCE`, `local_serial_bus`, `GBLC_MIRROR_INPUT` |
| `gambatte-dual-diagnostics.patch` | fork `libretro.cpp` — `dual_diag`, `GBLC perf`, `GBLC serial` |
| `local_serial.{h,cpp}` | fork `libgambatte/libretro/` — the tracked copies here predate `isIdle()`, which `retro_dual_is_checkpoint_safe()` needs |
| `test_local_serial.cpp` | fork `tests/dual_contract.c`, run by `make test-gambatte-dual` |

Only `gambatte-platforms.patch` still reverse-applies cleanly against the pinned
commit; the rest were subsumed by later fork commits and no longer match their
original context. They are kept because they are the readable form of what
changed and why, and because the fork's history compresses several of them into
single commits.

Do not add to this directory. Gambatte changes belong in the fork.
