/*
 * nr-sniffer: following other UEs through random access (milestone 2).
 *
 * Strong definitions of the RA hooks in common/utils/nr_sniffer_hooks.h. Only one UE is followed
 * (the first whose Msg4 is decoded); multi-UE tracking is milestone 3.
 */

#include "nr_sniffer_ra.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_prach_config.h"
#include "NR_CellGroupConfig.h"
#include "NR_RRCReconfiguration.h"
#include "NR_SIB1.h"
#include "NR_ServingCellConfigCommonSIB.h"
#include "nr_sniffer_csv.h"

#define MAX_RA_RNTI 128
#define MAX_TC_RNTI 16
/// a TC-RNTI from a RAR is dropped if no Msg4 comes within this time (ra-ContentionResolutionTimer is at most 64 ms)
#define TC_RNTI_LIFETIME_US 1000000

typedef struct {
  uint16_t rnti;
  uint64_t seen_us;
} tc_rnti_t;

static struct {
  pthread_mutex_t lock;
  bool follow;
  int nb_ra;
  uint16_t ra[MAX_RA_RNTI];
  int nb_tc;
  tc_rnti_t tc[MAX_TC_RNTI];
  uint16_t adopted; ///< C-RNTI of the UE being followed, 0 until its Msg4 is decoded
  uint16_t last_tc_dl; ///< TC-RNTI of the last DL DCI on a TC-RNTI (the Msg4 the next contention resolution belongs to)
  nr_sniffer_csv_t rar_csv;
  nr_sniffer_csv_t ue_csv;
  NR_CellGroupConfig_t *known_cg; ///< configuration of a UE to follow from the start (handed to the MAC once)
  uint16_t known_rnti;
} R = {.lock = PTHREAD_MUTEX_INITIALIZER};

void nr_sniffer_ra_init(const char *out_dir, bool follow)
{
  R.follow = follow;
  nr_sniffer_csv_open(&R.rar_csv, out_dir, "rar.csv", "timestamp_us,sfn,slot,rapid,timing_advance,ul_grant,tc_rnti");
  nr_sniffer_csv_open(&R.ue_csv, out_dir, "ue.csv", "timestamp_us,sfn,slot,c_rnti,contention_resolution_id");
}

static bool in_list(const uint16_t *list, int n, uint16_t rnti)
{
  for (int i = 0; i < n; i++)
    if (list[i] == rnti)
      return true;
  return false;
}

static int find_tc(uint16_t rnti)
{
  for (int i = 0; i < R.nb_tc; i++)
    if (R.tc[i].rnti == rnti)
      return i;
  return -1;
}

static void remove_tc(int i)
{
  R.tc[i] = R.tc[--R.nb_tc];
}

/* RA-RNTI = 1 + s_id + 14 t_id + 14 * 80 * f_id (TS 38.321 5.1.3) for every PRACH occasion of the
 * configuration, following configure_prach_occasions() of the OAI UE (but without the TDD UL-slot
 * filter: an RA-RNTI of an occasion that cannot be used simply never shows up). */
void nr_sniffer_ra_configure(const void *sib1_ptr, bool fr2)
{
  const NR_SIB1_t *sib1 = sib1_ptr;
  const NR_ServingCellConfigCommonSIB_t *scc = sib1->servingCellConfigCommon;
  if (!R.follow || !scc || !scc->uplinkConfigCommon)
    return;
  const NR_BWP_UplinkCommon_t *ul_bwp = &scc->uplinkConfigCommon->initialUplinkBWP;
  if (!ul_bwp->rach_ConfigCommon || ul_bwp->rach_ConfigCommon->present != NR_SetupRelease_RACH_ConfigCommon_PR_setup)
    return;
  const NR_RACH_ConfigCommon_t *rach = ul_bwp->rach_ConfigCommon->choice.setup;
  const int config_index = rach->rach_ConfigGeneric.prach_ConfigurationIndex;
  const int nb_fdm = 1 << rach->rach_ConfigGeneric.msg1_FDM;
  const int scs = rach->msg1_SubcarrierSpacing ? *rach->msg1_SubcarrierSpacing : ul_bwp->genericParameters.subcarrierSpacing;
  const bool tdd = scc->tdd_UL_DL_ConfigurationCommon != NULL;
  const nr_prach_info_t pi = get_nr_prach_occasion_info_from_index(config_index, fr2 ? FR2 : FR1, tdd);

  pthread_mutex_lock(&R.lock);
  R.nb_ra = 0;
  const int prach_slots_in_sf = (scs == 1 || scs == 3) ? pi.N_RA_slot : 1;
  for (int sf = 0; sf < 64; sf++) {
    if (!((pi.s_map >> sf) & 1))
      continue;
    const int sl = (scs == 1 || scs == 3) ? sf * 2 : sf;
    for (int i = 0; i < prach_slots_in_sf; i++) {
      int add_slot = i;
      if ((scs == 1 || scs == 3) && (pi.format & 0xff) > 3 && prach_slots_in_sf == 1)
        add_slot = 1;
      const int t_id = sl + add_slot;
      for (int t = 0; t < (int)pi.N_t_slot; t++) {
        const int s_id = pi.start_symbol + t * pi.N_dur;
        for (int f = 0; f < nb_fdm; f++) {
          const uint16_t rnti = nr_get_ra_rnti(s_id, t_id, f, 0);
          if (R.nb_ra < MAX_RA_RNTI && !in_list(R.ra, R.nb_ra, rnti))
            R.ra[R.nb_ra++] = rnti;
        }
      }
    }
  }
  printf("[SNIFFER] Watching random access: PRACH config %d, %d RA-RNTIs:", config_index, R.nb_ra);
  for (int i = 0; i < R.nb_ra; i++)
    printf(" %04x", R.ra[i]);
  printf("\n");
  fflush(stdout);
  pthread_mutex_unlock(&R.lock);
}

uint16_t nr_sniffer_ra_watch_rnti(void)
{
  /* read without the lock: a stale value only means one slot more or less of RA monitoring */
  return R.follow && R.nb_ra > 0 ? R.ra[0] : 0;
}

bool nr_sniffer_match_rnti(uint16_t cfg_rnti, uint32_t crc)
{
  /* a valid DCI leaves only the RNTI in the 24-bit CRC remainder */
  if (crc > 0xffff || !R.follow)
    return false;
  pthread_mutex_lock(&R.lock);
  const bool ok = R.nb_ra > 0 && cfg_rnti == R.ra[0] && (in_list(R.ra, R.nb_ra, crc) || find_tc(crc) >= 0);
  pthread_mutex_unlock(&R.lock);
  return ok;
}

int nr_sniffer_rnti_type(uint16_t rnti)
{
  if (!R.follow)
    return -1;
  pthread_mutex_lock(&R.lock);
  int type = -1;
  if (find_tc(rnti) >= 0)
    type = TYPE_TC_RNTI_;
  else if (in_list(R.ra, R.nb_ra, rnti))
    type = TYPE_RA_RNTI_;
  pthread_mutex_unlock(&R.lock);
  return type;
}

void nr_sniffer_on_rar(const uint8_t *pdu, int len, int frame, int slot)
{
  /* TS 38.321 6.1.5 / 6.2.3: subPDUs of [E|T|BI] (1 byte) or [E|T|RAPID] + 7-byte MAC RAR */
  const uint64_t now = nr_sniffer_now_us();
  pthread_mutex_lock(&R.lock);
  for (int i = 0; i < R.nb_tc;)
    if (now - R.tc[i].seen_us > TC_RNTI_LIFETIME_US)
      remove_tc(i);
    else
      i++;
  int pos = 0;
  while (pos < len) {
    const uint8_t h = pdu[pos];
    const bool more = h & 0x80;
    if (!(h & 0x40)) { /* backoff indicator subheader */
      pos++;
    } else {
      if (pos + 8 > len)
        break;
      const uint8_t *rar = &pdu[pos + 1];
      const int rapid = h & 0x3f;
      const int ta = ((rar[0] & 0x7f) << 5) | (rar[1] >> 3);
      const unsigned ul_grant = ((rar[1] & 0x07) << 24) | (rar[2] << 16) | (rar[3] << 8) | rar[4];
      const uint16_t tc_rnti = (rar[5] << 8) | rar[6];
      nr_sniffer_csv_row(&R.rar_csv, "%lu,%d,%d,%d,%d,0x%07x,0x%04x", now, frame, slot, rapid, ta, ul_grant, tc_rnti);
      printf("[SNIFFER] RAR at %d.%d: RAPID %d, TA %d, TC-RNTI %04x\n", frame, slot, rapid, ta, tc_rnti);
      if (!R.adopted && find_tc(tc_rnti) < 0) {
        if (R.nb_tc == MAX_TC_RNTI)
          remove_tc(0);
        R.tc[R.nb_tc++] = (tc_rnti_t){.rnti = tc_rnti, .seen_us = now};
      }
      pos += 8;
    }
    if (!more)
      break;
  }
  fflush(stdout);
  pthread_mutex_unlock(&R.lock);
}

uint16_t nr_sniffer_on_contention_resolution(const uint8_t *cont_res_id, int frame, int slot)
{
  pthread_mutex_lock(&R.lock);
  uint16_t adopted = 0;
  const int i = find_tc(R.last_tc_dl);
  if (R.follow && !R.adopted && i >= 0) {
    adopted = R.adopted = R.last_tc_dl;
    remove_tc(i); /* from now on it is this MAC's C-RNTI */
    nr_sniffer_csv_row(&R.ue_csv, "%lu,%d,%d,0x%04x,0x%02x%02x%02x%02x%02x%02x", nr_sniffer_now_us(), frame, slot,
                       adopted, cont_res_id[0], cont_res_id[1], cont_res_id[2], cont_res_id[3], cont_res_id[4],
                       cont_res_id[5]);
    printf("[SNIFFER] Msg4 at %d.%d: following UE with C-RNTI %04x\n", frame, slot, adopted);
    fflush(stdout);
  }
  pthread_mutex_unlock(&R.lock);
  return adopted;
}

void nr_sniffer_ra_on_dci(const nr_sniffer_dci_t *dci)
{
  if (dci->rnti_type == TYPE_TC_RNTI_ && (dci->dci_format == NR_DL_DCI_FORMAT_1_0)) {
    pthread_mutex_lock(&R.lock);
    R.last_tc_dl = dci->rnti;
    pthread_mutex_unlock(&R.lock);
  }
}

/* ------------------------------------------------------------------------- */
/* known configuration (NR-Scope fixed_rnti)                                 */
/* ------------------------------------------------------------------------- */

static NR_CellGroupConfig_t *decode_cellgroup(const uint8_t *buf, int len)
{
  NR_CellGroupConfig_t *cg = NULL;
  asn_dec_rval_t r = uper_decode_complete(NULL, &asn_DEF_NR_CellGroupConfig, (void **)&cg, buf, len);
  if (r.code == RC_OK && r.consumed == len)
    return cg;
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);
  return NULL;
}

/* the CellGroupConfig carried by an RRCReconfiguration: masterCellGroup (SA) or secondaryCellGroup (NSA, phy-test) */
static NR_CellGroupConfig_t *cellgroup_from_reconfiguration(const uint8_t *buf, int len)
{
  NR_RRCReconfiguration_t *rec = NULL;
  NR_CellGroupConfig_t *cg = NULL;
  asn_dec_rval_t r = uper_decode_complete(NULL, &asn_DEF_NR_RRCReconfiguration, (void **)&rec, buf, len);
  if (r.code == RC_OK && r.consumed == len && rec->criticalExtensions.present == NR_RRCReconfiguration__criticalExtensions_PR_rrcReconfiguration) {
    const NR_RRCReconfiguration_IEs_t *ies = rec->criticalExtensions.choice.rrcReconfiguration;
    const OCTET_STRING_t *os = ies->secondaryCellGroup;
    if (!os && ies->nonCriticalExtension)
      os = ies->nonCriticalExtension->masterCellGroup;
    if (os)
      cg = decode_cellgroup(os->buf, os->size);
  }
  ASN_STRUCT_FREE(asn_DEF_NR_RRCReconfiguration, rec);
  return cg;
}

bool nr_sniffer_ra_load_known_ue(const char *path, int rnti)
{
  FILE *f = fopen(path, "r");
  if (!f) {
    fprintf(stderr, "[SNIFFER] cannot open cellgroup_file %s\n", path);
    return false;
  }
  uint8_t buf[16384];
  const int len = fread(buf, 1, sizeof(buf), f);
  fclose(f);
  NR_CellGroupConfig_t *cg = decode_cellgroup(buf, len);
  const char *kind = "CellGroupConfig";
  if (!cg) {
    cg = cellgroup_from_reconfiguration(buf, len);
    kind = "RRCReconfiguration";
  }
  if (!cg) {
    fprintf(stderr, "[SNIFFER] %s is neither a UPER CellGroupConfig nor an RRCReconfiguration\n", path);
    return false;
  }
  /* reconfigurationWithSync would make the MAC re-synchronise and do random access; in SA the cell
   * configuration comes from SIB1, so only its newUE-Identity is kept */
  if (cg->spCellConfig && cg->spCellConfig->reconfigurationWithSync) {
    if (rnti < 0)
      rnti = cg->spCellConfig->reconfigurationWithSync->newUE_Identity;
    ASN_STRUCT_FREE(asn_DEF_NR_ReconfigurationWithSync, cg->spCellConfig->reconfigurationWithSync);
    cg->spCellConfig->reconfigurationWithSync = NULL;
  }
  if (rnti <= 0 || rnti > 0xffef) {
    fprintf(stderr, "[SNIFFER] cellgroup_file %s: give the UE's C-RNTI with --sniffer.rnti\n", path);
    ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);
    return false;
  }
  pthread_mutex_lock(&R.lock);
  R.known_cg = cg;
  R.known_rnti = rnti;
  R.adopted = rnti; /* do not adopt another UE through random access */
  pthread_mutex_unlock(&R.lock);
  printf("[SNIFFER] Known configuration: %s from %s (%d bytes) for C-RNTI %04x\n", kind, path, len, rnti);
  fflush(stdout);
  return true;
}

void *nr_sniffer_known_ue(uint16_t *rnti)
{
  pthread_mutex_lock(&R.lock);
  NR_CellGroupConfig_t *cg = R.known_cg;
  R.known_cg = NULL;
  *rnti = R.known_rnti;
  if (cg) {
    nr_sniffer_csv_row(&R.ue_csv, "%lu,-1,-1,0x%04x,known", nr_sniffer_now_us(), R.known_rnti);
    printf("[SNIFFER] SIB1 applied: following UE with C-RNTI %04x (known configuration)\n", R.known_rnti);
    fflush(stdout);
  }
  pthread_mutex_unlock(&R.lock);
  return cg;
}
