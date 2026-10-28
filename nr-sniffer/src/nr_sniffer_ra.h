/*
 * nr-sniffer: following other UEs through random access (milestone 2).
 *
 * RA-RNTI (from the SIB1 PRACH configuration) -> RAR -> TC-RNTI -> Msg4 -> C-RNTI.
 * On Msg4 the sniffer "adopts" the UE: OAI's UE MAC takes its TC-RNTI as C-RNTI and the RRCSetup
 * in the same PDU configures the MAC like that UE, so OAI's own DCI decoding follows it.
 */

#ifndef NR_SNIFFER_RA_H
#define NR_SNIFFER_RA_H

#include <stdbool.h>

#include "common/utils/nr_sniffer_hooks.h"

/// Open rar.csv and ue.csv in \p out_dir. \p follow: 0 disables RA tracking (milestone 1 behaviour).
void nr_sniffer_ra_init(const char *out_dir, bool follow);
/// Compute the RA-RNTIs of the cell from SIB1 (\p sib1 is a const NR_SIB1_t *).
void nr_sniffer_ra_configure(const void *sib1, bool fr2);
/// Every DCI the sniffer decodes (used to find the Msg4 TC-RNTI).
void nr_sniffer_ra_on_dci(const nr_sniffer_dci_t *dci);

#endif
