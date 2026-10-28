/*
 * nr-sniffer: small CSV writer used for all sniffer logs.
 */

#ifndef NR_SNIFFER_CSV_H
#define NR_SNIFFER_CSV_H

#include <stdio.h>
#include <stdint.h>

typedef struct {
  FILE *fp;
  char path[512];
} nr_sniffer_csv_t;

/// Open (append) \p dir/\p name; writes \p header if the file is new or empty. Returns 0 on success.
int nr_sniffer_csv_open(nr_sniffer_csv_t *csv, const char *dir, const char *name, const char *header);
/// Append one row (printf-style, without trailing newline) and flush it.
void nr_sniffer_csv_row(nr_sniffer_csv_t *csv, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void nr_sniffer_csv_close(nr_sniffer_csv_t *csv);
/// Wall-clock time in microseconds since the Unix epoch.
uint64_t nr_sniffer_now_us(void);

#endif
