#!/usr/bin/env bash
# Checks a folder of .pyre files made by scripts/archive.sh (and add-layer.sh)
# against their sources: the heights of X.pyre against <heights folder>/X.tif, and,
# if the file has layer 1 (kind 1), that layer against the surface rasters.
#
# Use: scripts/verify-archive.sh <folder of .pyre files> <heights rasters folder> [surface folder]
#
# - Each file's heights source is <heights folder>/<file name>.tif, else
#   <surface folder>/<file name>.tif (a sheet kept from its surface model); a file
#   whose source is in neither is reported, not checked.
# - One JSON line a file goes to <folder>/verify.jsonl. A run skips the files that
#   the log has as passed, so a stopped run goes on where it stopped; with
#   PYRE_VERIFY_FRESH=1 the log is emptied and every file is checked again.
# - Exits non-zero if any check fails. Reads only; changes no .pyre file.
# - PYRE: the pyre binary (default build/pyre).
set -uo pipefail

dir="${1:?give the folder of .pyre files}"
heights="${2:?give the folder of the heights rasters}"
surface="${3:-}"
pyre="${PYRE:-build/pyre}"
log="$dir/verify.jsonl"
[ "${PYRE_VERIFY_FRESH:-0}" = 1 ] && : > "$log"
touch "$log"
checked=0
failed=0
while IFS= read -r f; do
  name="$(basename "$f" .pyre)"
  grep -q "^{\"file\":\"$name\",\"ok\":true" "$log" && continue
  src="$heights/$name.tif"
  [ ! -e "$src" ] && [ -n "$surface" ] && src="$surface/$name.tif"
  if [ ! -e "$src" ]; then
    echo "{\"file\":\"$name\",\"ok\":false,\"error\":\"no source $src\"}" >> "$log"
    failed=$((failed + 1))
    continue
  fi
  h="$("$pyre" verify "$f" "$src" --json)"
  hs=$?
  l="null"
  ls=0
  if [ -n "$surface" ] && "$pyre" info "$f" --json | grep -q '"kind":1,'; then
    l="$("$pyre" verify "$f" --layer 1 "$surface" --json)"
    ls=$?
  fi
  ok=true
  if [ $hs -ne 0 ] || [ $ls -ne 0 ]; then ok=false; failed=$((failed + 1)); fi
  echo "{\"file\":\"$name\",\"ok\":$ok,\"heights\":${h:-null},\"layer\":${l:-null}}" >> "$log"
  checked=$((checked + 1))
done < <(find "$dir" -maxdepth 1 -type f -name '*.pyre' | sort)
echo "verify-archive.sh: $checked files checked, $failed failed; log: $log"
[ $failed -eq 0 ]
