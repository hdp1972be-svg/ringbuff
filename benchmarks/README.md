# CI benchmark results

These files are produced weekly by
[`.github/workflows/bench.yml`](../.github/workflows/bench.yml) on
GitHub-hosted runners.

**These are NOT the reference benchmark numbers for `rb`.** The
reference numbers live in [`docs/BENCHMARKS.md`](../docs/BENCHMARKS.md)
and were produced on a controlled machine.

CI runners are shared, virtualized, and noisy. Absolute numbers vary
by 20–50% between runs, and the CPU model changes without notice. What
these files are useful for is **regression detection**: if a commit
causes a consistent slowdown across several weekly runs, it will show
up here.

Each file contains:

- the CPU model of the runner that produced it,
- the compiler version,
- the raw output of `bench_rb` and `bench_rb_random`.

Do not quote these numbers in the README or the docs.
