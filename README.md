# O-RAN rfsim Lab

[![CI](https://github.com/EchoNestLabs/oran-rfsim-lab/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/EchoNestLabs/oran-rfsim-lab/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

A reproducible Open RAN lab that runs on a single Docker host: an OAI gNB
in rfsim mode with two OAI nr-UEs, the FlexRIC near-RT RIC and the open5gs
5G core. Example xApps and rApps build on that foundation, working toward a
closed control loop over the RAN. Maintained by
[**EchoNest Labs**](https://echonest.se).

The sample loop has three stages:

1. Telemetry: an xApp collects RAN performance measurements from the gNB
   over E2. [`kpm-hello`](xapps/kpm-hello/) is the shipped sample; the lab
   also runs FlexRIC's stock KPM monitor.
2. Decision: an rApp consumes the KPIs and produces a control decision
   (planned).
3. Control: an xApp applies that decision over E2 towards the gNB
   (planned). The FlexRIC SDK image ships the full xApp toolchain for it.

## What is in the repository

| Directory | Status | Description |
|---|---|---|
| [`deploy/rfsim-compose/`](deploy/rfsim-compose/) | **working** | The lab itself. This directory holds the single-host rf-simulated lab with docker compose profiles for the 5GC, RAN and RIC planes, plus bring-up and verify scripts. Optional add-ons: [`deploy/monitoring/`](deploy/monitoring/) and [`deploy/nms/`](deploy/nms/) (open5gs-nms topology web UI) |
| [`rapps/`](rapps/) | planned | rApps: the non-real-time control loop, from KPIs to decision to actuation |
| [`xapps/`](xapps/) | partial | xApps. [`kpm-hello`](xapps/kpm-hello/) ships as a hello-world telemetry sample; the KPM monitor running in the lab is FlexRIC's own example binary. More samples are planned, see the folder READMEs |
| [`dashboards/`](dashboards/) | planned | Grafana dashboards for the samples |
| [`replay/`](replay/) | planned | Recorded KPI traces, so the control loop can run without a RAN |
| [`docs/`](docs/) | partial | Documentation index: AMI usage, lab bring-up |

## Quick start

The quickest way to run the lab is the prebuilt machine image (see Machine
image below). If you would rather build everything from source, see
[`deploy/rfsim-compose/README.md`](deploy/rfsim-compose/README.md). On a
Docker host with TUN enabled:

```bash
cd deploy/rfsim-compose
./up.sh core   # open5gs 5G core (builds the pinned core image, one time)
./up.sh ran    # OAI gNB + 2 UEs in rfsim mode (also builds the FlexRIC source image on first run)
./up.sh ric    # FlexRIC near-RT RIC + KPM monitor xApp
./verify.sh core && ./verify.sh ran && ./verify.sh ric
```

Each `verify.sh <slice>` prints one PASS/FAIL line per gate and exits
non-zero if anything fails. All slices are idempotent, so re-running them
is safe.

## Sample applications

Samples live in [`xapps/`](xapps/) and [`rapps/`](rapps/). They are never
part of the appliance boot; they build on demand against the pinned FlexRIC
source that ships in the SDK image.

Start with [`xapps/kpm-hello`](xapps/kpm-hello/), a ~300-line xApp that
subscribes to E2SM-KPM and prints cell-level and per-UE measurements every
second:

```bash
cd deploy/rfsim-compose   # with the lab up (core, ran, ric)
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-hello
docker logs -f oran-kpm-hello
```

Then try [`xapps/kpm-exporter`](xapps/kpm-exporter/). It runs the same
subscription behind a Prometheus text endpoint, so the RAN KPIs become
scrapeable metrics instead of scrolling logs:

```bash
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-exporter
curl -s localhost:8080/metrics
```

## Machine image

The whole lab is published as a public EC2 machine image. Nothing needs
building; the stack starts by itself on first boot.

AMI name: `echonest-labs-oran-rfsim-lab-1.2.3-ubuntu22.04-x86_64`

| Region | AMI ID |
|---|---|
| `eu-north-1` | `ami-04b16c12f9e1973b3` |
| `us-east-1` | `ami-0ddcd9e2ebe5ef7ef` |

A few things worth knowing before you launch:

- Use a t3.2xlarge. The 17-container stack needs the 32 GB of RAM, and the
  image runs fine on a spot instance.
- 40 GB of gp3 storage on the root volume is enough.
- Access goes through AWS Session Manager (SSM). The image ships no SSH
  keys and regenerates SSH host keys at first boot. SSM needs an instance
  profile: either enable [default host management
  configuration](https://docs.aws.amazon.com/systems-manager/latest/userguide/default-host-management.html)
  on your account, or attach a role with the `AmazonSSMManagedInstanceCore`
  policy when you launch.
- Software versions: open5gs v2.8.0, OAI gNB and nr-UE v3.0.0 (rfsim),
  FlexRIC v3.0.0 built from a pinned source tag.

Connect once the instance is running:

```bash
aws ssm start-session --target <instance-id>
```

Sessions open as the `ssm-user` account, which has passwordless sudo. The
docker socket is root-only, so become root before you touch the lab:

```bash
sudo -i
cd /home/ubuntu/rfsim-compose   # the appliance copy of deploy/rfsim-compose/
```

On first boot you get 17 containers: the 12 open5gs network functions, the
OAI gNB and two UEs, and the near-RT RIC with its stock KPM monitor. The
stack is ready within a few minutes, and `docker ps -q | wc -l` prints 17
when it is. Check each layer:

```bash
cd /home/ubuntu/rfsim-compose
./verify.sh core   # 5GC: 12 network functions up, NRF SBI, AMF NGAP
./verify.sh ran    # gNB NGSetup, UE PDU sessions + oaitun tunnels
./verify.sh ric    # near-RT RIC, E2 nodes registered, KPM indications
```

The RAN and RIC gates check live state: SCTP associations, tunnel
addresses, a sliding window over recent logs. That keeps them accurate even
after hours of uptime. If a RIC gate fails right after a RAN restart, run
`./up.sh ric` once to resubscribe the KPM monitor.

> Two copies of the deployment tree live on the image. The appliance copy
> at `/home/ubuntu/rfsim-compose` is what the stack and `up.sh` /
> `verify.sh` use. The repo checkout at `/home/ubuntu/oran` is what you use
> to build samples. Stack operations go to the appliance copy, sample
> builds go to the repo checkout.

Running the sample xApp on the instance:

```bash
cd /home/ubuntu/oran/deploy/rfsim-compose   # repo checkout on the image
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-hello
docker logs -f oran-kpm-hello
```

If you want to write your own xApp, the image already contains the full
FlexRIC v3.0.0 SDK (image `oran/flexric-src:v1`, source in `/src/flexric`)
with the E2 endpoint reachable and KPM report dumps under `/tmp` inside the
container.
[`xapps/kpm-hello`](xapps/kpm-hello/) is a complete, minimal example of
building against it.

> This is an EchoNest Labs appliance, not an official image of open5gs,
> OAI, FlexRIC or the O-RAN Alliance. Everything in it is open source
> under its own license.

Two optional extras are baked into the image but off by default:

- `deploy/monitoring/` runs an external monitoring tier: node-exporter and
  cAdvisor on the VM itself, with Prometheus and Grafana elsewhere.
- `deploy/nms/` is the open5gs-nms web UI showing the live 5GC topology.
  It runs on the lab VM; see that folder's README (AGPL-3.0).

## License

The original code in this repository is Apache License 2.0, see
[LICENSE](LICENSE). Some components carry their own licenses:

- The sample xApps in [`xapps/`](xapps/) are simplified derivatives of the
  FlexRIC v3.0.0 example code, so they ship under the same Collaborative
  Standards Software License v1.0 (CSSL). The license text is vendored at
  [`xapps/LICENSES/LicenseRef-CSSL-1.0.txt`](xapps/LICENSES/LicenseRef-CSSL-1.0.txt).
- [`deploy/nms/`](deploy/nms/) wraps the AGPL-3.0 open5gs-nms web UI. That
  folder's README covers provenance and the corresponding source patches.
- open5gs, OAI and FlexRIC keep their own licenses and are pulled at
  pinned versions.
