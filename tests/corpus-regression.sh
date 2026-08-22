#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
usage: corpus-regression.sh [options] INPUT_DIR

Run Csharp over a HEIC/HEIF corpus without modifying the source files.

Options:
  --binary PATH       csharp executable (default: ./build-direct/csharp)
  --backend NAME      direct or cpu (default: direct)
  --quality N         JPEG quality (default: 90)
  --repeat N          repeat whole corpus N times (default: 1)
  --work-dir PATH     output/log directory (default: temporary directory)
  --keep              keep temporary work directory on success
  --limit N           process at most N source files per repeat
  --verbose           print each conversion command/result
  -h, --help          show this help

The harness is intentionally read-only toward INPUT_DIR. Every output is written
under the work directory. Existing source files are never renamed or modified.
EOF
}

binary=./build-direct/csharp
backend=direct
quality=90
repeat=1
work_dir=
keep=0
limit=0
verbose=0

while (($#)); do
  case "$1" in
    --binary) binary=${2:?missing --binary value}; shift 2 ;;
    --backend) backend=${2:?missing --backend value}; shift 2 ;;
    --quality) quality=${2:?missing --quality value}; shift 2 ;;
    --repeat) repeat=${2:?missing --repeat value}; shift 2 ;;
    --work-dir) work_dir=${2:?missing --work-dir value}; shift 2 ;;
    --keep) keep=1; shift ;;
    --limit) limit=${2:?missing --limit value}; shift 2 ;;
    --verbose) verbose=1; shift ;;
    -h|--help) usage; exit 0 ;;
    --) shift; break ;;
    -*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    *) break ;;
  esac
done

if (($# != 1)); then
  usage >&2
  exit 2
fi

input_dir=$1

case "$backend" in direct|cpu) ;; *) echo "backend must be direct or cpu" >&2; exit 2 ;; esac
[[ $quality =~ ^[0-9]+$ ]] && ((quality >= 1 && quality <= 100)) || {
  echo "quality must be 1..100" >&2; exit 2;
}
[[ $repeat =~ ^[0-9]+$ ]] && ((repeat >= 1)) || {
  echo "repeat must be >= 1" >&2; exit 2;
}
[[ $limit =~ ^[0-9]+$ ]] || { echo "limit must be >= 0" >&2; exit 2; }
[[ -d $input_dir ]] || { echo "input directory not found: $input_dir" >&2; exit 2; }
[[ -x $binary ]] || { echo "csharp binary not executable: $binary" >&2; exit 2; }

created_temp=0
if [[ -z $work_dir ]]; then
  work_dir=$(mktemp -d "${TMPDIR:-/tmp}/csharp-regression.XXXXXX")
  created_temp=1
else
  mkdir -p "$work_dir"
fi

cleanup() {
  local status=$?
  if ((created_temp && !keep && status == 0)); then
    rm -rf -- "$work_dir"
  else
    echo "work directory: $work_dir" >&2
  fi
  exit "$status"
}
trap cleanup EXIT INT TERM

outputs=$work_dir/outputs
logs=$work_dir/logs
mkdir -p "$outputs" "$logs"
summary=$work_dir/summary.tsv
printf 'repeat\tindex\tstatus\tbackend_requested\tselected_decode_path\tfallback_reason\tinput\toutput\tbytes\tlog\treport\n' > "$summary"

mapfile -d '' files < <(
  find "$input_dir" -type f \( \
      -iname '*.heic' -o -iname '*.heif' -o -iname '*.hif' \
    \) -print0 | sort -z
)

if ((${#files[@]} == 0)); then
  echo "no HEIC/HEIF files found under: $input_dir" >&2
  exit 3
fi

if ((limit > 0 && ${#files[@]} > limit)); then
  files=("${files[@]:0:limit}")
fi

jpeg_valid() {
  local path=$1
  [[ -s $path ]] || return 1
  # Basic structural gate works without optional packages.
  local magic tail
  magic=$(od -An -tx1 -N2 "$path" | tr -d ' \n')
  tail=$(tail -c 2 "$path" | od -An -tx1 | tr -d ' \n')
  [[ $magic == ffd8 && $tail == ffd9 ]] || return 1

  # If an independent JPEG validator is installed, use it too.
  if command -v jpeginfo >/dev/null 2>&1; then
    jpeginfo -c "$path" >/dev/null 2>&1 || return 1
  elif command -v djpeg >/dev/null 2>&1; then
    djpeg -outfile /dev/null "$path" >/dev/null 2>&1 || return 1
  fi
}

ok=0
failed=0
total=0
start_epoch=$(date +%s)

for ((r=1; r<=repeat; ++r)); do
  index=0
  for input in "${files[@]}"; do
    ((++index))
    ((++total))
    rel=${input#"$input_dir"/}
    safe=$(printf '%s' "$rel" | tr '/\t\r\n' '____')
    output="$outputs/r${r}-$(printf '%05d' "$index")-${safe%.*}.jpg"
    log="$logs/r${r}-$(printf '%05d' "$index").log"
    report="$logs/r${r}-$(printf '%05d' "$index").tsv"

    if ((verbose)); then
      printf '[%d/%d r%d] %s\n' "$index" "${#files[@]}" "$r" "$input" >&2
    fi

    status=ok
    if ! "$binary" --backend "$backend" --quality "$quality" --report "$report" --verbose \
          "$input" "$output" >"$log.stdout" 2>"$log"; then
      status=convert-failed
    elif ! jpeg_valid "$output"; then
      status=invalid-jpeg
    fi

    if [[ $status == ok ]]; then
      ((++ok))
      bytes=$(stat -c '%s' "$output")
    else
      ((++failed))
      bytes=0
      echo "FAIL: $status: $input" >&2
      if ((verbose)); then sed -n '1,120p' "$log" >&2 || true; fi
    fi

    selected=unknown
    reason=unknown
    if [[ -s $report ]]; then
      IFS=$'\t' read -r _ _ _ _ _ reason _ _ _ _ _ _ _ _ _ _ _ < <(tail -n 1 "$report")
      IFS=$'\t' read -r _ _ _ selected _ _ _ _ _ _ _ _ _ _ _ _ _ < <(tail -n 1 "$report")
    fi
    printf '%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
      "$r" "$index" "$status" "$backend" "$selected" "$reason" \
      "$input" "$output" "$bytes" "$log" "$report" >> "$summary"
  done
done

elapsed=$(( $(date +%s) - start_epoch ))
printf 'Csharp corpus regression: total=%d ok=%d failed=%d elapsed=%ds backend=%s files=%d repeats=%d\n' \
  "$total" "$ok" "$failed" "$elapsed" "$backend" "${#files[@]}" "$repeat"
printf 'summary: %s\n' "$summary"

if ((failed != 0)); then
  exit 1
fi
