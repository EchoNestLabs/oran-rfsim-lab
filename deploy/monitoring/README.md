# External Monitoring Tier

Optional monitoring for the lab, running outside it. The lab VM keeps
doing what it does; a second host does the watching.

```
+----------------------+          +---------------------------+
| monitor VM           |  scrape  | lab VM (AMI)              |
| prometheus, grafana  | <------> | node-exporter, cadvisor   |
|                      |  pull    | + the 17 lab containers   |
+----------------------+          +---------------------------+
```

Both roles are defined in one compose file, split by profiles:

- `exporters` runs on the lab VM and needs no configuration
- `monitor` runs on the monitor VM and needs one value: the lab VM IP

## Bring-up

On the lab VM (adds 2 containers, touch nothing else):

```bash
cd deploy/monitoring
./up.sh exporters
```

On the monitor VM:

```bash
cd deploy/monitoring
./up.sh monitor   # or: LAB_HOST=<lab ip> ./up.sh monitor
```

`up.sh monitor` writes `prometheus.yml` from the template with the lab
IP filled in, then starts Prometheus and a Grafana with the datasource
pre-provisioned (Grafana on port 3000, admin login on first visit).

Open firewall for the exporter ports on the lab VM (security group or
ufw) so only the monitor VM can reach `9100` (node-exporter) and
`8081` (cAdvisor).

## What you get

- `lab-host` job: CPU, memory, disk and network of the lab VM
- `lab-containers` job: per-container state and resource usage from
  cAdvisor. A container crash (for example `oran-gnb`) is visible here
  in seconds, no `docker ps` needed.

Useful quick checks:

- container restarts: `rate(container_start_time_seconds{job="lab-containers"}[5m])`
- memory per lab container: `container_memory_working_set_bytes{job="lab-containers"}`

## Notes

- The exporters are read-only observers; `up.sh` and `verify.sh` of the
  lab are unaffected (the container count from a stock boot changes
  from 17 to 19 with exporters running).
- open5gs also exposes status endpoints; adding them
  as scrape jobs is a one-line edit in `prometheus.yml.template`.
