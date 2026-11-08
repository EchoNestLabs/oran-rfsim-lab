# O-RAN rfsim Lab

[![CI](https://github.com/EchoNestLabs/oran-rfsim-lab/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/EchoNestLabs/oran-rfsim-lab/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

A reproducible Open RAN lab — OAI gNB in rfsim mode with OAI nr-UEs, the
FlexRIC near-RT RIC and the open5gs 5G core, on a single Docker host —
plus example xApps and rApps demonstrating closed-loop RAN intelligence
on top of it. Maintained by [**EchoNest Labs**](https://echonest.se).

The sample loop has three stages:

- **Telemetry**: an xApp collects RAN key performance measurements from
  the gNB over E2 — [`kpm-hello`](xapps/kpm-hello/) is the shipped sample;
  the lab also runs FlexRIC's stock KPM monitor.
- **Decision**: an rApp consumes KPIs and produces a control decision
  (planned).
- **Control**: an xApp applies the decision over E2 towards the gNB
  (planned; the FlexRIC SDK image ships the full xApp toolchain).

## Contents

| Directory | Status | Description |
|---|---|---|
| [`deploy/`](deploy/rfsim-compose/) | **working** | Deployment variants; [`rfsim-compose/`](deploy/rfsim-compose/) currently holds the single-host rf-simulated lab: docker compose profiles for the 5GC, RAN and RIC planes, bring-up and verify scripts. Optional add-ons: [`deploy/monitoring/`](deploy/monitoring/) (external monitoring tier) and [`deploy/nms/`](deploy/nms/) (open5gs-nms topology web UI) |
| [`rapps/`](rapps/) | planned | rApps (non-RT control loop: KPIs to decision to actuation) |
| [`xapps/`](xapps/) | partial | xApps: [`kpm-hello`](xapps/kpm-hello/) is a shipped hello-world telemetry sample; the built-in KPM monitor running in the lab is FlexRIC's example binary; other samples are planned (see the folder READMEs) |
| [`dashboards/`](dashboards/) | planned | Grafana dashboards for the samples |
| [`replay/`](replay/) | planned | Recorded KPI traces for RAN-free execution |
| [`docs/`](docs/) | partial | Documentation index and pointers (AMI usage, lab bring-up) |

## Quick start

The fastest way to run the lab is the prebuilt machine image (below).
To build everything from source instead, see
[`deploy/rfsim-compose/README.md`](deploy/rfsim-compose/README.md).
In short, on a Docker host with TUN enabled:

```bash
cd deploy/rfsim-compose
./up.sh core   # open5gs 5G core (builds the pinned core image, one time)
./up.sh ran    # OAI gNB + 2 UEs in rfsim mode (builds the FlexRIC source image, one time)
./up.sh ric    # FlexRIC near-RT RIC + KPM monitor xApp
./verify.sh core && ./verify.sh ran && ./verify.sh ric
```

Each `verify.sh <slice>` prints one PASS/FAIL line per gate and exits
non-zero on any failure. All slices are idempotent and safe to re-run.

## Sample applications

Sample apps live in [`xapps/`](xapps/) and [`rapps/`](rapps/) and are
**never** part of the appliance boot — they are built on demand against
the pinned FlexRIC source that ships in the SDK image. Start with
[`xapps/kpm-hello`](xapps/kpm-hello/): a ~300-line xApp that subscribes to
E2SM-KPM and prints per-second cell-level and per-UE measurements.

```bash
cd deploy/rfsim-compose   # with the lab up (core, ran, ric)
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-hello
docker logs -f oran-kpm-hello
```

Next, [`xapps/kpm-exporter`](xapps/kpm-exporter/): the same subscription
as a Prometheus text endpoint, so the RAN KPIs become scrapeable metrics
instead of scrolling logs.

```bash
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-exporter
curl -s localhost:8080/metrics
```

## Machine image (AMI)

The full lab is published as a public AWS EC2 machine image.
No build is required; the stack starts automatically on first boot.

AMI name: `echonest-labs-oran-rfsim-lab-1.2.3-ubuntu22.04-x86_64`

AMI IDs per region will be listed here when the image is published.

> **Two copies of the deployment tree exist on the image**: the
> appliance copy at `/home/ubuntu/rfsim-compose` (used for the boot
> above and for `up.sh`/`verify.sh`) and the repo checkout at
> `/home/ubuntu/oran` (used to build the samples, see below). Keep
> them straight: stack operations use the appliance copy, sample
> builds use the repo checkout.

- **Recommended instance**: t3.2xlarge (32 GB RAM; the 17-container stack
  needs it). Spot works fine.
- **Storage**: 40 GB gp3 root.
- **Access**: AWS Session Manager (SSM) only. The image ships no SSH keys,
  and SSH host keys are regenerated at first boot. SSM needs an instance
  profile: either enable [default host management
  configuration](https://docs.aws.amazon.com/systems-manager/latest/userguide/default-host-management.html)
  on your account, or create a role with the `AmazonSSMManagedInstanceCore`
  policy and select it under "IAM instance profile" when launching.
- **Stack versions**: open5gs v2.8.0, OAI gNB + nr-UE v3.0.0 (rfsim),
  FlexRIC v3.0.0 (built from a pinned source tag).

When the instance is running, connect (no SSH keys involved):

```bash
aws ssm start-session --target <instance-id>
```

Sessions open as the `ssm-user` account, which has passwordless sudo;
the docker socket is root-only, so become root before touching the lab:

```bash
sudo -i
cd /home/ubuntu/rfsim-compose   # the appliance copy of deploy/rfsim-compose/
```

The 17 containers on first boot are the 12 open5gs network functions,
the OAI gNB and 2 UEs, and the FlexRIC near-RT RIC with its stock KPM
monitor xApp. The stack is ready within a few minutes
(`docker ps -q | wc -l` prints 17). Verify each layer:

```bash
cd /home/ubuntu/rfsim-compose
./verify.sh core   # 5GC: 12 network functions up, NRF SBI, AMF NGAP
./verify.sh ran    # gNB NGSetup, UE PDU sessions + oaitun tunnels
./verify.sh ric    # near-RT RIC, E2 nodes registered, KPM indications
```

The RAN/RIC gates check live state (SCTP association, tunnel
addresses, a sliding log window), so they stay accurate after hours of
uptime; if a RIC gate fails right after a RAN restart, re-run
`./up.sh ric` to resubscribe the KPM monitor.

Running the sample xApp on the instance (verified on this image):

```bash
cd /home/ubuntu/oran/deploy/rfsim-compose   # repo checkout on the image
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-hello
docker logs -f oran-kpm-hello
```

Writing your own xApp: the image contains the full FlexRIC v3.0.0 SDK
(`oran/flexric-src:v1`, source in `/src/flexric`) with the E2 endpoint
and KPM reports under `/tmp` — [`xapps/kpm-hello`](xapps/kpm-hello/) is a
complete, minimal example of building against it.

> **Provenance note:** this is an EchoNest Labs appliance, not an
> official image of open5gs, OAI, FlexRIC or the O-RAN Alliance.
> All included components are open source under their own licenses.

Optional extras baked into the image but left off by default:

- `deploy/monitoring/` — external monitoring tier (node-exporter,
  cAdvisor on the VM; Prometheus + Grafana elsewhere).
- `deploy/nms/` — the open5gs-nms web NMS with the live 5GC topology
  (runs on the lab VM itself; see that folder's README, AGPL-3.0).

## License

Apache License 2.0 for this repository's original code, see
[LICENSE](LICENSE). Components that differ carry their own license:

- The sample xApps in [`xapps/`](xapps/) are simplified derivatives of
  FlexRIC v3.0.0 example code and are therefore licensed under the
  same Collaborative Standards Software License v1.0 (CSSL) — license
  text vendored at
  [`xapps/LICENSES/LicenseRef-CSSL-1.0.txt`](xapps/LICENSES/LicenseRef-CSSL-1.0.txt).
- [`deploy/nms/`](deploy/nms/) wraps the AGPL-3.0 open5gs-nms web UI;
  see that folder's README for provenance and the corresponding
  source patches.
- open5gs, OAI and FlexRIC themselves ship under their own licenses
  and are pulled at pinned versions.
