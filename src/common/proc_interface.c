#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "common/proc_interface.h"

/* Static helper: build path and open file for writing.
 * Returns NULL on failure. Caller must fclose the returned FILE*. */
static FILE *proc_open(const struct proc_interface *proc, const char *name)
{
    char path[PROC_PATH_MAX];
    int  n;

    n = snprintf(path, sizeof(path), "%s/%s", proc->dir, name);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return NULL;
    }
    return fopen(path, "w");
}

int proc_interface_init(struct proc_interface *proc, const char *dir)
{
    if (!proc) {
        return -1;
    }

    memset(proc, 0, sizeof(*proc));

    if (!dir || dir[0] == '\0') {
        dir = PROC_DIR_DEFAULT;
    }

    if (snprintf(proc->dir, sizeof(proc->dir), "%s", dir) >= (int)sizeof(proc->dir)) {
        return -1;
    }

    if (mkdir(proc->dir, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    proc->initialized = 1;
    return 0;
}

void proc_interface_cleanup(struct proc_interface *proc)
{
    if (!proc) {
        return;
    }
    memset(proc, 0, sizeof(*proc));
}

int proc_write_status(struct proc_interface *proc, const struct proc_status *s)
{
    FILE *f;

    if (!proc || !proc->initialized || !s) {
        return -1;
    }

    f = proc_open(proc, "status");
    if (!f) {
        return -1;
    }

    fprintf(f, "uptime_sec: %llu\n",  (unsigned long long)s->uptime_sec);
    fprintf(f, "fw_version: %s\n",    s->fw_version);
    fprintf(f, "state: %s\n",         s->state);

    fclose(f);
    return 0;
}

int proc_write_perf_counters(struct proc_interface *proc, const struct proc_perf_counters *p)
{
    FILE *f;

    if (!proc || !proc->initialized || !p) {
        return -1;
    }

    f = proc_open(proc, "perf_counters");
    if (!f) {
        return -1;
    }

    fprintf(f, "read_iops: %llu\n",       (unsigned long long)p->read_iops);
    fprintf(f, "write_iops: %llu\n",      (unsigned long long)p->write_iops);
    fprintf(f, "read_bw_mbps: %llu\n",    (unsigned long long)p->read_bw_mbps);
    fprintf(f, "write_bw_mbps: %llu\n",   (unsigned long long)p->write_bw_mbps);
    fprintf(f, "read_lat_p99_us: %llu\n", (unsigned long long)p->read_lat_p99_us);
    fprintf(f, "write_lat_p99_us: %llu\n",(unsigned long long)p->write_lat_p99_us);

    fclose(f);
    return 0;
}

int proc_write_ftl_stats(struct proc_interface *proc, const struct proc_ftl_stats *f_stats)
{
    FILE *f;

    if (!proc || !proc->initialized || !f_stats) {
        return -1;
    }

    f = proc_open(proc, "ftl_stats");
    if (!f) {
        return -1;
    }

    fprintf(f, "l2p_hit_rate_pct: %u\n",   f_stats->l2p_hit_rate_pct);
    fprintf(f, "gc_count: %llu\n",          (unsigned long long)f_stats->gc_count);
    fprintf(f, "waf: %.2f\n",               f_stats->waf);
    fprintf(f, "avg_erase_count: %llu\n",   (unsigned long long)f_stats->avg_erase_count);
    fprintf(f, "max_erase_count: %llu\n",   (unsigned long long)f_stats->max_erase_count);

    fclose(f);
    return 0;
}
