# SUMMER benchmark

C++ microbenchmark for [SUMMER](https://github.com/7244/SUMMER), comparing three counter-update strategies under random and fully-colliding access patterns: plain atomics, per-thread local counters with batched sync, and SUMMER.

## What it measures

- `THREAD_COUNT` threads (compile-time, defaults to `nproc` = one per logical CPU), each pinned to its own CPU via `sched_setaffinity`.
- Every iteration does:
  - 1 random `uint64_t` read from an 8GB buffer (index from a per-thread xorshift64* stream, kept alive with a compiler barrier),
  - 8 counter increments into a `2^23` (64MB) counter array.
- Every 10000 iterations each thread does `__atomic_add_fetch(&progress, 10000)` on a global progress counter. The run stops when progress reaches `BENCH_TARGET` (1e11 total iterations). Wall time is reported; **lower is better**.

## Variants

| # | name | counter update |
|---|------|----------------|
| 1 | atomic | 8x `__atomic_add_fetch(&g_counters[i], 1, SEQ_CST)` on random indices |
| 2 | local+batch | relaxed adds on per-thread `l_counters[th][i]`; per-index `last_sync[i]` CAS every `BENCH_SYNC_MS` (128ms); the CAS winner sums all 22 threads' rows (non-destructive reads) and atomically exchanges the total into `g_counters[i]` |
| 3 | summer | `summer.sum(th, {&g_counters[i]}, 1)` with `elem_count = 4096`; no manual flush (SUMMER auto-flushes on key change / local-sum overflow) |
| 4 | atomic, colliding | same as 1, but indices are fixed to `0..7` (8 hot cache lines) |
| 5 | local+batch, colliding | same as 2, but indices are fixed to `0..7` |
| 6 | summer, colliding | same as 3, but keys are fixed to `g_counters[0..7]` |

The colliding variants are otherwise identical to their base variants — the xorshift state still advances the same way, so the random-read sequence is bit-identical.

`./bench` with no argument runs all six; `./bench N` runs one.

## Measured results

Host: Intel Core Ultra 7 155H, 22 logical CPUs, `BENCH_TARGET = 1e8`:

| variant | time |
|---------|------|
| 1 atomic | 2.7s |
| 2 local+batch | 115.5s |
| 3 summer | 3.6s |
| 4 atomic, colliding | 34.2s |
| 5 local+batch, colliding | 0.85s |
| 6 summer, colliding | 0.81s |

## Observations

- **v3 ≈ v1**: with 2^23 random counters hashed into only 4096 buckets, a thread's bucket almost always holds a *different* key than the new draw, so SUMMER's auto-flush fires on nearly every `sum()` call — roughly the same atomic work as v1 plus hash overhead.
- **v2 is drain-bound**: with per-index 128ms sync, nearly every counter touch finds its index stale and triggers a 22-row merge (711M drains at 1e8). Raising `BENCH_SYNC_MS` to 2000 cuts it from 115.5s to 15.8s.
- **v4 collapses** under RMW contention: 22 threads hammering the same 8 cache lines.
- **v5 ≈ v6** and fastest: both reduce to ~8 local adds per iteration (private lines / SUMMER elems) plus the 8GB random read — with 8 fixed keys SUMMER settles each key into its bucket once and then does pure local `e.sum += 1` with zero atomics. Both are read-bound.

## Usage

```sh
make            # build and run all variants (default 1e11 iterations)
make 100000000  # same, but with the given total iteration count
make ITER=100000000
make run4       # single variant
make clean
```

`THREAD_COUNT` is baked in at compile time — change it in the Makefile or pass `make THREAD_COUNT=8` (must be < 256, SUMMER's `max_threads_t` is `uint8_t`).

## Configuration knobs (compile-time, via Makefile)

| knob | default | meaning |
|------|---------|---------|
| `THREAD_COUNT` | `nproc` | thread count (also SUMMER `max_threads`) |
| `ITER` / `BENCH_TARGET` | 100000000000 | total iterations to finish |
| `BENCH_SYNC_MS` | 128 | variant 2/5 per-index sync period (ms) |

## Requirements

- Linux, g++ or clang (C++20), curl, `-march=native`-able CPU.
- ~12GB free RAM: 8GB read buffer + `THREAD_COUNT` x 64MB local counters + 64MB global counters + 8MB sync table.

## Notes

- `SUMMER.h` is downloaded automatically by make from the SUMMER repo (not committed).
- SUMMER.h uses `CONCAT()` without defining it; bench.cpp supplies the standard token-paste macro before the include.
