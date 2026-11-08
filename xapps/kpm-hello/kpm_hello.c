/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * kpm-hello: a minimal FlexRIC xApp sample.
 *
 * It subscribes to the E2SM-KPM service of the gNB through the near-RT RIC
 * and prints what comes back: cell-level measurements plus one report per
 * UE. Pure observation - the xApp never sends anything towards the RAN.
 *
 * Structure: connect (like examples/xApp/c/helloworld) plus one simplified
 * KPM subscription (from examples/xApp/c/monitor/xapp_kpm_moni) - no unit
 * lookup table, no distribution-bin labels, one line per metric.
 */

#include "xApp/e42_xapp_api.h"
#include "util/byte_array.h"
#include "util/e.h"
#include "util/ngran_types.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint64_t const period_ms = 1000;  // one report per second

static pthread_mutex_t print_mtx;
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
  (void)sig;
  g_stop = 1;
}

// ---------- subscription generation (simplified from xapp_kpm_moni) ----------

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

  // Action definition. FlexRIC limitation: one action definition per
  // subscription message, so one subscription per report style.
  kpm_sub.sz_ad = 1;
  kpm_sub.ad = calloc(kpm_sub.sz_ad, sizeof(kpm_act_def_t));
  assert(kpm_sub.ad != NULL && "Memory exhausted");

  fill_kpm_act_def_fn const fill = get_fill_fn(report_item->act_def_format_type);
  assert(fill != NULL && "Unsupported KPM report style");
  *kpm_sub.ad = fill(report_item);

  return kpm_sub;
}

// ---------- printing ---------------------------------------------------------

static void print_meas_value(meas_record_lst_t const* rec)
{
  if (rec->value == REAL_MEAS_VALUE) {
    printf("%.2f", rec->real_val);
  } else if (rec->value == INTEGER_MEAS_VALUE) {
    printf("%d", rec->int_val);
  } else {
    printf("no value");
  }
}

// Print one measurement report: cell-level Format 1, or the per-UE half of
// a Format 3 indication.
static void print_meas_report(kpm_ind_msg_format_1_t const* f1)
{
  for (size_t j = 0; j < f1->meas_data_lst_len; ++j) {
    meas_data_lst_t const* data = &f1->meas_data_lst[j];
    for (size_t i = 0; i < f1->meas_info_lst_len; ++i) {
      meas_info_format_1_lst_t const* info = &f1->meas_info_lst[i];
      if (info->meas_type.type != NAME_MEAS_TYPE)
        continue;
      char* name = cp_ba_to_str(info->meas_type.name);
      // Records follow the label list; unlabelled metrics have one label.
      for (size_t z = 0; z < info->label_info_lst_len; ++z) {
        if (info->label_info_lst[z].noLabel == NULL)
          continue;
        if (i + z >= data->meas_record_len)
          break; // short report from the E2 node - skip rather than over-read
        printf("  %s = ", name);
        print_meas_value(&data->meas_record_lst[i + z]);
        printf("\n");
      }
      free(name);
    }
  }
}

static void print_ue_id(ue_id_e2sm_t const* ue)
{
  if (ue->type == GNB_UE_ID_E2SM) {
    if (ue->gnb.ran_ue_id != NULL)
      printf("ran_ue_id=%lu", (unsigned long)*ue->gnb.ran_ue_id);
    else
      printf("amf_ue_ngap_id=%lu", (unsigned long)ue->gnb.amf_ue_ngap_id);
  } else if (ue->type == GNB_DU_UE_ID_E2SM) {
    printf("gnb_cu_ue_f1ap=%u", ue->gnb_du.gnb_cu_ue_f1ap);
  } else if (ue->type == GNB_CU_UP_UE_ID_E2SM) {
    printf("gnb_cu_cp_ue_e1ap=%u", ue->gnb_cu_up.gnb_cu_cp_ue_e1ap);
  } else {
    printf("unknown UE id type %d", ue->type);
  }
}

// ---------- E2 indication callback -------------------------------------------

static void sm_cb_kpm(sm_ag_if_rd_t const* rd)
{
  assert(rd != NULL);
  assert(rd->type == INDICATION_MSG_AGENT_IF_ANS_V0);
  assert(rd->ind.type == KPM_STATS_V3_0);

  kpm_ind_data_t const* ind = &rd->ind.kpm.ind;

  pthread_mutex_lock(&print_mtx);
  // Note: the KPM indication header's collectStartTime is a 3GPP 9.3.12
  // timestamp (seconds since 1900-01-01) - a different clock than the
  // xApp's, so we show only report cadence (periodic, 1 s) and payload.
  printf("\n[kpm-hello] report\n");

  if (ind->msg.type == FORMAT_3_INDICATION_MESSAGE) {
    kpm_ind_msg_format_3_t const* f3 = &ind->msg.frm_3;
    for (size_t i = 0; i < f3->ue_meas_report_lst_len; ++i) {
      printf("[kpm-hello] UE ");
      print_ue_id(&f3->meas_report_per_ue[i].ue_meas_report_lst);
      printf("\n");
      print_meas_report(&f3->meas_report_per_ue[i].ind_msg_format_1);
    }
  } else if (ind->msg.type == FORMAT_1_INDICATION_MESSAGE) {
    printf("[kpm-hello] cell-level\n");
    print_meas_report(&ind->msg.frm_1);
  } else {
    printf("[kpm-hello] indication format %d not handled\n", ind->msg.type);
  }
  pthread_mutex_unlock(&print_mtx);
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
  pthread_mutex_init(&print_mtx, NULL);

  // Init the xApp: reads /usr/local/etc/flexric/flexric.conf by default
  fr_args_t args = init_fr_args(argc, argv);
  init_xapp_api(&args);
  sleep(1);

  e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();

  if (nodes.len == 0) {
    fprintf(stderr,
            "kpm-hello: no E2 nodes connected. Is the near-RT RIC up "
            "(./up.sh ric)?\n");
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

  // Subscribe to KPM (ran function id 2) on every node, for each report
  // style the node offers and this sample understands (cell-level, per-UE).
  int const KPM_RAN_FUNC_ID = 2;

  sm_ans_xapp_t (*hndl)[END_RIC_SERVICE_REPORT] = calloc(nodes.len, sizeof(*hndl));
  assert(hndl != NULL && "Memory exhausted");

  for (size_t i = 0; i < nodes.len; ++i) {
    e2_node_connected_xapp_t* n = &nodes.n[i];
    bool found = false;
    size_t const idx = find_sm_idx(n->rf, n->len_rf, eq_sm, KPM_RAN_FUNC_ID, &found);
    if (!found) {
      fprintf(stderr, "kpm-hello: E2 node %zu does not expose the KPM RAN "
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
        fprintf(stderr, "kpm-hello: KPM subscription rejected by node %zu\n", i);
        // leave nothing active behind, then exit like the success path does
        for (size_t ni = 0; ni <= i; ++ni) {
          for (size_t st = 0; st < END_RIC_SERVICE_REPORT; ++st) {
            if (hndl[ni][st].success == true)
              rm_report_sm_xapp_api(hndl[ni][st].u.handle);
          }
        }
        free(hndl);
        free_e2_node_arr_xapp(&nodes);
        while (try_stop_xapp_api() == false)
          usleep(1000);
        exit(1);
      }
    }
  }
  printf("kpm-hello: subscribed to E2SM-KPM, reports every %lu ms (Ctrl+C to "
         "stop)\n",
         (unsigned long)period_ms);

  while (!g_stop)
    sleep(1);

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

  // Only now is every in-flight indication handled: safe to destroy the
  // print mutex.
  pthread_mutex_destroy(&print_mtx);

  printf("kpm-hello stopped\n");
  return 0;
}
