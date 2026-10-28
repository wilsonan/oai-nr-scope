/*
 * nr-sniffer: the DCI CSV schema shared by the sniffer and the gNB ground-truth logger.
 */

#include "nr_sniffer_dci.h"

#include <stdio.h>

#include "common/utils/nr/nr_common.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac.h"

const char *nr_sniffer_rnti_type_name(int rnti_type)
{
  switch (rnti_type) {
    case TYPE_C_RNTI_: return "c";
    case TYPE_CS_RNTI_: return "cs";
    case TYPE_TC_RNTI_: return "tc";
    case TYPE_P_RNTI_: return "p";
    case TYPE_SI_RNTI_: return "si";
    case TYPE_RA_RNTI_: return "ra";
    case TYPE_MSGB_RNTI_: return "msgb";
    case TYPE_MCS_C_RNTI_: return "mcs-c";
    default: return "other";
  }
}

const char *nr_sniffer_dci_format_name(int dci_format)
{
  switch (dci_format) {
    case NR_UL_DCI_FORMAT_0_0: return "0_0";
    case NR_UL_DCI_FORMAT_0_1: return "0_1";
    case NR_DL_DCI_FORMAT_1_0: return "1_0";
    case NR_DL_DCI_FORMAT_1_1: return "1_1";
    case NR_DL_DCI_FORMAT_2_0: return "2_0";
    case NR_DL_DCI_FORMAT_2_1: return "2_1";
    case NR_DL_DCI_FORMAT_2_2: return "2_2";
    case NR_DL_DCI_FORMAT_2_3: return "2_3";
    default: return "unknown";
  }
}

const char *nr_sniffer_dci_direction(int dci_format)
{
  return dci_format == NR_UL_DCI_FORMAT_0_0 || dci_format == NR_UL_DCI_FORMAT_0_1 ? "ul" : "dl";
}

const char *nr_sniffer_mcs_table_name(int mcs_table)
{
  /* FAPI: 0 qam64, 1 qam256, 2 qam64LowSE; PUSCH with transform precoding: 3 qam64, 4 qam64LowSE */
  static const char *names[] = {"qam64", "qam256", "qam64LowSE", "tp-qam64", "tp-qam64LowSE"};
  return mcs_table >= 0 && mcs_table < 5 ? names[mcs_table] : "unknown";
}

int nr_sniffer_dci_row(char *buf, int size, const nr_sniffer_dci_t *d, const char *source)
{
  return snprintf(buf, size, "%lu,%d,%d,0x%04x,%s,%s,%s,%d,%d,%d,%d,%d,%d,%d,%d,%s,%d,%d,%d,%d,%s",
                  (unsigned long)d->timestamp_us, d->sfn, d->slot, d->rnti, nr_sniffer_rnti_type_name(d->rnti_type),
                  nr_sniffer_dci_format_name(d->dci_format), nr_sniffer_dci_direction(d->dci_format), d->bwp_id,
                  d->coreset_id, d->ss_id, d->aggregation_level, d->cce_index, d->prb_start, d->prb_len, d->mcs,
                  nr_sniffer_mcs_table_name(d->mcs_table), d->tbs_bits, d->ndi, d->rv, d->harq_id, source);
}
