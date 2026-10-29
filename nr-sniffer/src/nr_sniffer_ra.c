/*
 * nr-sniffer: following other UEs through random access, or with a known configuration.
 *
 * Strong definitions of the RA and UE-following hooks in common/utils/nr_sniffer_hooks.h.
 * RA-RNTI (from the SIB1 PRACH configuration) -> RAR -> TC-RNTI -> Msg4 -> C-RNTI. Each followed UE
 * gets its own shadow MAC instance in OAI's UE MAC (instance 1, 2, ...), configured with the cell
 * configuration plus that UE's CellGroupConfig, so that OAI's own DCI decoding follows it.
 */

#include "nr_sniffer_ra.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_prach_config.h"
#include "NR_CellGroupConfig.h"
#include "NR_DL-CCCH-Message.h"
#include "NR_RRCReconfiguration.h"
#include "NR_RRCSetup.h"
#include "NR_RRCSetup-IEs.h"
#include "NR_SIB1.h"
#include "NR_ServingCellConfigCommonSIB.h"
#include "nr_sniffer_csv.h"

#define MAX_RA_RNTI 128
#define MAX_TC_RNTI 16
#define MAX_KNOWN_UE 4
/// a TC-RNTI from a RAR is dropped if no Msg4 comes within this time (ra-ContentionResolutionTimer is at most 64 ms)
#define TC_RNTI_LIFETIME_US 1000000

/* DL-SCH LCIDs (TS 38.321 Table 6.2.1-1) */
#define LCID_CCCH 0
#define LCID_MAX_SDU 32
#define LCID_DRX_LONG 59
#define LCID_DRX 60
#define LCID_TA_COMMAND 61
#define LCID_CON_RES_ID 62
#define LCID_PADDING 63

typedef struct {
  uint16_t rnti;
  uint64_t seen_us;
} tc_rnti_t;

typedef struct {
  uint16_t rnti;
  int instance; ///< shadow MAC instance in OAI's UE MAC
  bool active; ///< false: the UE left (no DCI for ue_timeout); its instance can follow another UE
  uint64_t last_us; ///< last DCI decoded for it (or when following started)
} followed_ue_t;

typedef struct {
  NR_CellGroupConfig_t *cg;
  uint16_t rnti;
} known_ue_t;

static struct {
  pthread_mutex_t lock;
  bool follow;
  int max_ues;
  uint64_t ue_timeout_us; ///< 0: never stop following a UE
  int nb_ra;
  uint16_t ra[MAX_RA_RNTI];
  int nb_tc;
  tc_rnti_t tc[MAX_TC_RNTI];
  int nb_ues;
  followed_ue_t ues[NR_SNIFFER_MAX_UES];
  int nb_known;
  known_ue_t known[MAX_KNOWN_UE]; ///< known configurations waiting for SIB1
  uint16_t last_tc_dl; ///< TC-RNTI of the last DL DCI 1_0 on a TC-RNTI (the Msg4 the next PDU belongs to)
  NR_SIB1_t *sib1; ///< copy of the last SIB1, to configure shadow MAC instances
  nr_sniffer_csv_t rar_csv;
  nr_sniffer_csv_t ue_csv;
} R = {.lock = PTHREAD_MUTEX_INITIALIZER, .max_ues = NR_SNIFFER_MAX_UES};

void nr_sniffer_ra_reset(void)
{
  pthread_mutex_lock(&R.lock);
  for (int i = 0; i < R.nb_known; i++)
    ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, R.known[i].cg);
  R.nb_known = 0;
  R.nb_ra = 0;
  R.nb_tc = 0;
  R.nb_ues = 0;
  R.last_tc_dl = 0;
  ASN_STRUCT_FREE(asn_DEF_NR_SIB1, R.sib1);
  R.sib1 = NULL;
  pthread_mutex_unlock(&R.lock);
}

void nr_sniffer_ra_init(const char *out_dir, bool follow, int max_ues, double ue_timeout_s)
{
  R.follow = follow;
  R.max_ues = max_ues < 0 ? 0 : max_ues > NR_SNIFFER_MAX_UES ? NR_SNIFFER_MAX_UES : max_ues;
  R.ue_timeout_us = ue_timeout_s > 0 ? (uint64_t)(ue_timeout_s * 1e6) : 0;
  nr_sniffer_csv_open(&R.rar_csv, out_dir, "rar.csv", "timestamp_us,sfn,slot,rapid,timing_advance,ul_grant,tc_rnti");
  nr_sniffer_csv_open(&R.ue_csv, out_dir, "ue.csv", "timestamp_us,sfn,slot,c_rnti,event,contention_resolution_id");
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

/* index of the followed (active) UE with this RNTI, or -1 */
static int find_ue(uint16_t rnti)
{
  for (int i = 0; i < R.nb_ues; i++)
    if (R.ues[i].active && R.ues[i].rnti == rnti)
      return i;
  return -1;
}

static int nb_active_ues(void)
{
  int n = 0;
  for (int i = 0; i < R.nb_ues; i++)
    n += R.ues[i].active;
  return n;
}

/* Follow \p rnti in a shadow MAC instance: the one of a UE that left, else a new one. -1 if max_ues UEs are
 * followed already, or this one is. */
static int add_ue(uint16_t rnti)
{
  if (nb_active_ues() >= R.max_ues || find_ue(rnti) >= 0)
    return -1;
  const uint64_t now = nr_sniffer_now_us();
  for (int i = 0; i < R.nb_ues; i++)
    if (!R.ues[i].active) {
      R.ues[i] = (followed_ue_t){.rnti = rnti, .instance = R.ues[i].instance, .active = true, .last_us = now};
      return R.ues[i].instance;
    }
  if (R.nb_ues >= NR_SNIFFER_MAX_UES)
    return -1;
  const int instance = R.nb_ues + 1;
  R.ues[R.nb_ues++] = (followed_ue_t){.rnti = rnti, .instance = instance, .active = true, .last_us = now};
  return instance;
}

/* stop following UEs without a DCI for ue_timeout (they left: RRC release, radio link failure, ...) */
static void expire_ues(void)
{
  if (!R.ue_timeout_us)
    return;
  const uint64_t now = nr_sniffer_now_us();
  for (int i = 0; i < R.nb_ues; i++) {
    followed_ue_t *u = &R.ues[i];
    if (u->active && now - u->last_us > R.ue_timeout_us) {
      u->active = false;
      nr_sniffer_csv_row(&R.ue_csv, "%lu,-1,-1,0x%04x,left,", now, u->rnti);
      printf("[SNIFFER] UE %04x: no DCI for %.1f s, no longer followed (shadow MAC %d is free)\n", u->rnti,
             R.ue_timeout_us / 1e6, u->instance);
      fflush(stdout);
    }
  }
}

/* RA-RNTI = 1 + s_id + 14 t_id + 14 * 80 * f_id (TS 38.321 5.1.3) for every PRACH occasion of the
 * configuration, following configure_prach_occasions() of the OAI UE (but without the TDD UL-slot
 * filter: an RA-RNTI of an occasion that cannot be used simply never shows up). */
void nr_sniffer_ra_configure(const void *sib1_ptr, bool fr2)
{
  const NR_SIB1_t *sib1 = sib1_ptr;
  pthread_mutex_lock(&R.lock);
  ASN_STRUCT_FREE(asn_DEF_NR_SIB1, R.sib1);
  R.sib1 = NULL;
  asn_copy(&asn_DEF_NR_SIB1, (void **)&R.sib1, sib1);
  pthread_mutex_unlock(&R.lock);

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
  printf(" (following up to %d UEs)\n", R.max_ues);
  fflush(stdout);
  pthread_mutex_unlock(&R.lock);
}

const void *nr_sniffer_sib1(void)
{
  /* written once per SIB1 change, read by the MAC when it creates a shadow instance */
  return R.sib1;
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
      if (find_tc(tc_rnti) < 0 && find_ue(tc_rnti) < 0) {
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

static NR_CellGroupConfig_t *decode_cellgroup(const uint8_t *buf, int len)
{
  NR_CellGroupConfig_t *cg = NULL;
  asn_dec_rval_t r = uper_decode_complete(NULL, &asn_DEF_NR_CellGroupConfig, (void **)&cg, buf, len);
  if (r.code == RC_OK && r.consumed == len)
    return cg;
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);
  return NULL;
}

/* the masterCellGroup of an RRCSetup in a DL-CCCH SDU, or NULL */
static NR_CellGroupConfig_t *cellgroup_from_rrcsetup(const uint8_t *buf, int len)
{
  NR_DL_CCCH_Message_t *msg = NULL;
  NR_CellGroupConfig_t *cg = NULL;
  asn_dec_rval_t r = uper_decode(NULL, &asn_DEF_NR_DL_CCCH_Message, (void **)&msg, buf, len, 0, 0);
  if (r.code == RC_OK && msg->message.present == NR_DL_CCCH_MessageType_PR_c1
      && msg->message.choice.c1->present == NR_DL_CCCH_MessageType__c1_PR_rrcSetup) {
    const NR_RRCSetup_t *setup = msg->message.choice.c1->choice.rrcSetup;
    if (setup->criticalExtensions.present == NR_RRCSetup__criticalExtensions_PR_rrcSetup) {
      const OCTET_STRING_t *mcg = &setup->criticalExtensions.choice.rrcSetup->masterCellGroup;
      cg = decode_cellgroup(mcg->buf, mcg->size);
    }
  }
  ASN_STRUCT_FREE(asn_DEF_NR_DL_CCCH_Message, msg);
  return cg;
}

bool nr_sniffer_on_msg4(const uint8_t *pdu, int len, int frame, int slot, uint16_t *rnti, void **cell_group, int *instance)
{
  /* TS 38.321 6.1.2: R/F/LCID[/L] subheaders; fixed-size MAC CEs and padding have no L field */
  const uint8_t *cont_res = NULL, *ccch = NULL;
  int ccch_len = 0;
  int pos = 0;
  while (pos < len) {
    const int lcid = pdu[pos] & 0x3f;
    if (lcid <= LCID_MAX_SDU) {
      const bool f = pdu[pos] & 0x40;
      if (pos + (f ? 3 : 2) > len)
        break;
      const int l = f ? (pdu[pos + 1] << 8) | pdu[pos + 2] : pdu[pos + 1];
      pos += f ? 3 : 2;
      if (pos + l > len)
        break;
      if (lcid == LCID_CCCH) {
        ccch = &pdu[pos];
        ccch_len = l;
      }
      pos += l;
    } else if (lcid == LCID_CON_RES_ID) {
      if (pos + 7 > len)
        break;
      cont_res = &pdu[pos + 1];
      pos += 7;
    } else if (lcid == LCID_TA_COMMAND) {
      pos += 2;
    } else if (lcid == LCID_DRX || lcid == LCID_DRX_LONG) {
      pos += 1;
    } else { /* padding, or a MAC CE a Msg4 does not carry */
      break;
    }
  }
  if (!cont_res)
    return false; /* not a Msg4 */

  pthread_mutex_lock(&R.lock);
  expire_ues();
  const uint16_t tc = R.last_tc_dl;
  const int i = find_tc(tc);
  const bool room = nb_active_ues() < R.max_ues;
  NR_CellGroupConfig_t *cg = NULL;
  int inst = -1;
  if (R.follow && i >= 0 && ccch && room && (cg = cellgroup_from_rrcsetup(ccch, ccch_len))
      && (inst = add_ue(tc)) > 0) {
    remove_tc(i); /* from now on its shadow MAC instance owns this RNTI */
    nr_sniffer_csv_row(&R.ue_csv, "%lu,%d,%d,0x%04x,follow,0x%02x%02x%02x%02x%02x%02x", nr_sniffer_now_us(), frame, slot,
                       tc, cont_res[0], cont_res[1], cont_res[2], cont_res[3], cont_res[4], cont_res[5]);
    printf("[SNIFFER] Msg4 at %d.%d: following UE with C-RNTI %04x (shadow MAC %d, %d UEs followed)\n", frame, slot, tc,
           inst, nb_active_ues());
    *rnti = tc;
    *cell_group = cg;
    *instance = inst;
  } else {
    ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);
    printf("[SNIFFER] Msg4 at %d.%d on TC-RNTI %04x not followed (%s)\n", frame, slot, tc,
           i < 0 ? "TC-RNTI not from a decoded RAR" : !ccch ? "no RRCSetup" : !room ? "max_ues reached"
                                                                                  : "RRCSetup not decoded");
  }
  fflush(stdout);
  pthread_mutex_unlock(&R.lock);
  return true;
}

int nr_sniffer_shadow_instance(uint16_t rnti)
{
  pthread_mutex_lock(&R.lock);
  const int i = find_ue(rnti);
  const int instance = i >= 0 ? R.ues[i].instance : -1;
  pthread_mutex_unlock(&R.lock);
  return instance;
}

int nr_sniffer_shadow_instances(int *instances, int max)
{
  pthread_mutex_lock(&R.lock);
  expire_ues(); /* called every slot by the MAC */
  int n = 0;
  for (int i = 0; i < R.nb_ues && n < max; i++)
    if (R.ues[i].active)
      instances[n++] = R.ues[i].instance;
  pthread_mutex_unlock(&R.lock);
  return n;
}

void nr_sniffer_ra_on_dci(const nr_sniffer_dci_t *dci)
{
  pthread_mutex_lock(&R.lock);
  if (dci->rnti_type == TYPE_TC_RNTI_ && dci->dci_format == NR_DL_DCI_FORMAT_1_0) {
    R.last_tc_dl = dci->rnti;
  } else if (dci->rnti_type == TYPE_C_RNTI_) {
    const int i = find_ue(dci->rnti);
    if (i >= 0)
      R.ues[i].last_us = nr_sniffer_now_us();
  }
  pthread_mutex_unlock(&R.lock);
}

/* ------------------------------------------------------------------------- */
/* known configuration (NR-Scope fixed_rnti)                                 */
/* ------------------------------------------------------------------------- */

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
  const bool room = R.nb_known < MAX_KNOWN_UE;
  if (room)
    R.known[R.nb_known++] = (known_ue_t){.cg = cg, .rnti = rnti};
  pthread_mutex_unlock(&R.lock);
  if (!room) {
    fprintf(stderr, "[SNIFFER] at most %d known configurations\n", MAX_KNOWN_UE);
    ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);
    return false;
  }
  printf("[SNIFFER] Known configuration: %s from %s (%d bytes) for C-RNTI %04x\n", kind, path, len, rnti);
  fflush(stdout);
  return true;
}

void *nr_sniffer_known_ue(uint16_t *rnti, int *instance)
{
  pthread_mutex_lock(&R.lock);
  NR_CellGroupConfig_t *cg = NULL;
  while (!cg && R.nb_known > 0) {
    const known_ue_t k = R.known[--R.nb_known];
    const int inst = add_ue(k.rnti);
    if (inst < 0) {
      fprintf(stderr, "[SNIFFER] not following C-RNTI %04x: max_ues reached or already followed\n", k.rnti);
      ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, k.cg);
      continue;
    }
    cg = k.cg;
    *rnti = k.rnti;
    *instance = inst;
    nr_sniffer_csv_row(&R.ue_csv, "%lu,-1,-1,0x%04x,known,", nr_sniffer_now_us(), k.rnti);
    printf("[SNIFFER] SIB1 applied: following UE with C-RNTI %04x (known configuration, shadow MAC %d)\n", k.rnti, inst);
    fflush(stdout);
  }
  pthread_mutex_unlock(&R.lock);
  return cg;
}
