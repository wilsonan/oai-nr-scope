/*
 * nr-sniffer: small CSV writer used for all sniffer logs.
 */

#include "nr_sniffer_csv.h"

#include <errno.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

int nr_sniffer_csv_open(nr_sniffer_csv_t *csv, const char *dir, const char *name, const char *header)
{
  memset(csv, 0, sizeof(*csv));
  snprintf(csv->path, sizeof(csv->path), "%s/%s", dir, name);
  csv->fp = fopen(csv->path, "a");
  if (!csv->fp) {
    fprintf(stderr, "[SNIFFER] cannot open %s: %s\n", csv->path, strerror(errno));
    return -1;
  }
  struct stat st;
  if (fstat(fileno(csv->fp), &st) == 0 && st.st_size == 0) {
    fprintf(csv->fp, "%s\n", header);
    fflush(csv->fp);
  }
  return 0;
}

void nr_sniffer_csv_row(nr_sniffer_csv_t *csv, const char *fmt, ...)
{
  if (!csv->fp)
    return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(csv->fp, fmt, ap);
  va_end(ap);
  fputc('\n', csv->fp);
  fflush(csv->fp);
}

void nr_sniffer_csv_close(nr_sniffer_csv_t *csv)
{
  if (csv->fp)
    fclose(csv->fp);
  csv->fp = NULL;
}

uint64_t nr_sniffer_now_us(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}
