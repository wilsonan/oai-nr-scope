/*
 * nr-sniffer: the DCI CSV schema shared by the sniffer (source=sniffer) and the
 * gNB ground-truth logger (source=gnb), see PLAN.md.
 */

#ifndef NR_SNIFFER_DCI_H
#define NR_SNIFFER_DCI_H

#include <stdint.h>

#include "common/utils/nr_sniffer_hooks.h"

#define NR_SNIFFER_DCI_HEADER                                                                                      \
  "timestamp_us,sfn,slot,rnti,rnti_type,dci_format,direction,bwp_id,coreset_id,ss_id,aggregation_level,cce_index," \
  "prb_start,prb_len,mcs,mcs_table,tbs_bits,ndi,rv,harq_id,source"

/* nr_sniffer_dci_t (one DCI with the allocation it schedules) is defined in OAI's nr_sniffer_hooks.h */

const char *nr_sniffer_rnti_type_name(int rnti_type);
const char *nr_sniffer_dci_format_name(int dci_format);
/// "dl" or "ul"
const char *nr_sniffer_dci_direction(int dci_format);
const char *nr_sniffer_mcs_table_name(int mcs_table);
/// Format \p d as one CSV row (no newline) into \p buf. Returns snprintf's result.
int nr_sniffer_dci_row(char *buf, int size, const nr_sniffer_dci_t *d, const char *source);

#endif
