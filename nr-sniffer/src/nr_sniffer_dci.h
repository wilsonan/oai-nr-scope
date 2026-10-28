/*
 * nr-sniffer: the DCI CSV schema shared by the sniffer (source=sniffer) and the
 * gNB ground-truth logger (source=gnb), see PLAN.md.
 */

#ifndef NR_SNIFFER_DCI_H
#define NR_SNIFFER_DCI_H

#include <stdint.h>

#define NR_SNIFFER_DCI_HEADER                                                                                      \
  "timestamp_us,sfn,slot,rnti,rnti_type,dci_format,direction,bwp_id,coreset_id,ss_id,aggregation_level,cce_index," \
  "prb_start,prb_len,mcs,mcs_table,tbs_bits,ndi,rv,harq_id,source"

/// One DCI, with the PDSCH/PUSCH allocation it schedules. prb_start is relative to the BWP start.
typedef struct {
  uint64_t timestamp_us;
  int sfn, slot; ///< frame and slot the PDCCH was sent in
  int rnti;
  int rnti_type; ///< nr_rnti_type_t
  int dci_format; ///< nr_dci_format_t
  int bwp_id, coreset_id, ss_id;
  int aggregation_level, cce_index;
  int prb_start, prb_len;
  int mcs;
  int mcs_table; ///< FAPI mcsTable / mcs_table index
  int tbs_bits;
  int ndi, rv, harq_id;
} nr_sniffer_dci_t;

const char *nr_sniffer_rnti_type_name(int rnti_type);
const char *nr_sniffer_dci_format_name(int dci_format);
/// "dl" or "ul"
const char *nr_sniffer_dci_direction(int dci_format);
const char *nr_sniffer_mcs_table_name(int mcs_table);
/// Format \p d as one CSV row (no newline) into \p buf. Returns snprintf's result.
int nr_sniffer_dci_row(char *buf, int size, const nr_sniffer_dci_t *d, const char *source);

#endif
