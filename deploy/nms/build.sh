#!/usr/bin/env bash
# Bake the open5gs-nms images for this repo.
#
# Provenance: upstream https://github.com/paulmataruso/open5gs-nms at the
# pinned commit below, plus the source patch in patches/ (see README.md
# for the license and corresponding-source notes).
#
# Usage:  ./build.sh [--no-clone]     (run after the AMI's docker is up;
#                                     --no-clone rebuilds from an existing .upstream)
set -euo pipefail
cd "$(dirname "$0")"

NMS_REPO_URL="https://github.com/paulmataruso/open5gs-nms"
NMS_SHA="54c405d634768c4cb27d3ef52d6e9c6114695aad"
SRC=".upstream"

if [[ "${1:-}" != "--no-clone" ]]; then
  rm -rf "$SRC"
  git init -q "$SRC"
  git -C "$SRC" remote add origin "$NMS_REPO_URL"
  git -C "$SRC" fetch -q --depth 1 origin "$NMS_SHA"
  git -C "$SRC" checkout -q --detach "$NMS_SHA"
fi
git -C "$SRC" checkout -q --detach "$NMS_SHA"

# Apply the source patches (idempotent: a patch is skipped when it no
# longer applies, i.e. it was already applied).
for p in patches/0*.patch; do
  [[ -e "$p" ]] || continue
  if patch -d "$SRC" -p1 --dry-run < "$p" >/dev/null 2>&1; then
    patch -d "$SRC" -p1 < "$p"
  fi
done

COMMON_LABELS=(
  --label org.opencontainers.image.source="$NMS_REPO_URL"
  --label org.opencontainers.image.revision="$NMS_SHA"
  --label org.opencontainers.image.licenses="AGPL-3.0"
)

docker build -t "oran/open5gs-nms-backend:$NMS_SHA" \
  "${COMMON_LABELS[@]}" "$SRC/backend"

docker build -t "oran/open5gs-nms-frontend:$NMS_SHA" \
  "${COMMON_LABELS[@]}" \
  --build-arg VITE_API_URL= \
  --build-arg VITE_WS_URL= \
  --build-arg VITE_PROMETHEUS_PORT=9090 \
  --build-arg VITE_GRAFANA_PORT=3000 \
  --build-arg VITE_ENABLE_SMS=false \
  --build-arg VITE_ENABLE_IMS=false \
  --build-arg VITE_ENABLE_VALIDATION=false \
  --build-arg VITE_ENABLE_VOWIFI=false \
  --build-arg VITE_ENABLE_DNS_MIGRATION=false \
  --build-arg VITE_ENABLE_PCAP=false \
  --build-arg VITE_ENABLE_PSTN=false \
  "$SRC/frontend"

echo "built oran/open5gs-nms-{backend,frontend}:$NMS_SHA"
