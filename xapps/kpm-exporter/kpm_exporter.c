/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 * Copyright (C) 2026 EchoNest Labs
 *
 * kpm-exporter: the kpm-hello sample with the print loop replaced by a
 * Prometheus text endpoint. Same KPM subscription (cell-level Format 1 +
 * per-UE Format 4, 1 s period, SST=1 filter); the indication callback
 * updates a small gauge table and a tiny HTTP server serves it on
 * :8080/metrics.
 *
 * Derived from FlexRIC v3.0.0 example code (via kpm-hello); licensed
 * under the same Collaborative Standards Software License v1.0 (CSSL);
 * see ../LICENSES/LicenseRef-CSSL-1.0.txt.
 *
 * Deliberately hello-world scope: ~8 gauges, POSIX sockets only, no
 * config, no TLS, no dashboard. Anything more ambitious wants a real
 * exporter (and a real metrics library).
 */

#include "xApp/e42_xapp_api.h"
#include "util/byte_array.h"
#include "util/e.h"
#include "util/ngran_types.h"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t const period_ms = 1000;  // one report per second
static int const EXPORTER_PORT = 8080;
// A UE (or the cell report) that has not been heard from for this long is
// dropped from the exposition so detached UEs do not stick around forever.
static double const STALE_S = 30.0;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
  (void)sig;
  g_stop = 1;
}

// ---------- gauge table (written by the E2 callback, read by the server) ----

#define MAX_UE_SLOTS 8

typedef struct {
  unsigned long ue_id;
  double thp_dl;
  double thp_ul;
  double rlc_delay_dl;
  struct timespec ts;
  bool used;
} ue_slot_t;

static struct {
  pthread_mutex_t mtx;
  unsigned long nb_id;
  double prb_dl;
  double prb_ul;
  double rrc_conn_mean;
  struct timespec ts;
  bool used;
  ue_slot_t ue[MAX_UE_SLOTS];
  unsigned long reports_total;
} g_tbl = {.mtx = PTHREAD_MUTEX_INITIALIZER};

static double elapsed_s(struct timespec const* from, struct timespec const* to)
{
  return (double)(to->tv_sec - from->tv_sec) + (double)(to->tv_nsec - from->tv_nsec) / 1e9;
}

// One measurement record pair (name, value) from a report block. Cell
// metrics update the cell fields; per-UE metrics update the given slot.
static void put_metric(char const* name, meas_record_lst_t const* rec, ue_slot_t* ue)
{
  double v = 0.0;
  if (rec->value == REAL_MEAS_VALUE)
    v = rec->real_val;
  else if (rec->value == INTEGER_MEAS_VALUE)
    v = (double)rec->int_val;
  else
    return; // "no value" - skip rather than export a zero

  if (strcmp(name, "RRU.PrbTotDl") == 0)
    g_tbl.prb_dl = v;
  else if (strcmp(name, "RRU.PrbTotUl") == 0)
    g_tbl.prb_ul = v;
  else if (strcmp(name, "RRC.ConnMean") == 0)
    g_tbl.rrc_conn_mean = v;
  else if (ue != NULL && strcmp(name, "DRB.UEThpDl") == 0)
    ue->thp_dl = v;
  else if (ue != NULL && strcmp(name, "DRB.UEThpUl") == 0)
    ue->thp_ul = v;
  else if (ue != NULL && strcmp(name, "DRB.RlcSduDelayDl") == 0)
    ue->rlc_delay_dl = v;
  // any other metric is deliberately not exported (hello-world scope)
}

static void walk_report(kpm_ind_msg_format_1_t const* f1, ue_slot_t* ue /* NULL = cell */)
{
  for (size_t j = 0; j < f1->meas_data_lst_len; ++j) {
    meas_data_lst_t const* data = &f1->meas_data_lst[j];
    for (size_t i = 0; i < f1->meas_info_lst_len; ++i) {
      meas_info_format_1_lst_t const* info = &f1->meas_info_lst[i];
      if (info->meas_type.type != NAME_MEAS_TYPE)
        continue;
      char* name = cp_ba_to_str(info->meas_type.name);
      for (size_t z = 0; z < info->label_info_lst_len; ++z) {
        if (info->label_info_lst[z].noLabel == NULL)
          continue;
        if (i + z >= data->meas_record_len)
          break; // short report from the E2 node - skip rather than over-read
        put_metric(name, &data->meas_record_lst[i + z], ue);
      }
      free(name);
    }
  }
}

static ue_slot_t* ue_slot_for(unsigned long ran_ue_id)
{
  for (size_t i = 0; i < MAX_UE_SLOTS; ++i) {
    if (g_tbl.ue[i].used && g_tbl.ue[i].ue_id == ran_ue_id)
      return &g_tbl.ue[i];
  }
  for (size_t i = 0; i < MAX_UE_SLOTS; ++i) {
    if (!g_tbl.ue[i].used) {
      memset(&g_tbl.ue[i], 0, sizeof(g_tbl.ue[i]));
      g_tbl.ue[i].used = true;
      g_tbl.ue[i].ue_id = ran_ue_id;
      return &g_tbl.ue[i];
    }
  }
  // Table full: reclaim the stalest slot (>= STALE_S with no report) instead
  // of dropping the UE for good - slot exhaustion with UE re-attaches would
  // otherwise silence all UEs from then on. Values start at 0; the next
  // report for this ue_id fills them.
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  ue_slot_t* victim = NULL;
  for (size_t i = 0; i < MAX_UE_SLOTS; ++i) {
    ue_slot_t* slot = &g_tbl.ue[i];
    if (elapsed_s(&slot->ts, &now) < STALE_S)
      continue;
    if (victim == NULL || elapsed_s(&slot->ts, &now) > elapsed_s(&victim->ts, &now))
      victim = slot;
  }
  if (victim != NULL) {
    memset(victim, 0, sizeof(*victim));
    victim->used = true;
    victim->ue_id = ran_ue_id;
  }
  return victim; // NULL only if all 8 slots were refreshed within STALE_S
}

static unsigned long ue_id_of(ue_id_e2sm_t const* ue)
{
  if (ue->type == GNB_UE_ID_E2SM && ue->gnb.ran_ue_id != NULL)
    return (unsigned long)*ue->gnb.ran_ue_id;
  if (ue->type == GNB_UE_ID_E2SM)
    return (unsigned long)ue->gnb.amf_ue_ngap_id;
  if (ue->type == GNB_DU_UE_ID_E2SM)
    return (unsigned long)ue->gnb_du.gnb_cu_ue_f1ap;
  if (ue->type == GNB_CU_UP_UE_ID_E2SM)
    return (unsigned long)ue->gnb_cu_up.gnb_cu_cp_ue_e1ap;
  return 0;
}

// ---------- E2 indication callback -------------------------------------------

static void sm_cb_kpm(sm_ag_if_rd_t const* rd)
{
  assert(rd != NULL);
  assert(rd->type == INDICATION_MSG_AGENT_IF_ANS_V0);
  assert(rd->ind.type == KPM_STATS_V3_0);

  kpm_ind_data_t const* ind = &rd->ind.kpm.ind;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  pthread_mutex_lock(&g_tbl.mtx);
  ++g_tbl.reports_total;

  if (ind->msg.type == FORMAT_1_INDICATION_MESSAGE) {
    // cell-level report (subscription format 1). nb_id was captured at
    // startup from the E2 node (the indication header does not carry it).
    // Freshness of the CELL block is tied to cell reports only; a format-3
    // per-UE report must not keep stale cell PRB/RRC values alive.
    g_tbl.used = true;
    g_tbl.ts = now;
    walk_report(&ind->msg.frm_1, NULL);
  } else if (ind->msg.type == FORMAT_3_INDICATION_MESSAGE) {
    kpm_ind_msg_format_3_t const* f3 = &ind->msg.frm_3;
    for (size_t i = 0; i < f3->ue_meas_report_lst_len; ++i) {
      ue_slot_t* ue = ue_slot_for(ue_id_of(&f3->meas_report_per_ue[i].ue_meas_report_lst));
      if (ue != NULL) {
        ue->ts = now;
        walk_report(&f3->meas_report_per_ue[i].ind_msg_format_1, ue);
      }
    }
  }
  pthread_mutex_unlock(&g_tbl.mtx);
}

// ---------- Prometheus exposition --------------------------------------------

static void line(char** p, char const* end, char const* fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  size_t room = (size_t)(end - *p);
  int n = vsnprintf(*p, room, fmt, ap);
  va_end(ap);
  // vsnprintf returns the WOULD-BE length: clamp to end so a full buffer can
  // never advance the pointer past it (size is unsigned, would wrap).
  if (n < 0 || (size_t)n >= room)
    *p = (char*)end;
  else
    *p += n;
}

static void fill_metrics_body(char* buf, size_t cap)
{
  char* p = buf;
  char* const end = buf + cap;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);

  line(&p, end, "# HELP kpm_up Whether the exporter is running.\n");
  line(&p, end, "# TYPE kpm_up gauge\n");
  line(&p, end, "kpm_up 1\n");

  pthread_mutex_lock(&g_tbl.mtx);

  // Declare each family exactly once, then list all rows for it.
  line(&p, end, "# HELP kpm_reports_total KPM indications received from the RIC.\n");
  line(&p, end, "# TYPE kpm_reports_total counter\n");
  line(&p, end, "kpm_reports_total %lu\n", g_tbl.reports_total);

  line(&p, end, "# HELP kpm_cell_prb_dl Downlink PRBs in use (RRU.PrbTotDl).\n");
  line(&p, end, "# TYPE kpm_cell_prb_dl gauge\n");
  line(&p, end, "# HELP kpm_cell_prb_ul Uplink PRBs in use (RRU.PrbTotUl).\n");
  line(&p, end, "# TYPE kpm_cell_prb_ul gauge\n");
  line(&p, end, "# HELP kpm_cell_rrc_conn_mean Mean connected UEs (RRC.ConnMean).\n");
  line(&p, end, "# TYPE kpm_cell_rrc_conn_mean gauge\n");
  line(&p, end, "# HELP kpm_ue_thp_dl UE downlink throughput (DRB.UEThpDl).\n");
  line(&p, end, "# TYPE kpm_ue_thp_dl gauge\n");
  line(&p, end, "# HELP kpm_ue_thp_ul UE uplink throughput (DRB.UEThpUl).\n");
  line(&p, end, "# TYPE kpm_ue_thp_ul gauge\n");
  line(&p, end, "# HELP kpm_ue_rlc_sdu_delay_dl UE DL RLC SDU delay (DRB.RlcSduDelayDl).\n");
  line(&p, end, "# TYPE kpm_ue_rlc_sdu_delay_dl gauge\n");

  if (g_tbl.used && elapsed_s(&g_tbl.ts, &now) < STALE_S) {
    line(&p, end, "kpm_cell_prb_dl{cell=\"%lu\"} %.0f\n", g_tbl.nb_id, g_tbl.prb_dl);
    line(&p, end, "kpm_cell_prb_ul{cell=\"%lu\"} %.0f\n", g_tbl.nb_id, g_tbl.prb_ul);
    line(&p, end, "kpm_cell_rrc_conn_mean{cell=\"%lu\"} %.0f\n", g_tbl.nb_id,
         g_tbl.rrc_conn_mean);
  }

  for (size_t i = 0; i < MAX_UE_SLOTS; ++i) {
    ue_slot_t const* ue = &g_tbl.ue[i];
    if (!ue->used || elapsed_s(&ue->ts, &now) >= STALE_S)
      continue;
    line(&p, end, "kpm_ue_thp_dl{ue_id=\"%lu\"} %.2f\n", ue->ue_id, ue->thp_dl);
    line(&p, end, "kpm_ue_thp_ul{ue_id=\"%lu\"} %.2f\n", ue->ue_id, ue->thp_ul);
    line(&p, end, "kpm_ue_rlc_sdu_delay_dl{ue_id=\"%lu\"} %.2f\n", ue->ue_id,
         ue->rlc_delay_dl);
  }

  pthread_mutex_unlock(&g_tbl.mtx);
}

// ---------- minimal HTTP server ----------------------------------------------

static int g_srv_fd = -1;

// A client that connects but never finishes its request (half-open probe,
// stalled scraper) must not wedge the single accept loop - or shutdown.
static void client_timeouts(int cfd)
{
  struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void serve_client(int cfd)
{
  char req[2048] = {0};
  ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
  if (n <= 0)
    return;

  char body[8192];
  if (strncmp(req, "GET /metrics", 12) == 0) {
    fill_metrics_body(body, sizeof(body));
    char head[192];
    snprintf(head, sizeof(head),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/plain; version=0.0.4\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n\r\n",
             strlen(body));
    // MSG_NOSIGNAL: a scraper that closes early (scrape timeout) must not
    // SIGPIPE-kill the xApp with its subscriptions left on the RIC.
    send(cfd, head, strlen(head), MSG_NOSIGNAL);
    send(cfd, body, strlen(body), MSG_NOSIGNAL);
  } else {
    char const* resp =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 10\r\n"
        "Connection: close\r\n\r\n"
        "not found\n";
    send(cfd, resp, strlen(resp), MSG_NOSIGNAL);
  }
}

static void* http_thread(void* arg)
{
  (void)arg;
  while (!g_stop) {
    struct pollfd pfd = {.fd = g_srv_fd, .events = POLLIN};
    int r = poll(&pfd, 1, 1000);
    if (r <= 0)
      continue; // timeout (re-check g_stop) or transient error
    int cfd = accept(g_srv_fd, NULL, NULL);
    if (cfd < 0)
      continue;
    client_timeouts(cfd);
    serve_client(cfd);
    close(cfd);
  }
  return NULL;
}

static int open_server_socket(int port)
{
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 8) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// ---------- subscription generation (identical to kpm-hello) -----------------

// Filter predicate for Action Definition Format 4: match UEs by S-NSSAI.
// SST=1 matches the subscribers seeded by the lab (open5gs-dbctl default).
static test_info_lst_t filter_predicate(test_cond_type_e type, test_cond_e cond, int value)
{
  test_info_lst_t dst = {0};

  dst.test_cond_type = type;
  dst.S_NSSAI = TRUE_TEST_COND_TYPE;

  dst.test_cond = calloc(1, sizeof(test_cond_e));
  assert(dst.test_cond != NULL && "Memory exhausted");
  *dst.test_cond = cond;

  dst.test_cond_value = calloc(1, sizeof(test_cond_value_t));
  assert(dst.test_cond_value != NULL && "Memory exhausted");
  dst.test_cond_value->type = OCTET_STRING_TEST_COND_VALUE;

  dst.test_cond_value->octet_string_value = calloc(1, sizeof(byte_array_t));
  assert(dst.test_cond_value->octet_string_value != NULL && "Memory exhausted");
  const size_t len_nssai = 1;
  dst.test_cond_value->octet_string_value->len = len_nssai;
  dst.test_cond_value->octet_string_value->buf = calloc(len_nssai, sizeof(uint8_t));
  assert(dst.test_cond_value->octet_string_value->buf != NULL && "Memory exhausted");
  dst.test_cond_value->octet_string_value->buf[0] = value;

  return dst;
}

// Request every KPI the report style advertises, unlabelled (no bins).
static kpm_act_def_format_1_t fill_act_def_frm_1(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);

  kpm_act_def_format_1_t ad_frm_1 = {0};

  size_t const sz = report_item->meas_info_for_action_lst_len;
  ad_frm_1.meas_info_lst_len = sz;
  ad_frm_1.meas_info_lst = calloc(sz, sizeof(meas_info_format_1_lst_t));
  assert(ad_frm_1.meas_info_lst != NULL && "Memory exhausted");

  for (size_t i = 0; i < sz; ++i) {
    meas_info_format_1_lst_t* meas_item = &ad_frm_1.meas_info_lst[i];

    // 8.3.9: measurement by name
    meas_item->meas_type.type = NAME_MEAS_TYPE;
    meas_item->meas_type.name = copy_byte_array(report_item->meas_info_for_action_lst[i].name);

    // 8.3.11: a single noLabel entry, so each metric yields one record
    meas_item->label_info_lst_len = 1;
    meas_item->label_info_lst = ecalloc(1, sizeof(label_info_lst_t));
    label_info_lst_t label_item = {0};
    label_item.noLabel = ecalloc(1, sizeof(enum_value_e));
    *label_item.noLabel = TRUE_ENUM_VALUE;
    meas_item->label_info_lst[0] = label_item;
  }

  // 8.3.8: measurement granularity period
  ad_frm_1.gran_period_ms = period_ms;

  // 8.3.20 (optional): all cells
  ad_frm_1.cell_global_id = NULL;

#if defined(KPM_V2_03) || defined(KPM_V3_00)
  ad_frm_1.meas_bin_range_info_lst_len = 0;
  ad_frm_1.meas_bin_info_lst = NULL;
#endif

  return ad_frm_1;
}

static kpm_act_def_t fill_report_style_1(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);
  assert(report_item->act_def_format_type == FORMAT_1_ACTION_DEFINITION);

  kpm_act_def_t act_def = {.type = FORMAT_1_ACTION_DEFINITION};
  act_def.frm_1 = fill_act_def_frm_1(report_item);
  return act_def;
}

static kpm_act_def_t fill_report_style_4(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);
  assert(report_item->act_def_format_type == FORMAT_4_ACTION_DEFINITION);

  kpm_act_def_t act_def = {.type = FORMAT_4_ACTION_DEFINITION};

  // Matching condition: UEs with S-NSSAI SST=1 (as xapp_kpm_moni does)
  act_def.frm_4.matching_cond_lst_len = 1;
  act_def.frm_4.matching_cond_lst = calloc(act_def.frm_4.matching_cond_lst_len,
                                           sizeof(matching_condition_format_4_lst_t));
  assert(act_def.frm_4.matching_cond_lst != NULL && "Memory exhausted");

  test_cond_type_e const type = S_NSSAI_TEST_COND_TYPE;
  test_cond_e const condition = EQUAL_TEST_COND;
  int const value = 1;
  act_def.frm_4.matching_cond_lst[0].test_info_lst = filter_predicate(type, condition, value);

  act_def.frm_4.action_def_format_1 = fill_act_def_frm_1(report_item);
  return act_def;
}

typedef kpm_act_def_t (*fill_kpm_act_def_fn)(ric_report_style_item_t const*);

static fill_kpm_act_def_fn get_fill_fn(format_action_def_e const fmt)
{
  if (fmt == FORMAT_1_ACTION_DEFINITION)
    return fill_report_style_1;
  if (fmt == FORMAT_4_ACTION_DEFINITION)
    return fill_report_style_4;
  return NULL;
}

static kpm_sub_data_t gen_kpm_subs(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);

  kpm_sub_data_t kpm_sub = {0};

  // Event trigger: periodic report every period_ms
  kpm_sub.ev_trg_def.type = FORMAT_1_RIC_EVENT_TRIGGER;
  kpm_sub.ev_trg_def.kpm_ric_event_trigger_format_1.report_period_ms = period_ms;

  kpm_sub.sz_ad = 1;
  kpm_sub.ad = calloc(kpm_sub.sz_ad, sizeof(kpm_act_def_t));
  assert(kpm_sub.ad != NULL && "Memory exhausted");

  fill_kpm_act_def_fn const fill = get_fill_fn(report_item->act_def_format_type);
  assert(fill != NULL && "Unsupported KPM report style");
  *kpm_sub.ad = fill(report_item);

  return kpm_sub;
}

// ---------- helpers ----------------------------------------------------------

static bool eq_sm(sm_ran_function_t const* elem, int const id)
{
  return elem->id == id;
}
static size_t find_sm_idx(sm_ran_function_t* rf, size_t sz,
                          bool (*f)(sm_ran_function_t const*, int const), int const id,
                          bool* found)
{
  *found = false;
  for (size_t i = 0; i < sz; ++i) {
    if (f(&rf[i], id)) {
      *found = true;
      return i;
    }
  }
  return 0;
}

// ---------- main -------------------------------------------------------------

int main(int argc, char* argv[])
{
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  // Init the xApp: reads /usr/local/etc/flexric/flexric.conf by default
  fr_args_t args = init_fr_args(argc, argv);
  init_xapp_api(&args);
  sleep(1);

  e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();

  if (nodes.len == 0) {
    fprintf(stderr,
            "kpm-exporter: no E2 nodes connected. Is the near-RT RIC up "
            "(./up.sh ric)?\n");
    free_e2_node_arr_xapp(&nodes);
    while (try_stop_xapp_api() == false)
      usleep(1000);
    exit(1);
  }

  printf("Connected E2 nodes = %u\n", (unsigned)nodes.len);
  for (size_t i = 0; i < nodes.len; ++i) {
    printf("E2 node %zu: nb_id %d, mcc %d, mnc %d, ran_type %s\n",
           i,
           nodes.n[i].id.nb_id.nb_id,
           nodes.n[i].id.plmn.mcc,
           nodes.n[i].id.plmn.mnc,
           get_ngran_name(nodes.n[i].id.type));
  }
  g_tbl.nb_id = (unsigned long)nodes.n[0].id.nb_id.nb_id;

  int const KPM_RAN_FUNC_ID = 2;

  // Unsubscribe everything and tear down the xApp API. hndl entries that
  // were never attempted are zeroed by calloc (success == false), so the
  // full-range loop is safe on every error path.
  #define TEARDOWN_AND_EXIT() \
    do { \
      for (size_t i_ = 0; i_ < nodes.len; ++i_) \
        for (size_t st_ = 0; st_ < END_RIC_SERVICE_REPORT; ++st_) \
          if (hndl[i_][st_].success == true) \
            rm_report_sm_xapp_api(hndl[i_][st_].u.handle); \
      free(hndl); \
      free_e2_node_arr_xapp(&nodes); \
      while (try_stop_xapp_api() == false) \
        usleep(1000); \
      exit(1); \
    } while (0)

  sm_ans_xapp_t (*hndl)[END_RIC_SERVICE_REPORT] = calloc(nodes.len, sizeof(*hndl));
  assert(hndl != NULL && "Memory exhausted");

  for (size_t i = 0; i < nodes.len; ++i) {
    e2_node_connected_xapp_t* n = &nodes.n[i];
    bool found = false;
    size_t const idx = find_sm_idx(n->rf, n->len_rf, eq_sm, KPM_RAN_FUNC_ID, &found);
    if (!found) {
      fprintf(stderr, "kpm-exporter: E2 node %zu does not expose the KPM RAN "
                      "function (id %d)\n", i, KPM_RAN_FUNC_ID);
      exit(1);
    }
    assert(n->rf[idx].defn.type == KPM_RAN_FUNC_DEF_E &&
           "KPM is not the received RAN Function");

    for (size_t j = 0; j < n->rf[idx].defn.kpm.sz_ric_report_style_list; ++j) {
      ric_report_style_item_t* report_item = &n->rf[idx].defn.kpm.ric_report_style_list[j];
      if (report_item->report_style_type >= END_RIC_SERVICE_REPORT ||
          get_fill_fn(report_item->act_def_format_type) == NULL)
        continue;

      kpm_sub_data_t kpm_sub = gen_kpm_subs(report_item);
      hndl[i][report_item->report_style_type] =
          report_sm_xapp_api(&n->id, KPM_RAN_FUNC_ID, &kpm_sub, sm_cb_kpm);
      free_kpm_sub_data(&kpm_sub);
      if (hndl[i][report_item->report_style_type].success != true) {
        fprintf(stderr, "kpm-exporter: KPM subscription rejected by node %zu\n", i);
        TEARDOWN_AND_EXIT();
      }
    }
  }
  printf("kpm-exporter: subscribed to E2SM-KPM, serving :%d/metrics\n",
         EXPORTER_PORT);

  g_srv_fd = open_server_socket(EXPORTER_PORT);
  if (g_srv_fd < 0) {
    fprintf(stderr, "kpm-exporter: cannot open TCP port %d\n", EXPORTER_PORT);
    TEARDOWN_AND_EXIT();
  }

  pthread_t http_tid;
  int rc = pthread_create(&http_tid, NULL, http_thread, NULL);
  if (rc != 0) {
    fprintf(stderr, "kpm-exporter: pthread_create failed (%d)\n", rc);
    close(g_srv_fd);
    TEARDOWN_AND_EXIT();
  }

  while (!g_stop)
    sleep(1);

  pthread_join(http_tid, NULL);
  close(g_srv_fd);

  // Unsubscribe and tear down
  for (size_t i = 0; i < nodes.len; ++i) {
    for (size_t st = 0; st < END_RIC_SERVICE_REPORT; ++st) {
      if (hndl[i][st].success == true)
        rm_report_sm_xapp_api(hndl[i][st].u.handle);
    }
  }
  free(hndl);
  free_e2_node_arr_xapp(&nodes);

  while (try_stop_xapp_api() == false)
    usleep(1000);

  printf("kpm-exporter stopped\n");
  return 0;
}
