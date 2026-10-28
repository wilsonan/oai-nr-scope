/*
 * nr-sniffer: passive 5G SA telemetry on top of the OAI nrUE receive chain.
 *
 * This file provides the strong definitions of the weak hooks declared in
 * OAI's common/utils/nr_sniffer_hooks.h (added by oai-patches/). The rest of
 * the receive chain (cell search, PBCH, PDCCH, PDSCH, ASN.1) is OAI's own.
 *
 * Milestone 1 scope: cell search, MIB and SIB1 decoding, logged to CSV.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "common/config/config_userapi.h"
#include "common/utils/nr_sniffer_hooks.h"
#include "common/utils/oai_asn1.h"
#include "NR_SIB1.h"
#include "NR_ServingCellConfigCommonSIB.h"
#include "nr_sniffer_csv.h"

#define SNIFFER_SECTION "sniffer"

static struct {
  bool initialized;
  pthread_mutex_t lock;
  /* configuration */
  char *out_dir;
  int passive;
  int pci_filter;
  char *mcs_table;
  int dump_xml;
  /* outputs */
  nr_sniffer_csv_t mib_csv;
  nr_sniffer_csv_t sib1_csv;
  /* state */
  int last_pci;
  char last_mib_key[256];
  char last_sib1_key[1024];
  uint64_t nb_mib;
  uint64_t nb_sib1;
} S = {.lock = PTHREAD_MUTEX_INITIALIZER, .last_pci = -1};

static const char *MIB_HEADER =
    "timestamp_us,sfn,ssb_index,half_frame,pci,scs_common_khz,ssb_subcarrier_offset,ssb_start_subcarrier,"
    "dmrs_typeA_pos,coreset0,searchspace0,cell_barred,intra_freq_reselection,fr2,nb_mib_decoded";

static const char *SIB1_HEADER =
    "timestamp_us,sfn,slot,pci,mcc,mnc,tac,nr_cell_id,cell_reserved_for_operator,band,"
    "offset_to_point_a,scs_khz,carrier_bw_prb,offset_to_carrier,initial_dl_bwp_loc_bw,initial_dl_bwp_start_prb,"
    "initial_dl_bwp_size_prb,initial_dl_bwp_scs_khz,searchspace_sib1,ra_searchspace,paging_searchspace,"
    "nb_common_searchspaces,prach_config_index,msg1_fdm,msg1_freq_start,ra_response_window,"
    "ssb_positions_in_burst,ssb_periodicity_ms,ss_pbch_block_power_dbm,tdd_ref_scs_khz,tdd_period,"
    "tdd_dl_slots,tdd_dl_symbols,tdd_ul_slots,tdd_ul_symbols,si_scheduling_present,nb_sib1_decoded";

/* ------------------------------------------------------------------------- */
/* configuration                                                             */
/* ------------------------------------------------------------------------- */

static int mkdir_p(const char *path)
{
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
      *p = '/';
    }
  }
  if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

void nr_sniffer_init(configmodule_interface_t *cfg)
{
  pthread_mutex_lock(&S.lock);
  if (S.initialized) {
    pthread_mutex_unlock(&S.lock);
    return;
  }

  paramdef_t params[] = {
      STRINGPARAM("out_dir", "directory for the sniffer CSV logs", 0, &S.out_dir, "./sniffer-logs"),
      INTPARAM("passive", "1: never start random access or transmit as a UE (default), 0: behave as a normal UE",
               PARAMFLAG_BOOL, &S.passive, 1),
      INTPARAM("pci", "only log cells with this PCI (-1: any cell)", 0, &S.pci_filter, -1),
      STRINGPARAM("mcs_table", "MCS table assumed for DCI decoding: qam256 or qam64 (used from milestone 2)", 0,
                  &S.mcs_table, "qam256"),
      INTPARAM("dump_xml", "1: write each new SIB1 as ASN.1 XER to out_dir", PARAMFLAG_BOOL, &S.dump_xml, 1),
  };
  config_get(cfg, params, sizeof(params) / sizeof(params[0]), SNIFFER_SECTION);

  if (mkdir_p(S.out_dir) != 0)
    fprintf(stderr, "[SNIFFER] cannot create output directory %s: %s\n", S.out_dir, strerror(errno));
  nr_sniffer_csv_open(&S.mib_csv, S.out_dir, "mib.csv", MIB_HEADER);
  nr_sniffer_csv_open(&S.sib1_csv, S.out_dir, "sib1.csv", SIB1_HEADER);

  printf("[SNIFFER] nr-sniffer started: out_dir=%s passive=%d pci=%d mcs_table=%s\n",
         S.out_dir, S.passive, S.pci_filter, S.mcs_table);
  S.initialized = true;
  pthread_mutex_unlock(&S.lock);
}

bool nr_sniffer_is_passive(void)
{
  /* before init the safe answer is "passive" */
  return !S.initialized || S.passive != 0;
}

static bool pci_accepted(int pci)
{
  return S.pci_filter < 0 || S.pci_filter == pci;
}

/* ------------------------------------------------------------------------- */
/* MIB                                                                       */
/* ------------------------------------------------------------------------- */

static int scs_common_khz(int scs_common, bool fr2)
{
  /* subCarrierSpacingCommon: scs15or60 (0), scs30or120 (1) */
  if (fr2)
    return scs_common ? 120 : 60;
  return scs_common ? 30 : 15;
}

void nr_sniffer_on_mib(const nr_sniffer_mib_t *mib)
{
  if (!S.initialized)
    return;
  pthread_mutex_lock(&S.lock);
  S.nb_mib++;
  S.last_pci = mib->pci;

  if (pci_accepted(mib->pci)) {
    /* log the first MIB and every change of its static content */
    char key[sizeof(S.last_mib_key)];
    snprintf(key, sizeof(key), "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
             mib->pci, mib->scs_common, mib->ssb_subcarrier_offset, mib->ssb_start_subcarrier, mib->dmrs_typeA_pos,
             mib->coreset0, mib->searchspace0, mib->cell_barred, mib->intra_freq_reselection, mib->fr2);
    if (strcmp(key, S.last_mib_key) != 0) {
      strcpy(S.last_mib_key, key);
      const int scs = scs_common_khz(mib->scs_common, mib->fr2);
      nr_sniffer_csv_row(&S.mib_csv, "%lu,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lu",
                         nr_sniffer_now_us(), mib->sfn, mib->ssb_index, mib->half_frame, mib->pci, scs,
                         mib->ssb_subcarrier_offset, mib->ssb_start_subcarrier, mib->dmrs_typeA_pos, mib->coreset0,
                         mib->searchspace0, mib->cell_barred, mib->intra_freq_reselection, mib->fr2, S.nb_mib);
      printf("[SNIFFER] Found cell: PCI %d, MIB SFN %d, SSB idx %d, SCS common %d kHz, k_SSB %d, "
             "CORESET0 %d, SearchSpace0 %d, %s\n",
             mib->pci, mib->sfn, mib->ssb_index, scs, mib->ssb_subcarrier_offset, mib->coreset0, mib->searchspace0,
             mib->cell_barred == 0 ? "barred" : "not barred");
    }
  }
  pthread_mutex_unlock(&S.lock);
}

/* ------------------------------------------------------------------------- */
/* SIB1                                                                      */
/* ------------------------------------------------------------------------- */

static int scs_khz(long scs)
{
  return 15 << scs;
}

static int digits_to_int(NR_MCC_MNC_Digit_t *const *digits, int count)
{
  int v = 0;
  for (int i = 0; i < count; i++)
    v = v * 10 + (int)*digits[i];
  return v;
}

/* locationAndBandwidth (RIV) -> start PRB / number of PRBs, TS 38.214 5.1.2.2.2 with N = 275 */
static void riv_decode(long riv, int *start, int *size)
{
  const int n = 275;
  int l = riv / n + 1;
  int s = riv % n;
  if (l + s > n) {
    l = n - l + 2;
    s = n - 1 - s;
  }
  *start = s;
  *size = l;
}

static const int SSB_PERIODICITY_MS[] = {5, 10, 20, 40, 80, 160};
static const char *TDD_PERIOD[] = {"0.5ms", "0.625ms", "1ms", "1.25ms", "2ms", "2.5ms", "5ms", "10ms"};
static const int MSG1_FDM[] = {1, 2, 4, 8};
static const int RA_RESPONSE_WINDOW_SLOTS[] = {1, 2, 4, 8, 10, 20, 40, 80};

#define OPT_LONG(p) ((p) ? (long)*(p) : -1L)

void nr_sniffer_on_sib1(const void *sib1_ptr, int frame, int slot)
{
  if (!S.initialized || !sib1_ptr)
    return;
  const NR_SIB1_t *sib1 = sib1_ptr;
  pthread_mutex_lock(&S.lock);
  S.nb_sib1++;
  const int pci = S.last_pci;
  if (!pci_accepted(pci)) {
    pthread_mutex_unlock(&S.lock);
    return;
  }

  /* PLMN / cell identity */
  int mcc = -1, mnc = -1;
  long tac = -1, cell_reserved = -1;
  uint64_t nci = 0;
  const struct NR_CellAccessRelatedInfo *cari = &sib1->cellAccessRelatedInfo;
  if (cari->plmn_IdentityInfoList.list.count > 0) {
    const NR_PLMN_IdentityInfo_t *info = cari->plmn_IdentityInfoList.list.array[0];
    if (info->plmn_IdentityList.list.count > 0) {
      const NR_PLMN_Identity_t *plmn = info->plmn_IdentityList.list.array[0];
      if (plmn->mcc)
        mcc = digits_to_int(plmn->mcc->list.array, plmn->mcc->list.count);
      mnc = digits_to_int(plmn->mnc.list.array, plmn->mnc.list.count);
    }
    if (info->trackingAreaCode)
      tac = (long)BIT_STRING_to_uint32(info->trackingAreaCode);
    nci = BIT_STRING_to_uint64(&info->cellIdentity);
    cell_reserved = info->cellReservedForOperatorUse;
  }

  /* serving cell config common */
  long band = -1, offset_to_point_a = -1, carrier_bw = -1, offset_to_carrier = -1, scs = -1;
  long loc_bw = -1, bwp_scs = -1, ss_sib1 = -1, ra_ss = -1, paging_ss = -1, nb_css = 0;
  long prach_idx = -1, msg1_fdm = -1, msg1_fstart = -1, ra_win = -1;
  long ssb_period = -1, ss_pbch_power = 0, tdd_ref_scs = -1, tdd_dl_slots = -1, tdd_dl_sym = -1, tdd_ul_slots = -1,
       tdd_ul_sym = -1;
  const char *tdd_period = "";
  char ssb_pos[16] = "";
  int bwp_start = -1, bwp_size = -1;

  const NR_ServingCellConfigCommonSIB_t *scc = sib1->servingCellConfigCommon;
  if (scc) {
    const NR_FrequencyInfoDL_SIB_t *fdl = &scc->downlinkConfigCommon.frequencyInfoDL;
    if (fdl->frequencyBandList.list.count > 0)
      band = OPT_LONG(fdl->frequencyBandList.list.array[0]->freqBandIndicatorNR);
    offset_to_point_a = fdl->offsetToPointA;
    if (fdl->scs_SpecificCarrierList.list.count > 0) {
      const NR_SCS_SpecificCarrier_t *c = fdl->scs_SpecificCarrierList.list.array[0];
      carrier_bw = c->carrierBandwidth;
      offset_to_carrier = c->offsetToCarrier;
      scs = scs_khz(c->subcarrierSpacing);
    }
    const NR_BWP_DownlinkCommon_t *dl_bwp = &scc->downlinkConfigCommon.initialDownlinkBWP;
    loc_bw = dl_bwp->genericParameters.locationAndBandwidth;
    riv_decode(loc_bw, &bwp_start, &bwp_size);
    bwp_scs = scs_khz(dl_bwp->genericParameters.subcarrierSpacing);
    if (dl_bwp->pdcch_ConfigCommon && dl_bwp->pdcch_ConfigCommon->present == NR_SetupRelease_PDCCH_ConfigCommon_PR_setup) {
      const NR_PDCCH_ConfigCommon_t *pdcch = dl_bwp->pdcch_ConfigCommon->choice.setup;
      ss_sib1 = OPT_LONG(pdcch->searchSpaceSIB1);
      ra_ss = OPT_LONG(pdcch->ra_SearchSpace);
      paging_ss = OPT_LONG(pdcch->pagingSearchSpace);
      nb_css = pdcch->commonSearchSpaceList ? pdcch->commonSearchSpaceList->list.count : 0;
    }
    if (scc->uplinkConfigCommon) {
      const NR_BWP_UplinkCommon_t *ul_bwp = &scc->uplinkConfigCommon->initialUplinkBWP;
      if (ul_bwp->rach_ConfigCommon && ul_bwp->rach_ConfigCommon->present == NR_SetupRelease_RACH_ConfigCommon_PR_setup) {
        const NR_RACH_ConfigGeneric_t *g = &ul_bwp->rach_ConfigCommon->choice.setup->rach_ConfigGeneric;
        prach_idx = g->prach_ConfigurationIndex;
        msg1_fdm = MSG1_FDM[g->msg1_FDM & 3];
        msg1_fstart = g->msg1_FrequencyStart;
        ra_win = RA_RESPONSE_WINDOW_SLOTS[g->ra_ResponseWindow & 7];
      }
    }
    if (scc->ssb_PositionsInBurst.inOneGroup.size > 0)
      snprintf(ssb_pos, sizeof(ssb_pos), "0x%02x", scc->ssb_PositionsInBurst.inOneGroup.buf[0]);
    if (scc->ssb_PeriodicityServingCell >= 0 && scc->ssb_PeriodicityServingCell < 6)
      ssb_period = SSB_PERIODICITY_MS[scc->ssb_PeriodicityServingCell];
    ss_pbch_power = scc->ss_PBCH_BlockPower;
    if (scc->tdd_UL_DL_ConfigurationCommon) {
      const NR_TDD_UL_DL_ConfigCommon_t *tdd = scc->tdd_UL_DL_ConfigurationCommon;
      tdd_ref_scs = scs_khz(tdd->referenceSubcarrierSpacing);
      if (tdd->pattern1.dl_UL_TransmissionPeriodicity >= 0 && tdd->pattern1.dl_UL_TransmissionPeriodicity < 8)
        tdd_period = TDD_PERIOD[tdd->pattern1.dl_UL_TransmissionPeriodicity];
      tdd_dl_slots = tdd->pattern1.nrofDownlinkSlots;
      tdd_dl_sym = tdd->pattern1.nrofDownlinkSymbols;
      tdd_ul_slots = tdd->pattern1.nrofUplinkSlots;
      tdd_ul_sym = tdd->pattern1.nrofUplinkSymbols;
    }
  }

  char row[1024];
  snprintf(row, sizeof(row),
           "%d,%d,%d,%ld,0x%09lx,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%d,%d,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,"
           "%s,%ld,%ld,%ld,%s,%ld,%ld,%ld,%ld,%d",
           pci, mcc, mnc, tac, (unsigned long)nci, cell_reserved, band,
           offset_to_point_a, scs, carrier_bw, offset_to_carrier, loc_bw, bwp_start, bwp_size, bwp_scs, ss_sib1, ra_ss,
           paging_ss, nb_css, prach_idx, msg1_fdm, msg1_fstart, ra_win, ssb_pos, ssb_period, ss_pbch_power,
           tdd_ref_scs, tdd_period, tdd_dl_slots, tdd_dl_sym, tdd_ul_slots, tdd_ul_sym, sib1->si_SchedulingInfo != NULL);

  if (strcmp(row, S.last_sib1_key) != 0) {
    strncpy(S.last_sib1_key, row, sizeof(S.last_sib1_key) - 1);
    /* row starts with pci; insert timestamp, sfn and slot in front of it */
    nr_sniffer_csv_row(&S.sib1_csv, "%lu,%d,%d,%s,%lu", nr_sniffer_now_us(), frame, slot, row, S.nb_sib1);
    printf("[SNIFFER] SIB1 decoded: PCI %d, PLMN %03d/%02d, TAC %ld, NCI 0x%09lx, band n%ld, %ld PRB @ %ld kHz, "
           "initial BWP %d+%d PRB, PRACH idx %ld, TDD %s %ldD/%ldU\n",
           pci, mcc, mnc, tac, (unsigned long)nci, band, carrier_bw, scs, bwp_start, bwp_size, prach_idx, tdd_period,
           tdd_dl_slots, tdd_ul_slots);
    if (S.dump_xml) {
      char path[600];
      snprintf(path, sizeof(path), "%s/sib1_pci%d.xml", S.out_dir, pci);
      FILE *fp = fopen(path, "w");
      if (fp) {
        xer_fprint(fp, &asn_DEF_NR_SIB1, (void *)sib1);
        fclose(fp);
      }
    }
  }
  pthread_mutex_unlock(&S.lock);
}
