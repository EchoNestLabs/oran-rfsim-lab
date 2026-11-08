# kpm-exporter xApp

Near-RT telemetry sample: the same subscription loop as
[`kpm-hello`](../kpm-hello/) (E2SM-KPM cell-level style 1 + per-UE style 4,
1 s period, SST=1 filter) with the print loop replaced by a Prometheus text
endpoint, so anything that can scrape can see RAN KPIs.

One C file, no metrics library: a mutex-guarded gauge table written by the
E2 indication callback, and a ~100-line POSIX-socket HTTP server serving
`:8080/metrics`.

Exposed metrics (hello-world sized: 8 gauges + 1 counter):

| Metric | Source | Labels |
|---|---|---|
| `kpm_up` | exporter liveness | - |
| `kpm_reports_total` | indications received | - |
| `kpm_cell_prb_dl` | `RRU.PrbTotDl` | `cell` |
| `kpm_cell_prb_ul` | `RRU.PrbTotUl` | `cell` |
| `kpm_cell_rrc_conn_mean` | `RRC.ConnMean` | `cell` |
| `kpm_ue_thp_dl` | `DRB.UEThpDl` | `ue_id` |
| `kpm_ue_thp_ul` | `DRB.UEThpUl` | `ue_id` |
| `kpm_ue_rlc_sdu_delay_dl` | `DRB.RlcSduDelayDl` | `ue_id` |

UE (and cell) entries disappear from the exposition after 30 s without a
report, so UEs the RAN stops reporting do not stick around. Note this is
gated on the RAN: while the gNB keeps a departed UE in its per-UE reports
(e.g. a UE that died without a clean detach keeps its context for a
while), the row persists with the last known values.

From `deploy/rfsim-compose/` (with the lab up: core, ran, ric):

```bash
docker compose -f docker-compose.yml -f docker-compose.samples.yml \
  --profile samples up --build kpm-exporter
curl -s localhost:8080/metrics
```

Example:

```
kpm_up 1
kpm_reports_total 412
kpm_cell_prb_dl{cell="3584"} 12
kpm_cell_rrc_conn_mean{cell="3584"} 2
kpm_ue_thp_dl{ue_id="1"} 0.11
kpm_ue_rlc_sdu_delay_dl{ue_id="1"} 2.10
```

To feed the optional monitoring tier
([`deploy/monitoring/`](../../deploy/monitoring/)), add a scrape job
pointing at the exporter container (`kpm-exporter:8080`, or
`localhost:8080` from the VM). Dashboards are deliberately not shipped
with the sample.
