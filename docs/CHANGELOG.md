# Changelog

## [Unreleased]

### Changed
- Notification API lives in the core (`rb.h` / `rb.c`) instead of a
  separate object. The public surface is now:
  - `rb_notify_value(const rb_t *)`
  - `rb_wait(rb_t *, uint32_t expected, int timeout_ms)` → `int` (0 / -errno)
  - `rb_notify_fd(rb_t *)` / `rb_notify_drain_fd(rb_t *)`
  - `include/rb_notify.h` is a thin compatibility shim that includes `rb.h`
- `docs/API.md` rewritten to document the current notification helpers
  (snapshot-before-drain pattern, futex vs eventfd, CMake vs header
  defaults). The old `rb_notify_t` / `rb_notify_signal_*` /
  `rb_notify_wait_*` material has been removed.

### Added
- `RB_PER_SLOT_LAP=1`: Vyukov per-slot-lap mode with 8-byte slot header carrying per-slot sequence stamping (commit `b99fe52`)
- `RB_PREFETCH_R` / `RB_PREFETCH_W` compile-time prefetch hints in acquire and consume paths (commit `30d42bc`)
- Dual-build (`RB_PER_SLOT_LAP=0/1`) test coverage

## [1.0.0] - 2026-09-11

Initial release.

### Added
- SPSC ring with scratchpad, zero-copy
- Latched `on_full` / `on_low_d` / `on_low_e` callbacks
- `rb_drain` / `rb_flush` batch primitives
- Cross-compile toolchain files for ARM Linux and Android arm64
- TSan/ASan-clean test suite (unit, threaded, backpressure)
- Benchmarks: full cycle, fixed-size, variable-size payloads
