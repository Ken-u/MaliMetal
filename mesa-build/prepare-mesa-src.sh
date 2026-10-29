#!/bin/bash
# Copy the Mesa source tree (26.1-devel, with PanVK) into $MESA_BUILD/src/mesa and apply ../mesa-patches/*.patch in order.
# The original tree is never modified. Re-running starts from a fresh copy.
#   MESA_SRC_ORIG   plain copy of the Mesa tree (an AOSP external/mesa3d checkout works)
#   MESA_BUILD      work directory of the PanVK build
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/env.sh"
: "${MESA_BUILD:?set MESA_BUILD in .env}" "${MESA_SRC_ORIG:?set MESA_SRC_ORIG in .env}"
DST="${1:-$MESA_BUILD/src/mesa}"
mkdir -p "$(dirname "$DST")"
rm -rf "$DST"
rsync -a --exclude .git "$MESA_SRC_ORIG"/ "$DST"/
for p in "$MALIMETAL_ROOT"/mesa-patches/*.patch; do
	patch -p1 -s -d "$DST" < "$p" || { echo "error: $(basename "$p") does not apply" >&2; exit 1; }
	echo "applied $(basename "$p")"
done
