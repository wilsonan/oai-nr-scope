/*
 * nr-gnb-dci-logger: T-tracer client that writes every DCI an OAI gNB sends
 * (GNB_MAC_DCI events) to CSV in the nr-sniffer DCI schema, with source=gnb.
 * This is the ground truth the sniffer's dci.csv is compared against.
 *
 * Run the gNB with --T_stdout 2 (or 0); it waits for this logger to connect.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "configuration.h"
#include "database.h"
#include "event.h"
#include "handler.h"
#include "nr_sniffer_dci.h"

#define DEFAULT_IP "127.0.0.1"
#define DEFAULT_PORT 2021

/* GNB_MAC_DCI fields, in the order they are copied into nr_sniffer_dci_t */
static const char *FIELDS[] = {"frame",     "slot",      "rnti",     "rnti_type",         "dci_format", "bwp_id",
                               "coreset_id", "ss_id",    "aggregation_level", "cce_index",  "prb_start",  "prb_len",
                               "mcs",        "mcs_table", "tbs_bits", "ndi",              "rv",          "harq_pid"};
#define NB_FIELDS (sizeof(FIELDS) / sizeof(FIELDS[0]))

typedef struct {
  int index[NB_FIELDS];
  FILE *out;
  unsigned long count;
} logger_t;

/* Every row is flushed when written, so stopping is just exiting. Waiting for the socket to close is not
 * enough: the gNB's forked T local tracer can keep it open for a while after the gNB is gone. */
static void on_signal(int sig)
{
  (void)sig;
  _exit(0);
}

static void on_dci(void *priv, event e)
{
  logger_t *l = priv;
  int v[NB_FIELDS];
  for (size_t i = 0; i < NB_FIELDS; i++)
    v[i] = e.e[l->index[i]].i;
  const nr_sniffer_dci_t d = {
      .timestamp_us = (uint64_t)e.sending_time.tv_sec * 1000000ULL + e.sending_time.tv_nsec / 1000,
      .sfn = v[0],
      .slot = v[1],
      .rnti = v[2],
      .rnti_type = v[3],
      .dci_format = v[4],
      .bwp_id = v[5],
      .coreset_id = v[6],
      .ss_id = v[7],
      .aggregation_level = v[8],
      .cce_index = v[9],
      .prb_start = v[10],
      .prb_len = v[11],
      .mcs = v[12],
      .mcs_table = v[13],
      .tbs_bits = v[14],
      .ndi = v[15],
      .rv = v[16],
      .harq_id = v[17],
  };
  char row[512];
  nr_sniffer_dci_row(row, sizeof(row), &d, "gnb");
  fprintf(l->out, "%s\n", row);
  fflush(l->out);
  l->count++;
}

static void usage(void)
{
  printf("usage: nr-gnb-dci-logger -d <T_messages.txt> [options]\n"
         "    -d <database>   OAI's common/utils/T/T_messages.txt (mandatory)\n"
         "    -ip <address>   gNB T-tracer address (default %s)\n"
         "    -p <port>       gNB T-tracer port (default %d)\n"
         "    -o <file>       output CSV (default: stdout)\n",
         DEFAULT_IP,
         DEFAULT_PORT);
  exit(1);
}

int main(int n, char **v)
{
  char *database_filename = NULL;
  char *ip = DEFAULT_IP;
  int port = DEFAULT_PORT;
  char *out_filename = NULL;

  for (int i = 1; i < n; i++) {
    if (!strcmp(v[i], "-h") || !strcmp(v[i], "--help")) usage();
    if (!strcmp(v[i], "-d") && i < n - 1) { database_filename = v[++i]; continue; }
    if (!strcmp(v[i], "-ip") && i < n - 1) { ip = v[++i]; continue; }
    if (!strcmp(v[i], "-p") && i < n - 1) { port = atoi(v[++i]); continue; }
    if (!strcmp(v[i], "-o") && i < n - 1) { out_filename = v[++i]; continue; }
    usage();
  }
  if (!database_filename)
    usage();

  void *database = parse_database(database_filename);
  load_config_file(database_filename);
  event_handler *h = new_handler(database);

  logger_t l = {.out = stdout};
  const int ev_id = event_id_from_name(database, "GNB_MAC_DCI");
  const database_event_format f = get_format(database, ev_id);
  for (size_t i = 0; i < NB_FIELDS; i++) {
    int k;
    for (k = 0; k < f.count; k++)
      if (!strcmp(f.name[k], FIELDS[i]))
        break;
    if (k == f.count || strcmp(f.type[k], "int")) {
      fprintf(stderr, "fatal: GNB_MAC_DCI has no int field '%s' (is the nr-sniffer OAI patch applied?)\n", FIELDS[i]);
      exit(1);
    }
    l.index[i] = k;
  }

  if (out_filename) {
    l.out = fopen(out_filename, "w");
    if (!l.out) {
      perror(out_filename);
      exit(1);
    }
  }
  fprintf(l.out, "%s\n", NR_SNIFFER_DCI_HEADER);
  fflush(l.out);

  /* connect (retries until the gNB is up) and turn on GNB_MAC_DCI only */
  const int in = connect_to(ip, port);
  int nb_events = number_of_ids(database);
  int *is_on = calloc(nb_events, sizeof(int));
  if (!is_on)
    abort();
  on_off(database, "GNB_MAC_DCI", is_on, 1);
  char mt = 1;
  if (socket_send(in, &mt, 1) == -1 || socket_send(in, &nb_events, sizeof(int)) == -1
      || socket_send(in, is_on, nb_events * sizeof(int)) == -1) {
    fprintf(stderr, "fatal: socket_send failed\n");
    exit(1);
  }
  free(is_on);
  register_handler_function(h, ev_id, on_dci, &l);
  fprintf(stderr, "nr-gnb-dci-logger: connected to %s:%d, logging GNB_MAC_DCI\n", ip, port);

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  OBUF ebuf = {.osize = 0, .omaxsize = 0, .obuf = NULL};
  while (1) {
    event e = get_event(in, &ebuf, database);
    if (e.type == -1)
      break;
    handle_event(h, e);
  }
  fprintf(stderr, "nr-gnb-dci-logger: %lu DCIs logged\n", l.count);
  if (l.out != stdout)
    fclose(l.out);
  return 0;
}
