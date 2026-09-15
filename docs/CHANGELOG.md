# Changelog

## [Unreleased]

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
