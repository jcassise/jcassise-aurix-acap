#include "sysinfo.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>

static int slurp(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t k = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[k] = 0;
    return (int)k;
}

int sysinfo_parse_meminfo(const char *t, uint64_t *total, uint64_t *avail)
{
    const char *p = strstr(t, "MemTotal:"), *q = strstr(t, "MemAvailable:");
    if (!p) return -1;
    *total = strtoull(p + 9, NULL, 10);
    if (q) *avail = strtoull(q + 13, NULL, 10);
    else {   /* old kernels: free + buffers + cached */
        const char *f = strstr(t, "MemFree:"), *b = strstr(t, "Buffers:"), *c = strstr(t, "\nCached:");
        *avail = (f ? strtoull(f + 8, NULL, 10) : 0) + (b ? strtoull(b + 8, NULL, 10) : 0) +
                 (c ? strtoull(c + 8, NULL, 10) : 0);
    }
    return 0;
}

int sysinfo_parse_stat(const char *t, cpu_counters *c, int *cores)
{
    unsigned long long v[8] = { 0 };
    int k = sscanf(t, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
                   &v[6], &v[7]);
    if (k < 4) return -1;
    c->idle = v[3] + (k > 4 ? v[4] : 0);
    c->total = 0;
    for (int i = 0; i < k; i++) c->total += v[i];
    int n = 0;
    for (const char *p = t; (p = strstr(p, "\ncpu")); p += 4)
        if (p[4] >= '0' && p[4] <= '9') n++;
    *cores = n ? n : 1;
    return 0;
}

static double hottest_zone(void)
{
    double best = -1000;
    DIR *d = opendir("/sys/class/thermal");
    if (!d) return best;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "thermal_zone", 12)) continue;
        char path[300], buf[32];
        snprintf(path, sizeof path, "/sys/class/thermal/%s/temp", e->d_name);
        if (slurp(path, buf, sizeof buf) > 0) {
            double c = atof(buf) / 1000.0;
            if (c > -50 && c < 200 && c > best) best = c;
        }
    }
    closedir(d);
    return best;
}

void sysinfo_read(sys_reading *o, cpu_counters *prev, const char *storage_path)
{
    memset(o, 0, sizeof *o);
    o->cpu_pct = -1;
    o->temp_c = -1000;
    static char buf[16384];

    cpu_counters now;
    if (slurp("/proc/stat", buf, sizeof buf) > 0 && !sysinfo_parse_stat(buf, &now, &o->cores)) {
        if (prev->total && now.total > prev->total)
            o->cpu_pct = 100.0 * (1.0 - (double)(now.idle - prev->idle) / (double)(now.total - prev->total));
        *prev = now;
    }
    if (slurp("/proc/meminfo", buf, sizeof buf) > 0)
        sysinfo_parse_meminfo(buf, &o->mem_total_kb, &o->mem_available_kb);
    if (slurp("/proc/self/status", buf, sizeof buf) > 0) {
        const char *p = strstr(buf, "VmRSS:");
        if (p) o->app_rss_kb = strtoull(p + 6, NULL, 10);
    }
    if (slurp("/proc/loadavg", buf, sizeof buf) > 0) sscanf(buf, "%lf %lf", &o->load1, &o->load5);
    if (slurp("/proc/uptime", buf, sizeof buf) > 0) o->uptime_s = atof(buf);
    o->temp_c = hottest_zone();
    struct statvfs sv;
    if (storage_path && !statvfs(storage_path, &sv)) {
        o->storage_free_kb = (uint64_t)sv.f_bavail * sv.f_frsize / 1024;
        o->storage_total_kb = (uint64_t)sv.f_blocks * sv.f_frsize / 1024;
    }
}
