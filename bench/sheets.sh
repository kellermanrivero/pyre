#!/usr/bin/env bash
# Prints the full paths of the test set (bench/sheets.txt), one a line.
# Use: build/pyre bench $(bench/sheets.sh) [options]
set -euo pipefail
DATA="${PYRE_MDT02:-/Volumes/KELLERMAN/PublicData/mdt02/PUBLICACION_CNIG_DATOS_VARIOS_MDT02_WGS84}"
here="$(cd "$(dirname "$0")" && pwd)"
grep -v '^#' "$here/sheets.txt" | awk '{print $1}' | while read -r f; do
  [ -n "$f" ] && echo "$DATA/$f"
done
