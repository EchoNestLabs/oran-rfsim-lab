#!/usr/bin/env bash
# Bring up the monitoring tier on this host, by role:
#   exporters (run on the lab VM):        ./up.sh exporters
#   monitor   (run on the monitor VM):    LAB_HOST=<lab ip> ./up.sh monitor
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
role="${1:-}"
compose() { docker compose -f "$here/docker-compose.yml" "$@"; }

case "$role" in
  exporters)
    compose --profile exporters up -d
    docker ps --format 'table {{.Names}}\t{{.Status}}' | grep oran-
    ;;
  monitor)
    if [ -z "${LAB_HOST:-}" ]; then
      echo "usage: LAB_HOST=<lab vm ip> ./up.sh monitor" >&2
      exit 2
    fi
    sed "s/@@LAB_HOST@@/${LAB_HOST}/" "$here/prometheus.yml.template" \
      > "$here/prometheus.yml"
    compose --profile monitor up -d
    echo "ready: prometheus :9090, grafana :3000 (targets: ${LAB_HOST})"
    ;;
  *)
    echo "usage: ./up.sh exporters | LAB_HOST=<lab ip> ./up.sh monitor" >&2
    exit 2
    ;;
esac
