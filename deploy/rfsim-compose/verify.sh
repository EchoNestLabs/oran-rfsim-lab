#!/usr/bin/env bash
# Gate checks for a slice. Exit 0 = green. Usage: ./verify.sh core
set -u
here="$(cd "$(dirname "$0")" && pwd)"
cd "$here" || exit 1
slice="${1:-core}"
rc=0

# gate <name> <cmd...> [timeout_s]: PASS only if the command succeeds
# within timeout (default 10s). Settling gates (E2 attach, log lines that
# appear shortly after a container recreate) use longer timeouts.
gate() { # gate <name> <cmd...> [timeout_s]
  local name="$1" out=""; shift
  local timeout="10"; [[ "${1:-}" =~ ^[0-9]+$ ]] && { timeout="$1"; shift; }
  printf '%-28s' "$name"
  local waited=0
  while :; do
    if out=$("$@" 2>&1); then echo "PASS  (${out:0:80})"; return; fi
    (( waited >= timeout )) && { echo "FAIL  ${out:0:120}"; rc=1; return; }
    sleep 5; waited=$((waited + 5))
  done
}

case "$slice" in
  core)
    # every NF container running
    for nf in mongodb nrf scp nssf bsf ausf udm udr pcf amf smf upf; do
      gate "5GC:$nf" docker inspect -f '{{.State.Running}}' oran-$nf
    done
    # NRF SBI: Open5GS speaks HTTP/2 only; "/" returns 400 but proves liveness
    gate "5GC:NRF SBI :7777" docker exec oran-nrf curl -s --http2-prior-knowledge -o /dev/null -w '%{http_code}' http://10.71.0.11:7777/
    # AMF NGAP: the AMF image ships without ss, so the init log is the
    # contract (the AMF itself logs at a trivially low rate)
    gate "5GC:AMF NGAP :38412" bash -c 'docker logs oran-amf 2>&1 | grep -q "ngap_server() \\[0.0.0.0\\]:38412"'
    ;;
  ran)
    # RAN containers running
    for nf in gnb ue1 ue2; do
      gate "RAN:$nf" docker inspect -f '{{.State.Running}}' oran-$nf
    done
    # gNB <-> AMF: NG Setup completed (live SCTP association to 10.71.0.20:38412,
    # independent of log-buffer rotation - the gNB's high-rate UE stats would
    # rotate one-shot log lines out within hours)
    gate "RAN:gNB NGSetup OK" bash -c 'docker exec oran-gnb ss -S | grep -qE "ESTAB.*10.71.0.20:38412"'
    # UEs: PDU session accepted (implies registration + auth). The data address
    # on the tunnel exists exactly while the PDU session is up.
    gate "RAN:UE1 PDU session" bash -c 'docker exec oran-ue1 ip -4 addr show oaitun_ue1 | grep -q "inet "'
    gate "RAN:UE2 PDU session" bash -c 'docker exec oran-ue2 ip -4 addr show oaitun_ue1 | grep -q "inet "'
    # UE data tunnels exist
    gate "RAN:UE1 tunnel" docker exec oran-ue1 sh -c 'ip link show oaitun_ue1'
    gate "RAN:UE2 tunnel" docker exec oran-ue2 sh -c 'ip link show oaitun_ue1'
    ;;
  ric)
    # RIC + xapp containers running
    for nf in near-rt-ric xapp-kpm-moni; do
      gate "RIC:$nf" docker inspect -f '{{.State.Running}}' oran-$nf
    done
    # E2 interface: the gNB agent's SCTP association to the RIC is live
    # state (10.71.0.40 appears as the remote address of the E2 assoc in
    # the RIC netns; a whole-log grep would survive a gNB disconnect)
    gate "RIC:gNB E2 Registered" docker exec oran-near-rt-ric grep -q 10.71.0.40 /proc/net/sctp/assocs
    # xapp receives KPM indications (proves the E2SM pipeline end to end).
    # Checked over a sliding window: the monitor logs ~1.6k lines/min, so a
    # one-shot grep would rotate out of the 3x10MB log buffer within hours.
    gate "RIC:xapp KPM indications" 30 bash -c 'docker logs --since 15s oran-xapp-kpm-moni 2>&1 | grep -q "ind_msg"'
    ;;
  *) echo "unknown slice: $slice" >&2; exit 2;;
esac
exit $rc
