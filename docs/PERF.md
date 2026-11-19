# Performance notes

The search loop and the list decoder are sensitive to small code changes. This file records what was measured, so
the code only needs a one-line reason and a pointer here. Add to it when a measurement changes the code.

## How to measure

- **Golden check first.** `tools/quality/check_golden.sh <binary> <data folder>` rebuilds `simplewiki-20261101`
  and checks that the graph files and the `--query Earth Banana` output are identical to the baseline. Then it runs
  `--bench` with every list in RAM and with `--ram-budget 0.03`.
- **Compare A/B, alternating.** Run the old and new binaries in turn, at least 5 pairs, on `enwiki-20261101` (the
  simplewiki searches take under 0.1 ms and hide most differences). A change counts as a slowdown only if it is
  slower in every pair, by more than 2%.
- **Count work, not just time.** `/usr/bin/time -l` (macOS) prints instructions retired and cycles. The same
  instruction count with a lower IPC points at memory layout or code alignment, not at the change itself.
- **Check the load.** Other processes on the machine move the timings by more than the effects below. Note
  `uptime` with each run, and repeat on a quiet machine before concluding anything.

## Findings

| Where | What | Effect | Measured |
|---|---|---|---|
| `decode_list` (NeighbourLists.h), the neighbour loop in `PathSearch::expand` | Loop over a 32-bit counter. A `size_t` counter or a range-for over the span made GCC 14 generate a slower loop. | English searches 8% slower | GCC 14 (machine not recorded) |
| `NeighbourLists::neighbours` | Fetch the blocks, then iterate over a returned span, rather than passing a callback per list. When GCC did not inline the callback, every array access in the BFS went through the closure. | Searches 4% slower | GCC 14 (machine not recorded) |
| `IoPool` | A plain `condition_variable` and stop flag, not `condition_variable_any` with a `stop_token`. The workers wake once per BFS level, and `condition_variable_any`'s internal second mutex costs on every wake. | Searches from the SSD (a budget below the list size) 25% slower | not recorded |
| `PathSearch` constructor, `GraphStore::open` | The per-node search arrays are allocated in the original order (`dist` for both sides, then `count` for both), after the lists. A version that allocated them elsewhere ran the same instructions at a lower IPC (1.16 to 1.09). | English searches about 5% slower | Apple M5 Pro, Apple clang 21, 2026-11. Not settled: this order measured even with the original, but the machine was heavily loaded. Re-measure on a quiet machine. |

## Ruled out

- **Loop alignment** (`-falign-loops=64`): no change to the PathSearch slowdown above.
- **Inlining**: the slower version inlined the same functions into the BFS loop (checked with `llvm-objdump`).
