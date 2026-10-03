/* AURIX - device readings from /proc and /sys (pure C, host-testable). */
#ifndef AURIX_SYSINFO_H
#define AURIX_SYSINFO_H
#include <stdint.h>

typedef struct {
    unsigned long long total, idle;    /* /proc/stat counters (aggregate) */
} cpu_counters;

typedef struct {
    double cpu_pct;                    /* since previous sample, -1 if unknown */
    int cores;
    double load1, load5;
    uint64_t mem_total_kb, mem_available_kb;
    uint64_t app_rss_kb;
    double temp_c;                     /* hottest thermal zone, -1000 if none */
    double uptime_s;
    uint64_t storage_free_kb, storage_total_kb;   /* filesystem holding `storage_path` */
} sys_reading;

/* `prev` carries CPU counters between calls. storage_path may be NULL. */
void sysinfo_read(sys_reading *out, cpu_counters *prev, const char *storage_path);

/* Parsers exposed for tests. */
int sysinfo_parse_meminfo(const char *text, uint64_t *total_kb, uint64_t *available_kb);
int sysinfo_parse_stat(const char *text, cpu_counters *c, int *cores);

#endif
