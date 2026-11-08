#!/usr/bin/env bash
# Bring up the oran-lab recipe: docker prereqs -> selected stack. Idempotent.
# Usage: ./up.sh [core|ran|ric]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

command -v docker >/dev/null 2>&1 || {
  echo "== installing docker prereqs =="
  if [ "$(id -u)" = 0 ]; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq docker.io docker-compose-v2 >/dev/null
  else
    sudo apt-get update -qq
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq docker.io docker-compose-v2 >/dev/null
    sudo usermod -aG docker "${SUDO_USER:-$USER}" 2>/dev/null || true
    echo "NOTE: re-login for docker group or run: sg docker -c './up.sh $*'"
  fi
}
docker compose version >/dev/null 2>&1 || {
  echo "docker compose v2 missing (install docker-compose-v2)" >&2
  exit 1
}

# rfsim/UE containers need kernel TUN available on the host
[ -e /dev/net/tun ] || {
  echo "/dev/net/tun missing: sudo modprobe tun" >&2
  exit 1
}

sl="${1:-core}"

# FlexRIC source image (nearRT-RIC + example xapps + SM libs),
# built from config/flexric-src against the pinned v3.0.0 SDK. The released
# oai-flexric image has an incompatible E42 wire format - do not mix.
FLEXRIC_SRC_TAG="${FLEXRIC_SRC_TAG:-oran/flexric-src:v1}"
ensure_flexric_src_img() {
  if ! docker image inspect "$FLEXRIC_SRC_TAG" >/dev/null 2>&1; then
    if [ -f "${FLEXRIC_SRC_CACHE:-$FLEXRIC_SRC_TAG}.tar" ]; then
      echo "== loading $FLEXRIC_SRC_TAG from cached tar =="
      docker load -i "${FLEXRIC_SRC_CACHE:-$FLEXRIC_SRC_TAG}.tar"
    else
      echo "== building $FLEXRIC_SRC_TAG from pinned flexric v3.0.0 source (one-time, ~30-45 min) =="
      docker build -t "$FLEXRIC_SRC_TAG" config/flexric-src
    fi
  fi
}

IMG="${OPEN5GS_TAG:-oran/open5gs:v2.8.0}"
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "== building $IMG from pinned open5gs tag (one-time, ~10-15 min) =="
  docker build -f docker/open5gs.Dockerfile --build-arg OG5GS_REF=v2.8.0 -t "$IMG" .
fi

case "$sl" in
  core)
    echo "== bringing up slice: $sl =="
    docker compose --profile core up -d
    ;;
  ran)
    echo "== ensuring core slice is up =="
    docker compose --profile core up -d
    # seed subscribers (one-shot; dbctl at the same pinned open5gs tag)
    mkdir -p bin
    curl -sfo bin/dbctl.sh "https://raw.githubusercontent.com/open5gs/open5gs/v2.8.0/misc/db/open5gs-dbctl" || {
      echo "failed to download open5gs-dbctl" >&2; exit 1;
    }
    chmod +x bin/dbctl.sh
    docker compose --profile ran-init up --exit-code-from dbinit dbinit || {
      echo "dbinit failed" >&2; exit 1;
    }
    echo "== bringing up slice: ran (OAI rfsim gNB+2xUE) =="
    ensure_flexric_src_img
    # RIC before the gNB so the E2 agent connects on first attempt
    docker compose --profile ric up -d --no-deps near-rt-ric
    docker compose --profile ran up -d
    # Self-heal stale NGAP: if the 5GC was (re)started while the gNB kept
    # running, the gNB's SCTP association to the AMF can stay half-open and
    # NGSetup never completes. A gNB restart re-registers it.
    sleep 45
    if ! docker logs --since 10m oran-gnb 2>&1 | grep -q "NGSetupResponse"; then
      echo "== gNB: no NGSetup seen, restarting gNB =="
      docker restart oran-gnb >/dev/null
      sleep 45
    fi
    # Self-heal stale UE tunnels: after a host reboot the oai-nr-ue containers
    # may come back with a stale (non-existent) oaitun_ue1 device even though
    # the PDU session registered fine. A UE restart re-attaches the tunnel.
    for ue in oran-ue1 oran-ue2; do
      if [ "$(docker exec "$ue" ip link show oaitun_ue1 >/dev/null 2>&1 && echo ok || echo miss)" = "miss" ]; then
        echo "== $ue: tunnel missing, restarting UE container =="
        docker restart "$ue" >/dev/null
      fi
    done
    sleep 30
    # KPM monitor xapp last (needs RIC + gNB with their E2s up)
    docker compose --profile ric up -d --no-deps xapp-kpm-moni
    ;;
  ric)
    ensure_flexric_src_img
    echo "== ensuring core+ric slices are up =="
    docker compose --profile core up -d
    docker compose --profile ric up -d --no-deps near-rt-ric
    # KPM monitor needs the RIC up (and the gNB for E2 indications).
    # force-recreate: a running monitor holds a one-shot subscription that a
    # RAN restart orphaned, and a plain 'up -d' would leave it unsubscribed.
    docker compose --profile ric up -d --no-deps --force-recreate xapp-kpm-moni
    ;;
  *)
    echo "unknown slice: $sl (core|ran|ric)" >&2; exit 1
    ;;
esac

echo "== waiting for healthy/core =="
# docker compose ps Status reads "Up ..." ("... (healthy)" when a
# healthcheck is defined); anything neither Up nor exited cleanly is
# counted as unhealthy.
for _ in $(seq 1 60); do
  unhealthy=$(docker compose ps --format '{{.Name}} {{.Status}}' \
    | grep -cv 'Up \|exited (0)' || true)
  [ "$unhealthy" = "0" ] && break
  sleep 2
done
docker compose ps
echo "== done: ./verify.sh $sl =="
