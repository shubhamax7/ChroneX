#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 <results-directory> [benchmark-filter]" >&2
  exit 2
fi

results_dir="$1"
filter="${2:-BM_(Reference|Optimized)Book.*|BM_MatchingPipeline.*}"
mkdir -p "$results_dir"

cmake --preset benchmark
cmake --build --preset benchmark --parallel

{
  echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_commit=$(git rev-parse HEAD)"
  echo "git_dirty=$(if [[ -n "$(git status --porcelain)" ]]; then echo true; else echo false; fi)"
  echo "os=$(uname -a)"
  echo "cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -1 || echo unknown)"
  echo "compiler=$(${CXX:-c++} --version | head -1)"
  echo "cmake=$(cmake --version | head -1)"
  echo "ninja=$(ninja --version)"
  echo "power_mode=record manually if known; no pinning performed by this script"
} > "$results_dir/environment.txt"

build/benchmark/chronex_benchmarks \
  --benchmark_filter="$filter" \
  --benchmark_repetitions=5 \
  --benchmark_min_time=0.05s \
  --benchmark_out="$results_dir/benchmark.json" \
  --benchmark_out_format=json \
  --benchmark_report_aggregates_only=false
