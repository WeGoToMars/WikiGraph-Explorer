#!/usr/bin/env bash
# Guard for changes to the core: rebuild simplewiki-20261101 with a given binary and check that the graph files are
# byte-for-byte the reference ones (the build is deterministic), then print search timings to compare.
#
# Usage: tools/quality/check_golden.sh <wikigraph binary> <data folder with the simplewiki-20261101 dumps> [bench n]
# The data folder gets a simplewiki-20261101.graph directory; use a scratch copy, not one another process is using.
#
# Reference timings (Apple M5 Pro, release preset, 3000 searches):
#   all in RAM:        mean 0.087 ms, p99 0.76 ms
#   --ram-budget 0.03: mean 0.39 ms,  p99 1.42 ms, 30 blocks from disk per search
set -euo pipefail
bin=$1 data=$2 n=${3:-3000}
here=$(cd "$(dirname "$0")" && pwd)
wiki=simplewiki-20261101

"$bin" --data-dir "$data" --wiki "$wiki" --rebuild > /dev/null
( cd "$data/$wiki.graph" && shasum -a 256 $(ls | grep -v '^graph.meta$') ) | diff "$here/$wiki.sha256" - \
  && echo "graph files: identical to the baseline" \
  || { echo "graph files DIFFER from the baseline"; exit 1; }

"$bin" --data-dir "$data" --wiki "$wiki" --query Earth Banana --paths 5 | tail -5 | sed -E "s/ \| [0-9.]+ ms//" | diff "$here/$wiki-query.txt" - \
  && echo "query output: identical to the baseline" \
  || { echo "query output DIFFERS from the baseline"; exit 1; }

for budget in all 0.03; do
  for _ in 1 2 3; do "$bin" --data-dir "$data" --wiki "$wiki" --ram-budget "$budget" --bench "$n" | grep '^bench:' | sed "s/^/[$budget] /"; done
done
