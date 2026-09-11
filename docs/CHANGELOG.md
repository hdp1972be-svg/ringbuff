# Changelog

## [1.0.0] - 2026-09-11

Initial release.

### Added
- SPSC ring with scratchpad, zero-copy
- Latched `on_full` / `on_low_d` / `on_low_e` callbacks
- `rb_drain` / `rb_flush` batch primitives
- Cross-compile toolchain files for ARM Linux and Android arm64
- TSan/ASan-clean test suite (unit, threaded, backpressure)
- Benchmarks: full cycle, fixed-size, variable-size payloads
