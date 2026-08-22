#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "usage: summarize-regression.sh WORK_DIR" >&2
}

(($# == 1)) || { usage; exit 2; }
work=$1
summary="$work/summary.tsv"
[[ -r $summary ]] || { echo "missing summary: $summary" >&2; exit 2; }

printf 'repeat\tindex\tstatus\trequested\tobserved_path\tinput\n'
direct=0
feature_fallback=0
decode_fallback=0
cpu=0
failed=0

while IFS=$'\t' read -r repeat index status requested input output bytes log; do
  [[ $repeat == repeat ]] && continue
  observed=unknown
  if [[ $status != ok ]]; then
    observed=failed
    ((++failed))
  elif [[ $requested == cpu ]]; then
    observed=cpu
    ((++cpu))
  elif grep -q 'direct decode failed; retrying with libheif CPU decoder' "$log" 2>/dev/null; then
    observed=cpu-fallback-decode-failure
    ((++decode_fallback))
  elif grep -q 'using libheif CPU fallback for non-NVDEC HEIC features' "$log" 2>/dev/null; then
    observed=cpu-fallback-feature
    ((++feature_fallback))
  else
    observed=direct
    ((++direct))
  fi
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$repeat" "$index" "$status" "$requested" "$observed" "$input"
done < "$summary"

{
  echo
  echo "Observed backend paths:"
  printf '  direct:                      %d\n' "$direct"
  printf '  CPU requested:               %d\n' "$cpu"
  printf '  CPU fallback (features):     %d\n' "$feature_fallback"
  printf '  CPU fallback (decode fail):  %d\n' "$decode_fallback"
  printf '  failed:                      %d\n' "$failed"
} >&2
