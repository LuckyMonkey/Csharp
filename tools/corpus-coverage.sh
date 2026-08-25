#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: corpus-coverage.sh [--inspect PATH] INPUT_DIR

Classify HEIC/HEIF files by whether Csharp's current v0.1 policy predicts the
direct NVDEC fast path or libheif CPU fallback. Prints a TSV file plus a compact
coverage summary. Source files are read-only.
EOF
}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
inspect="$root/build-tools/csharp-inspect"

while (($#)); do
  case "$1" in
    --inspect) inspect=${2:?missing --inspect value}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) break ;;
  esac
done

(($# == 1)) || { usage >&2; exit 2; }
input=$1
[[ -d $input ]] || { echo "not a directory: $input" >&2; exit 2; }
[[ -x $inspect ]] || {
  echo "csharp-inspect not found: $inspect" >&2
  echo "run: tools/build-tools.sh" >&2
  exit 2
}

mapfile -d '' files < <(find "$input" -type f \( -iname '*.heic' -o -iname '*.heif' -o -iname '*.hif' \) -print0 | sort -z)
((${#files[@]})) || { echo "no HEIC/HEIF files found" >&2; exit 3; }

report=${CSHARP_COVERAGE_REPORT:-/tmp/csharp-coverage-$$.tsv}
"$inspect" --tsv "${files[@]}" > "$report"

total=$(awk 'NR>1 {n++} END {print n+0}' "$report")
direct=$(awk -F '\t' 'NR>1 && $12==1 {n++} END {print n+0}' "$report")
fallback=$(awk -F '\t' 'NR>1 && $12==0 {n++} END {print n+0}' "$report")
alpha=$(awk -F '\t' 'NR>1 && $6==1 {n++} END {print n+0}' "$report")
non8=$(awk -F '\t' 'NR>1 && ($4!=8 || $5!=8) {n++} END {print n+0}' "$report")
metadata=$(awk -F '\t' 'NR>1 && $9>0 {n++} END {print n+0}' "$report")
icc=$(awk -F '\t' 'NR>1 && $10>0 {n++} END {print n+0}' "$report")

pct=0
if ((total)); then pct=$(awk -v d="$direct" -v t="$total" 'BEGIN {printf "%.1f", 100*d/t}'); fi

printf 'Csharp corpus coverage\n'
printf '  files:            %d\n' "$total"
printf '  direct eligible:  %d (%s%%)\n' "$direct" "$pct"
printf '  CPU fallback:     %d\n' "$fallback"
printf '  alpha:            %d\n' "$alpha"
printf '  non-8-bit:        %d\n' "$non8"
printf '  metadata blocks:  %d files\n' "$metadata"
printf '  ICC profiles:     %d files\n' "$icc"
printf '  report:           %s\n' "$report"
