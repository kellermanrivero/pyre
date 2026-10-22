#!/usr/bin/env bash
# The second pass on a folder of .pyre files: adds layer kind 1 (the height of
# objects above the ground) to each, from a surface model (for example the MDS02).
#
# Use: scripts/add-layer.sh <folder of .pyre files> <surface rasters or folder> [pyre encode options…]
#      (for example --unit 3 --min 3 --stride 2)
#
# - A file that has a layer of kind 1 already is skipped, so a stopped run goes on.
# - Each file is written as X.pyre.part, checked with `pyre verify --layer`, and only
#   then renamed over X.pyre. A failed one stays as .part; nothing is deleted.
# - One JSON line a file goes to <folder>/layer.jsonl.
# - It stops when the free disk is less than PYRE_MIN_FREE_GB (20 by default).
# - PYRE: the pyre binary (default build/pyre). PYRE_LIMIT: do at most N files.
set -uo pipefail

dir="${1:?give the folder of .pyre files}"
surface="${2:?give the surface rasters or their folder}"
shift 2
pyre="${PYRE:-build/pyre}"
minFree="${PYRE_MIN_FREE_GB:-20}"
limit="${PYRE_LIMIT:-0}"
log="$dir/layer.jsonl"
done=0
failed=0
while IFS= read -r f; do
  if "$pyre" info "$f" --json | grep -q '"kind":1,'; then continue; fi
  if [ "$limit" -gt 0 ] && [ "$done" -ge "$limit" ]; then break; fi
  free=$(df -Pk "$dir" | awk 'NR == 2 { print int($4 / 1048576) }')
  if [ "$free" -lt "$minFree" ]; then
    echo "add-layer.sh: stopped: $free GB free under $dir (the limit is $minFree GB)" >&2
    exit 3
  fi
  name="$(basename "$f" .pyre)"
  if ! e="$("$pyre" encode "$surface" --layer-of "$f" --kind 1 -o "$f.part" "$@" --json)"; then
    echo "{\"file\":\"$name\",\"ok\":false,\"error\":\"encode failed\"}" >> "$log"
    failed=$((failed + 1))
    continue
  fi
  v="$("$pyre" verify "$f.part" --layer 1 "$surface" --json)"
  status=$?
  if [ $status -eq 0 ]; then
    mv "$f.part" "$f"
    echo "{\"file\":\"$name\",\"ok\":true,\"encode\":$e,\"verify\":$v}" >> "$log"
  else
    echo "{\"file\":\"$name\",\"ok\":false,\"error\":\"verify failed\",\"encode\":$e,\"verify\":${v:-null}}" >> "$log"
    failed=$((failed + 1))
  fi
  done=$((done + 1))
done < <(find "$dir" -maxdepth 1 -type f -name '*.pyre' | sort)
echo "add-layer.sh: $done files done in this run, $failed failed; log: $log"
[ $failed -eq 0 ]
