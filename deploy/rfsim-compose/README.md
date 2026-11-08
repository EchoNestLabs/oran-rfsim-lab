# Lab Deployment

Docker compose deployment of the lab on a single host, one container
per network function. The RAN is simulated: the gNB runs against OAI's
rfsimulator, so no RF hardware is needed.

- open5gs 5G core (12 network functions, built from the pinned v2.8.0
  tag by `docker/open5gs.Dockerfile`)
- OAI gNB in rfsim mode + 2 OAI nr-UEs (official v3.0.0 images)
- FlexRIC near-RT RIC and its KPM monitor xApp, built from one pinned
  FlexRIC source tree (`config/flexric-src/Dockerfile`)

## Prerequisites

- Docker with the compose v2 plugin (`docker compose version`), plus
  `docker.io docker-compose-v2` on Ubuntu (`./up.sh` offers to install both
  if missing)
- `/dev/net/tun` available on the host (the UE containers need TUN):
  `sudo modprobe tun`
- Disk: about 20 GB free for the images and build cache
- Time budget: the two source builds (open5gs, FlexRIC) take roughly
  45-60 minutes combined, once

No `.env` file is needed; image tags default to locally built images
(`OPEN5GS_TAG`, `FLEXRIC_SRC_TAG` may be overridden).

## Bring-up

```bash
./up.sh core   # build (one time) + start the open5gs core
./up.sh ran    # start gNB + UEs; builds the FlexRIC source image (one time)
./up.sh ric    # start near-RT RIC + KPM monitor xApp
```

Each slice is idempotent and can be re-run on a live stack. The `ran`
slice also self-heals the two states a host reboot or core restart
leaves behind: a missing `oaitun_ue1` tunnel (restarts that UE) and a
stale gNB-to-AMF SCTP association (restarts the gNB).

## Verify

```bash
./verify.sh core   # 12 network functions up, NRF SBI, AMF NGAP listening
./verify.sh ran    # gNB NGSetup, UE PDU sessions, oaitun tunnels
./verify.sh ric    # RIC up, E2 nodes registered, KPM indications flowing
```

Optional sample xApps (never part of the appliance boot; see
[`../../xapps/kpm-hello/`](../../xapps/kpm-hello/)):

```bash
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-hello
docker logs -f oran-kpm-hello
```

One PASS/FAIL line per gate, exit code 0 only when all gates pass.
The RAN/RIC gates check live state (SCTP association, tunnel addresses,
a sliding log window) rather than one-shot log lines, so they stay
accurate after hours of runtime - the high-rate gNB and monitor logs
would otherwise rotate those lines out of the 3x10 MB docker log
buffers within hours.

## What runs where

All containers sit on a dedicated `oran-core-net` bridge (10.71.0.0/24):
mongodb .10, amf .20, upf .30, gnb .40, ue1 .41, ue2 .42, near-rt-ric .50,
xapp-kpm-moni .51.

## FlexRIC note

The RIC plane is built from a pinned GitHub mirror of FlexRIC v3.0.0
(`config/flexric-src/Dockerfile` pins `duranta-project/flexric` at tag `v3.0.0`,
the same commit the official `scripts/install_asn1c.sh` flow expects).
Mixing the released `oaisoftwarealliance/oai-flexric:v3.0.0` image with
xApps built from the v3.0.0 source tag does not work: the two have
incompatible E2/E42 wire encodings. Keep the whole RIC plane on one
source tree, as `up.sh` does via `FLEXRIC_SRC_TAG`.

One more FlexRIC behaviour to know: the stock KPM monitor (and a
one-shot xApp like `kpm-hello`) subscribes once, when the first E2 node
appears. If the gNB later disappears and re-registers (e.g. the `ran`
slice restarted it), the old subscription is orphaned and no further
indications arrive. Re-run `./up.sh ric` - it restarts the monitor so
it re-subscribes to the current E2 node. `verify.sh ric` catches this
state ("KPM indications" FAILs with nothing in the last 15 s).


## Optional add-ons

- `../nms/` — open5gs-nms web NMS with the live 5GC topology (baked into
  the AMI, started with `./start.sh`). Note the open5gs pin here also
  feeds it: it needs the v2.8.0+ JSON info endpoints and the `metrics:`
  blocks in `config/open5gs/{amf,smf}.yaml`.
- `../monitoring/` — external monitoring tier (node-exporter + cAdvisor
  on this VM, Prometheus + Grafana elsewhere).
