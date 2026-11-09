# kpm-hello xApp

The hello world of this repo: a single-file xApp (~300 lines of C) that
subscribes to E2SM-KPM measurements from the gNB through the near-RT RIC and
prints what it receives, once a second: cell-level metrics plus one report
per UE. It is pure observation; the xApp never sends anything towards the
RAN.

This is the E2/KPM equivalent of NVIDIA's `prb-power` sample in
[aerial-sample-apps](https://github.com/NVIDIA/aerial-sample-apps): subscribe
to one stream of RAN data, do one small thing with it, show the output.

## What it demonstrates

| Building block | Where to look |
|---|---|
| Connecting an xApp to the near-RT RIC (E2) | `main()`: `init_xapp_api` / `e2_nodes_xapp_api` |
| Discovering the KPM RAN function and its report styles | `find_sm_idx` / the subscription loop |
| Subscribing to periodic E2SM-KPM reports | `gen_kpm_subs` (event trigger + action definition, both formats 1 and 4) |
| Handling indications and parsing measurements | `sm_cb_kpm` / `print_meas_report` |

The per-UE subscription uses Action Definition Format 4 and filters UEs by
S-NSSAI SST=1, the SST of the subscribers seeded by the lab. Cell-level
reports arrive regardless and cover the rest.

## Run it

You need the lab up (at least `core`, `ran`, `ric` slices). The sample is
not part of the appliance boot; it is built on demand against the pinned
FlexRIC source that ships in the lab's SDK image (`oran/flexric-src:v1`,
source in `/src/flexric`).

From `deploy/rfsim-compose/`:

```bash
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-hello
docker logs -f oran-kpm-hello
```

Expected output (shape is representative; names and values follow what the
gNB reports):

```
Connected E2 nodes = 1
E2 node 0: nb_id 1, mcc 1, mnc 1, ran_type gNB_MONLITHIC
kpm-hello: subscribed to E2SM-KPM, reports every 1000 ms (Ctrl+C to stop)

[kpm-hello] report
[kpm-hello] cell-level
  RRU.PrbTotDl = 12
  RRU.PrbTotUl = 4
  ...
[kpm-hello] report
[kpm-hello] UE ran_ue_id=1
  DRB.UEThpDl = 0.00
  RRU.PrbTotDl = 12
  ...
[kpm-hello] UE ran_ue_id=2
  ...
```

Tip: start an iperf3 through one UE and watch `DRB.UEThpDl` / `RRU.PrbTotDl`
move. See the repo README for the lab bring-up.

## Build and layout

```
xapps/kpm-hello/
├── kpm_hello.c     # the whole sample
├── CMakeLists.txt  # mirrors upstream examples (links e42_xapp)
├── Dockerfile      # FROM oran/flexric-src:v1, builds in /src/flexric
└── README.md
```

The Dockerfile copies the source into the SDK image's FlexRIC tree as a new
upstream-style example (`examples/xApp/c/hello`) and builds it there. No
existing FlexRIC sources are modified, and the SDK image itself is never
rebuilt.

## Next samples

- [`xapps/kpm-exporter`](../kpm-exporter/): the same loop plus a
  `/metrics` endpoint for Prometheus (the `deploy/monitoring/` tier can
  scrape it). Shipped and verified.
- [`rapps/slice-assurance`](../../rapps/slice-assurance/): a non-RT
  decision loop consuming exported KPIs. Planned.

See the READMEs for the current state.
