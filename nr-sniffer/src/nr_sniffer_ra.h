/*
 * nr-sniffer: following other UEs through random access (M2), or with a known configuration (M3).
 *
 * RA-RNTI (from the SIB1 PRACH configuration) -> RAR -> TC-RNTI -> Msg4 -> C-RNTI. On Msg4 the
 * sniffer follows the UE in a shadow MAC instance of OAI's UE MAC, configured with the cell
 * configuration plus the CellGroupConfig of that UE's RRCSetup, so that OAI's own DCI decoding
 * follows it. Several UEs are followed at once, one shadow instance each.
 */

#ifndef NR_SNIFFER_RA_H
#define NR_SNIFFER_RA_H

#include <stdbool.h>

#include "common/utils/nr_sniffer_hooks.h"

/// UEs followed at once at most: shadow MAC instances 1..14 (OAI has 32 UE instances; each followed UE adds about
/// two search spaces to the sniffer's per-slot PDCCH configuration, FAPI_NR_DL_CONFIG_LIST_NUM = 32)
#define NR_SNIFFER_MAX_UES 14

/// Open rar.csv and ue.csv in \p out_dir. \p follow: 0 disables RA tracking (milestone 1 behaviour).
/// \p max_ues: how many UEs to follow at once at most (capped at NR_SNIFFER_MAX_UES). \p ue_timeout_s: a UE
/// without a DCI for that long is no longer followed and its shadow MAC instance is reused (0: never).
void nr_sniffer_ra_init(const char *out_dir, bool follow, int max_ues, double ue_timeout_s);
/// Forget all tracked RNTIs, followed UEs, known configurations and SIB1 (unit tests; output files stay open).
void nr_sniffer_ra_reset(void);
/// Compute the RA-RNTIs of the cell from SIB1 (\p sib1 is a const NR_SIB1_t *) and keep a copy of it.
void nr_sniffer_ra_configure(const void *sib1, bool fr2);
/// Load the configuration of a UE to follow without seeing its random access (NR-Scope's fixed_rnti):
/// \p path holds a UPER NR_CellGroupConfig (e.g. saved by nr-gnb-dci-logger -c) or an
/// NR_RRCReconfiguration (e.g. phy-test's reconfig.raw). \p rnti < 0 takes the C-RNTI from
/// reconfigurationWithSync. It is followed once SIB1 is applied. Returns false if the file cannot be used.
bool nr_sniffer_ra_load_known_ue(const char *path, int rnti);
/// Every DCI the sniffer decodes (used to find the Msg4 TC-RNTI).
void nr_sniffer_ra_on_dci(const nr_sniffer_dci_t *dci);

#endif
