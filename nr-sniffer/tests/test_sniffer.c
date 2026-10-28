/*
 * nr-sniffer unit tests: the RA tracker (nr_sniffer_ra.c) and the DCI CSV schema (nr_sniffer_dci.c),
 * without a radio or an OAI UE. Run by scripts/unit_tests.sh.
 *
 * Usage: nr-sniffer-unit-tests <tests/data dir> <scratch dir>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/utils/nr/nr_common.h"
#include "executables/softmodem-common.h"
#include "common/utils/LOG/log.h"
#include "openair2/LAYER2/NR_MAC_COMMON/nr_mac.h"
#include "NR_CellGroupConfig.h"
#include "NR_SIB1.h"
#include "nr_sniffer_csv.h"
#include "nr_sniffer_dci.h"
#include "nr_sniffer_ra.h"

/* provided by the softmodem executables, which the unit tests do not link */
void exit_function(const char *file, const char *function, const int line, const char *s, const int assert)
{
  fprintf(stderr, "exit_function() called from %s:%d %s(): %s\n", file, line, function, s);
  exit(3);
}
configmodule_interface_t *uniqCfg = NULL;
static softmodem_params_t softmodem_params;
softmodem_params_t *get_softmodem_params(void)
{
  return &softmodem_params;
}

static int nb_checks, nb_failures;
#define CHECK(cond)                                                    \
  do {                                                                 \
    nb_checks++;                                                       \
    if (!(cond)) {                                                     \
      nb_failures++;                                                   \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                  \
  } while (0)

static const char *data_dir, *out_dir;

static int read_file(const char *name, uint8_t *buf, int size)
{
  char path[600];
  snprintf(path, sizeof(path), "%s/%s", data_dir, name);
  FILE *f = fopen(path, "r");
  if (!f) {
    perror(path);
    exit(2);
  }
  const int len = fread(buf, 1, size, f);
  fclose(f);
  return len;
}

static int count_lines(const char *name)
{
  char path[600];
  snprintf(path, sizeof(path), "%s/%s", out_dir, name);
  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  int n = 0, c;
  while ((c = fgetc(f)) != EOF)
    n += c == '\n';
  fclose(f);
  return n;
}

/* SIB1 of OAI's gnb.sa.band78.106prb.rfsim.conf as decoded by nr-sniffer (XER) */
static NR_SIB1_t *load_sib1(void)
{
  static uint8_t buf[65536];
  const int len = read_file("sib1_band78_106prb.xml", buf, sizeof(buf));
  NR_SIB1_t *sib1 = NULL;
  asn_dec_rval_t r = xer_decode(NULL, &asn_DEF_NR_SIB1, (void **)&sib1, buf, len);
  if (r.code != RC_OK) {
    fprintf(stderr, "cannot decode the SIB1 fixture\n");
    exit(2);
  }
  return sib1;
}

static nr_sniffer_dci_t dci(int rnti, int rnti_type, int format)
{
  return (nr_sniffer_dci_t){.rnti = rnti, .rnti_type = rnti_type, .dci_format = format};
}

/* RA-RNTI = 1 + s_id + 14 t_id (+ 1120 f_id): PRACH config 98, 30 kHz, msg1-FDM 1 gives the occasions
 * at t_id 19, s_id 0/4/8, which is what the OAI gNB used in every rfsim run (RA-RNTI 010b/010f/0113) */
static void test_ra_rnti_set(NR_SIB1_t *sib1)
{
  nr_sniffer_ra_reset();
  CHECK(nr_sniffer_ra_watch_rnti() == 0);
  nr_sniffer_ra_configure(sib1, false);
  CHECK(nr_sniffer_ra_watch_rnti() == 0x010b);
  CHECK(nr_sniffer_rnti_type(0x010b) == TYPE_RA_RNTI_);
  CHECK(nr_sniffer_rnti_type(0x010f) == TYPE_RA_RNTI_);
  CHECK(nr_sniffer_rnti_type(0x0113) == TYPE_RA_RNTI_);
  CHECK(nr_sniffer_rnti_type(0x0117) == -1);
  CHECK(nr_sniffer_rnti_type(0xffff) == -1);
}

static void test_match_rnti(NR_SIB1_t *sib1)
{
  nr_sniffer_ra_reset();
  nr_sniffer_ra_configure(sib1, false);
  const uint16_t cfg = nr_sniffer_ra_watch_rnti();
  CHECK(nr_sniffer_match_rnti(cfg, 0x010f));
  CHECK(nr_sniffer_match_rnti(cfg, 0x0113));
  CHECK(!nr_sniffer_match_rnti(cfg, 0x4601)); /* not tracked */
  CHECK(!nr_sniffer_match_rnti(cfg, 0x01010f)); /* residual CRC bits left: not a valid DCI */
  CHECK(!nr_sniffer_match_rnti(0x4601, 0x010f)); /* not the RA-watch PDCCH configuration */
}

/* MAC RAR PDU (TS 38.321 6.1.5, 6.2.3): BI subheader, then two RAPID subPDUs */
static int build_rar(uint8_t *p)
{
  int n = 0;
  p[n++] = 0x80 | 0x05; /* E=1 T=0: backoff indicator 5 */
  p[n++] = 0x80 | 0x40 | 35; /* E=1 T=1 RAPID 35 */
  const uint8_t rar1[7] = {0x00, 0x08, 0x00, 0x10, 0x20, 0xc8, 0x5d}; /* TA 1, TC-RNTI c85d */
  memcpy(&p[n], rar1, 7);
  n += 7;
  p[n++] = 0x40 | 49; /* E=0 T=1 RAPID 49 */
  const uint8_t rar2[7] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x2b, 0x6c}; /* TA 0, TC-RNTI 2b6c */
  memcpy(&p[n], rar2, 7);
  n += 7;
  return n;
}

static void test_rar_and_msg4(NR_SIB1_t *sib1)
{
  nr_sniffer_ra_reset();
  nr_sniffer_ra_configure(sib1, false);
  const int rar_rows = count_lines("rar.csv");
  const int ue_rows = count_lines("ue.csv");
  uint8_t pdu[32];
  const int len = build_rar(pdu);
  nr_sniffer_on_rar(pdu, len, 130, 10);
  CHECK(count_lines("rar.csv") == rar_rows + 2);
  CHECK(nr_sniffer_rnti_type(0xc85d) == TYPE_TC_RNTI_);
  CHECK(nr_sniffer_rnti_type(0x2b6c) == TYPE_TC_RNTI_);
  CHECK(nr_sniffer_match_rnti(nr_sniffer_ra_watch_rnti(), 0x2b6c));

  /* a truncated RAR must not be read past its end */
  nr_sniffer_on_rar(pdu, 5, 131, 10);
  CHECK(count_lines("rar.csv") == rar_rows + 2);

  /* no Msg4 DCI seen yet: nothing to adopt */
  const uint8_t cont_res[6] = {1, 2, 3, 4, 5, 6};
  CHECK(nr_sniffer_on_contention_resolution(cont_res, 131, 10) == 0);

  /* Msg4 DCI 1_0 on TC-RNTI 2b6c, then its contention resolution CE */
  const nr_sniffer_dci_t msg4 = dci(0x2b6c, TYPE_TC_RNTI_, NR_DL_DCI_FORMAT_1_0);
  nr_sniffer_ra_on_dci(&msg4);
  CHECK(nr_sniffer_on_contention_resolution(cont_res, 131, 10) == 0x2b6c);
  CHECK(count_lines("ue.csv") == ue_rows + 1);
  CHECK(nr_sniffer_rnti_type(0x2b6c) == -1); /* now the MAC's own C-RNTI */
  CHECK(nr_sniffer_rnti_type(0xc85d) == TYPE_TC_RNTI_);

  /* one UE only (M2): a second Msg4 is not adopted, and new RARs no longer add TC-RNTIs */
  const nr_sniffer_dci_t msg4b = dci(0xc85d, TYPE_TC_RNTI_, NR_DL_DCI_FORMAT_1_0);
  nr_sniffer_ra_on_dci(&msg4b);
  CHECK(nr_sniffer_on_contention_resolution(cont_res, 132, 10) == 0);
  CHECK(count_lines("ue.csv") == ue_rows + 1);
}

static void test_ul_dci_is_not_msg4(NR_SIB1_t *sib1)
{
  nr_sniffer_ra_reset();
  nr_sniffer_ra_configure(sib1, false);
  uint8_t pdu[32];
  nr_sniffer_on_rar(pdu, build_rar(pdu), 130, 10);
  /* Msg3 retransmission grant (DCI 0_0 on TC-RNTI) must not mark the Msg4 TC-RNTI */
  const nr_sniffer_dci_t msg3 = dci(0xc85d, TYPE_TC_RNTI_, NR_UL_DCI_FORMAT_0_0);
  nr_sniffer_ra_on_dci(&msg3);
  const uint8_t cont_res[6] = {0};
  CHECK(nr_sniffer_on_contention_resolution(cont_res, 131, 10) == 0);
}

static void test_known_ue(NR_SIB1_t *sib1)
{
  char path[600];
  uint16_t rnti = 0;

  /* plain CellGroupConfig (exported by nr-gnb-dci-logger -c): needs the C-RNTI */
  nr_sniffer_ra_reset();
  snprintf(path, sizeof(path), "%s/cellgroup_rrcsetup.uper", data_dir);
  CHECK(!nr_sniffer_ra_load_known_ue(path, -1));
  CHECK(nr_sniffer_known_ue(&rnti) == NULL);
  CHECK(nr_sniffer_ra_load_known_ue(path, 0x1a2b));
  NR_CellGroupConfig_t *cg = nr_sniffer_known_ue(&rnti);
  CHECK(cg != NULL);
  CHECK(rnti == 0x1a2b);
  CHECK(cg && cg->spCellConfig && cg->spCellConfig->spCellConfigDedicated);
  CHECK(nr_sniffer_known_ue(&rnti) == NULL); /* handed over once */
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);
  /* the known UE is the followed one: RA does not adopt another */
  nr_sniffer_ra_configure(sib1, false);
  uint8_t pdu[32];
  nr_sniffer_on_rar(pdu, build_rar(pdu), 130, 10);
  CHECK(nr_sniffer_rnti_type(0x2b6c) == -1);

  /* phy-test RRCReconfiguration: CellGroupConfig in secondaryCellGroup, C-RNTI from reconfigurationWithSync */
  nr_sniffer_ra_reset();
  snprintf(path, sizeof(path), "%s/phytest_reconfig.raw", data_dir);
  CHECK(nr_sniffer_ra_load_known_ue(path, -1));
  cg = nr_sniffer_known_ue(&rnti);
  CHECK(cg != NULL);
  CHECK(rnti == 0x1234);
  CHECK(cg && cg->spCellConfig && cg->spCellConfig->reconfigurationWithSync == NULL);
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);

  /* --sniffer.rnti wins over newUE-Identity */
  nr_sniffer_ra_reset();
  CHECK(nr_sniffer_ra_load_known_ue(path, 0x4242));
  cg = nr_sniffer_known_ue(&rnti);
  CHECK(rnti == 0x4242);
  ASN_STRUCT_FREE(asn_DEF_NR_CellGroupConfig, cg);

  /* neither format, or no file */
  nr_sniffer_ra_reset();
  snprintf(path, sizeof(path), "%s/phytest_rbconfig.raw", data_dir);
  CHECK(!nr_sniffer_ra_load_known_ue(path, 0x1234));
  snprintf(path, sizeof(path), "%s/does_not_exist.uper", data_dir);
  CHECK(!nr_sniffer_ra_load_known_ue(path, 0x1234));
  CHECK(nr_sniffer_known_ue(&rnti) == NULL);
}

static void test_dci_row(void)
{
  const nr_sniffer_dci_t d = {
      .timestamp_us = 1793194561355383ULL,
      .sfn = 209,
      .slot = 1,
      .rnti = 0xb118,
      .rnti_type = TYPE_C_RNTI_,
      .dci_format = NR_UL_DCI_FORMAT_0_0,
      .bwp_id = 0,
      .coreset_id = 0,
      .ss_id = 4,
      .aggregation_level = 4,
      .cce_index = 0,
      .prb_start = 0,
      .prb_len = 5,
      .mcs = 0,
      .mcs_table = 0,
      .tbs_bits = 24,
      .ndi = 0,
      .rv = 0,
      .harq_id = 0,
  };
  char row[512];
  nr_sniffer_dci_row(row, sizeof(row), &d, "sniffer");
  /* the exact row the gNB logger wrote for the same DCI in an rfsim run, except source */
  CHECK(!strcmp(row, "1793194561355383,209,1,0xb118,c,0_0,ul,0,0,4,4,0,0,5,0,qam64,24,0,0,0,sniffer"));
  /* one CSV column per header field */
  int commas_row = 0, commas_header = 0;
  for (const char *p = row; *p; p++)
    commas_row += *p == ',';
  for (const char *p = NR_SNIFFER_DCI_HEADER; *p; p++)
    commas_header += *p == ',';
  CHECK(commas_row == commas_header);

  CHECK(!strcmp(nr_sniffer_dci_direction(NR_DL_DCI_FORMAT_1_1), "dl"));
  CHECK(!strcmp(nr_sniffer_dci_direction(NR_UL_DCI_FORMAT_0_1), "ul"));
  CHECK(!strcmp(nr_sniffer_dci_format_name(NR_DL_DCI_FORMAT_1_1), "1_1"));
  CHECK(!strcmp(nr_sniffer_rnti_type_name(TYPE_TC_RNTI_), "tc"));
  CHECK(!strcmp(nr_sniffer_rnti_type_name(TYPE_RA_RNTI_), "ra"));
  CHECK(!strcmp(nr_sniffer_mcs_table_name(1), "qam256"));
  CHECK(!strcmp(nr_sniffer_mcs_table_name(7), "unknown"));
}

int main(int argc, char **argv)
{
  if (argc != 3) {
    fprintf(stderr, "usage: %s <tests/data dir> <scratch dir>\n", argv[0]);
    return 2;
  }
  data_dir = argv[1];
  out_dir = argv[2];
  logInit(); /* OAI's LOG macros (used by the MAC helpers) need it, as in OAI's own unit tests */
  nr_sniffer_ra_init(out_dir, true);
  NR_SIB1_t *sib1 = load_sib1();

  test_ra_rnti_set(sib1);
  test_match_rnti(sib1);
  test_rar_and_msg4(sib1);
  test_ul_dci_is_not_msg4(sib1);
  test_known_ue(sib1);
  test_dci_row();

  ASN_STRUCT_FREE(asn_DEF_NR_SIB1, sib1);
  printf("%d checks, %d failures\n", nb_checks, nb_failures);
  printf("RESULT: %s\n", nb_failures ? "FAIL" : "PASS");
  return nb_failures ? 1 : 0;
}
