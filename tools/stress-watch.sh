#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: stress-watch.sh [options] INPUT_DIR

Run the corpus regression harness while sampling process RSS/FD counts and, when
available, NVIDIA GPU memory/utilization. Intended for 1k/10k hardening runs.

Options:
  --binary PATH       csharp executable (default: ./build-direct/csharp)
  --backend NAME      direct or cpu (default: direct)
  --repeat N          corpus repeats (default: 1)
  --work-dir PATH     persistent run directory
  --interval SEC      sampling interval (default: 1)
  -h, --help          show help
EOF
}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary=./build-direct/csharp
backend=direct
repeat=1
work_dir=
interval=1

while (($#)); do
  case "$1" in
    --binary) binary=${2:?missing value}; shift 2 ;;
    --backend) backend=${2:?missing value}; shift 2 ;;
    --repeat) repeat=${2:?missing value}; shift 2 ;;
    --work-dir) work_dir=${2:?missing value}; shift 2 ;;
    --interval) interval=${2:?missing value}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) break ;;
  esac
done

(($# == 1)) || { usage >&2; exit 2; }
input=$1
[[ -d $input ]] || { echo "input directory not found: $input" >&2; exit 2; }
[[ -x $binary ]] || { echo "csharp binary not executable: $binary" >&2; exit 2; }
[[ $backend == direct || $backend == cpu ]] || { echo "backend must be direct or cpu" >&2; exit 2; }
[[ $repeat =~ ^[0-9]+$ ]] && ((repeat >= 1)) || { echo "repeat must be >=1" >&2; exit 2; }

if [[ -z $work_dir ]]; then
  work_dir=$(mktemp -d "${TMPDIR:-/tmp}/csharp-stress.XXXXXX")
else
  mkdir -p "$work_dir"
fi
samples="$work_dir/resources.tsv"
runlog="$work_dir/harness.log"
printf 'epoch\tpid\trss_kib\tfds\tgpu_mem_mib\tgpu_util_pct\tdecoder_util_pct\n' > "$samples"

"$root/tests/corpus-regression.sh" \
  --binary "$binary" --backend "$backend" --repeat "$repeat" \
  --work-dir "$work_dir/regression" --keep "$input" >"$runlog" 2>&1 &
harness_pid=$!

cleanup() {
  local status=$?
  if kill -0 "$harness_pid" 2>/dev/null; then kill "$harness_pid" 2>/dev/null || true; fi
  echo "stress run directory: $work_dir" >&2
  exit "$status"
}
trap cleanup INT TERM

while kill -0 "$harness_pid" 2>/dev/null; do
  epoch=$(date +%s)
  rss=0
  fds=0
  if [[ -r /proc/$harness_pid/status ]]; then
    rss=$(awk '/^VmRSS:/ {print $2+0}' "/proc/$harness_pid/status")
    fds=$(find "/proc/$harness_pid/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)
  fi
  gpu_mem=-1
  gpu_util=-1
  dec_util=-1
  if command -v nvidia-smi >/dev/null 2>&1; then
    IFS=',' read -r gpu_mem gpu_util < <(nvidia-smi --query-gpu=memory.used,utilization.gpu --format=csv,noheader,nounits 2>/dev/null | head -n1 | tr -d ' ' || printf '%s,%s\n' -1 -1)
    if nvidia-smi dmon -c 1 -s u >/dev/null 2>&1; then
      dec_util=$(nvidia-smi dmon -c 1 -s u 2>/dev/null | awk '!/^#/ && NF>=6 {print $6; exit}')
      [[ $dec_util =~ ^[0-9]+$ ]] || dec_util=-1
    fi
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$epoch" "$harness_pid" "$rss" "$fds" "$gpu_mem" "$gpu_util" "$dec_util" >> "$samples"
  sleep "$interval"
done

set +e
wait "$harness_pid"
status=$?
set -e

printf 'resource samples: %s\n' "$samples"
printf 'harness log: %s\n' "$runlog"
awk -F '\t' 'NR>1 {if($3>maxrss)maxrss=$3; if($4>maxfd)maxfd=$4; if($5>maxgpu)maxgpu=$5} END {printf "peak RSS: %d KiB\npeak FDs: %d\npeak GPU memory: %d MiB\n", maxrss+0,maxfd+0,maxgpu+0}' "$samples"
exit "$status"
