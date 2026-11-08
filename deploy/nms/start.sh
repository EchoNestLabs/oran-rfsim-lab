#!/usr/bin/env bash
# Start open5gs-nms against the running lab stack (see README.md).
# Exposes the UI on http://<lab-vm>:8888 (port 8888, plus 80).
# On first sign-in the NMS prompts to set the admin password; export
# FIRST_RUN_PASSWORD to preseed it.
set -euo pipefail
cd "$(dirname "$0")"

# The backend shares the AMF container's network namespace, so the nginx
# upstream must point at the AMF's fixed IP on oran-core-net (10.71.0.20
# in this repo's compose, but resolve it anyway in case that changes).
AMF_IP=$(docker inspect oran-amf --format '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}')
if [[ -z "$AMF_IP" ]]; then
  echo "error: oran-amf is not running - bring the lab up first (deploy/rfsim-compose/up.sh)" >&2
  exit 1
fi
sed "s/@@AMF_IP@@/$AMF_IP/" nginx/nms.conf.template > nginx/nms.conf

mkdir -p data
docker compose up -d
docker ps --filter name=open5gs-nms --format '{{.Names}}  {{.Status}}'
echo "UI: http://<lab-vm-ip>:8888"
