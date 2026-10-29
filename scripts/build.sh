#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the vambed-neo image from the pins in pins.env.
# usage: scripts/build.sh [image tag, default vambed-neo:local] [extra docker build args...]
set -euo pipefail
cd "$(dirname "$0")/.."

. ./pins.env
tag="${1:-vambed-neo:local}"
shift || true

for v in BASE_IMAGE MBELIB_NEO_URL MBELIB_NEO_COMMIT CODEC2_URL CODEC2_COMMIT; do
  [ -n "${!v:-}" ] || { echo "build.sh: $v missing in pins.env" >&2; exit 2; }
done
for v in MBELIB_NEO_COMMIT CODEC2_COMMIT; do
  [[ "${!v}" =~ ^[0-9a-f]{40}$ ]] || { echo "build.sh: $v is not a 40-hex commit" >&2; exit 2; }
done

exec docker build -t "$tag" \
  --build-arg BASE_IMAGE="$BASE_IMAGE" \
  --build-arg MBELIB_NEO_URL="$MBELIB_NEO_URL" \
  --build-arg MBELIB_NEO_COMMIT="$MBELIB_NEO_COMMIT" \
  --build-arg CODEC2_URL="$CODEC2_URL" \
  --build-arg CODEC2_COMMIT="$CODEC2_COMMIT" \
  "$@" .
