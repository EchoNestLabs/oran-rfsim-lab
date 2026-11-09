# open5gs-nms (optional web NMS with live topology)

A web NMS for the lab: dashboard, **Topology** (4G/5G architecture with
live interface state), **RAN Network** (connected gNBs and their UEs),
subscribers, logs and metrics. Source project:
https://github.com/paulmataruso/open5gs-nms

Requires open5gs v2.8.0 or newer (the pinned `OG5GS_REF` in
`../rfsim-compose/docker/open5gs.Dockerfile`). The NMS consumes the JSON
info endpoints (`AMF /gnb-info`, `/ue-info`, `SMF /pdu-info`) that 2.7.x
does not serve, and the `metrics:` blocks in the lab's
`config/open5gs/{amf,smf}.yaml`.

## Why it has to run on the lab VM

The backend joins the AMF container's network namespace
(`network_mode: "container:oran-amf"`) and takes the host PID namespace
(`pid: "host"`, privileged). Sharing the AMF netns is how it reaches the
5GC's `127.0.0.x` metrics endpoints; the host PID view lets its
service-status probe see every NF daemon. It also reads `/etc/open5gs`
(mounted read-only from the lab's config dir) and the `open5gs`
database in the lab's mongodb. It cannot run on an external
monitoring host. nginx listens on 80 and 8888; the frontend serves on
8083 because the lab's cAdvisor already owns 8081.

> Security note: this is an admin console for a lab. It binds
> `0.0.0.0`, runs privileged, and shares the host PID namespace.
> Set `FIRST_RUN_PASSWORD` before the first start, and keep the
> ports firewalled (restrict the instance security group to operator
> IPs only; the shipped AMI opens these ports to a single owner IP).
> Do not expose this deployment to the public internet.

## Usage

```bash
cd deploy/nms
./build.sh          # bake the images (done for shipped AMIs; needs internet once)
./start.sh          # start it (requires the lab stack to be up first)
```

Login: the NMS prompts to set an admin password on first sign-in
(export `FIRST_RUN_PASSWORD` beforehand to preseed it).

502 on `/api/vowifi` and `/api/mms` is normal; those optional upstream
modules are intentionally disabled in the frontend build. The Dashboard's
"Active services" counters rely on systemd inside the AMF container; the
shipped patch below falls back to a `/proc` scan instead.

If you recreate the AMF container, re-run `./start.sh` (the backend
namespace attachment follows the container, not the compose hash).

## Provenance and license (AGPL-3.0)

open5gs-nms is Copyright (C) 2026 Paul Mataruso, licensed under
**AGPL-3.0** (see `LICENSE` in the upstream repo; `THIRD_PARTY_NOTICES.md`
there lists its dependencies).

- Upstream commit pinned in `build.sh`: `54c405d634768c4cb27d3ef52d6e9c6114695aad`
  (main, 2026-11-04).
- Two source patches are applied by `build.sh`:
  `patches/0001-proc-service-status-fallback.patch` adds a `/proc` fallback
  in the backend's service-status probe, needed because no systemd runs
  in the AMF container (changes
  `backend/src/application/use-cases/service-monitor.ts` and
  `backend/src/domain/entities/service-status.ts`); and
  `patches/0002-mongodb-uri-host.patch` makes the backend derive the
  mongodb host from `MONGODB_URI` instead of a hardcoded loopback
  address. The patch files in this directory are the corresponding
  source for those changes under AGPL-3.0.
- Images built by `build.sh` carry `org.opencontainers.image.source`,
  `.revision` and `.licenses` labels. Anyone running the images can get
  the corresponding source by checking out the pinned commit and applying
  the patches in this directory.
