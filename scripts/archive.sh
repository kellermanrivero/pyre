#!/usr/bin/env bash
# Encodes each source raster below a folder into its own .pyre file, and verifies it.
#
# Use: scripts/archive.sh <source folder> <output folder> [epsilon 0.09] [unit 0.01]
#
# - One .pyre a raster, with the raster's name: X.tif gives X.pyre. pyre-resample reads
#   the output folder as it reads the rasters.
# - A raster whose .pyre exists is skipped, so a stopped run goes on where it stopped.
# - Each file is written as X.pyre.part and renamed to X.pyre only when `pyre verify`
#   passes against the raster. A failed one stays as .part; nothing is deleted.
# - One JSON line a raster goes to <output folder>/archive.jsonl.
# - It stops when the free disk under the output folder is less than PYRE_MIN_FREE_GB
#   (20 by default).
# - PYRE: the pyre binary (default build/pyre). PYRE_LIMIT: do at most N rasters.
set -uo pipefail

src="${1:?give the folder of the source rasters}"
out="${2:?give the output folder}"
eps="${3:-0.09}"
unit="${4:-0.01}"
pyre="${PYRE:-build/pyre}"
minFree="${PYRE_MIN_FREE_GB:-20}"
limit="${PYRE_LIMIT:-0}"

mkdir -p "$out"
log="$out/archive.jsonl"
done=0
failed=0
while IFS= read -r f; do
  name="$(basename "$f")"
  name="${name%.*}"
  dst="$out/$name.pyre"
  [ -e "$dst" ] && continue
  if [ "$limit" -gt 0 ] && [ "$done" -ge "$limit" ]; then break; fi
  free=$(df -Pk "$out" | awk 'NR == 2 { print int($4 / 1048576) }')
  if [ "$free" -lt "$minFree" ]; then
    echo "archive.sh: stopped: $free GB free under $out (the limit is $minFree GB)" >&2
    exit 3
  fi
  if ! e="$("$pyre" encode "$f" -o "$dst.part" --epsilon "$eps" --unit "$unit" --json)"; then
    echo "{\"sheet\":\"$name\",\"ok\":false,\"error\":\"encode failed\"}" >> "$log"
    failed=$((failed + 1))
    continue
  fi
  v="$("$pyre" verify "$dst.part" "$f" --json)"
  status=$?
  if [ $status -eq 0 ]; then
    mv "$dst.part" "$dst"
    echo "{\"sheet\":\"$name\",\"ok\":true,\"encode\":$e,\"verify\":$v}" >> "$log"
  else
    echo "{\"sheet\":\"$name\",\"ok\":false,\"error\":\"verify failed\",\"encode\":$e,\"verify\":${v:-null}}" >> "$log"
    failed=$((failed + 1))
  fi
  done=$((done + 1))
done < <(find "$src" -type f \( -iname '*.tif' -o -iname '*.tiff' \) | sort)
echo "archive.sh: $done rasters done in this run, $failed failed; log: $log"
[ $failed -eq 0 ]
