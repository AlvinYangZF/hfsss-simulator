#ifndef __HFSSS_PROC_INTERFACE_H
#define __HFSSS_PROC_INTERFACE_H

#include <stdint.h>

#define PROC_DIR_DEFAULT "/tmp/hfsss/proc"
#define PROC_PATH_MAX 512
#define PROC_STR_MAX  64

struct proc_interface {
    char dir[PROC_PATH_MAX];
    int  initialized;
};

struct proc_status {
    uint64_t uptime_sec;
    char     fw_version[PROC_STR_MAX];
    char     state[PROC_STR_MAX];
};

struct proc_perf_counters {
    uint64_t read_iops;
    uint64_t write_iops;
    uint64_t read_bw_mbps;
    uint64_t write_bw_mbps;
    uint64_t read_lat_p99_us;
    uint64_t write_lat_p99_us;
};

struct proc_ftl_stats {
    uint32_t l2p_hit_rate_pct;
    uint64_t gc_count;
    double   waf;
    uint64_t avg_erase_count;
    uint64_t max_erase_count;
};

int  proc_interface_init(struct proc_interface *proc, const char *dir);
void proc_interface_cleanup(struct proc_interface *proc);
int  proc_write_status(struct proc_interface *proc, const struct proc_status *s);
int  proc_write_perf_counters(struct proc_interface *proc, const struct proc_perf_counters *p);
int  proc_write_ftl_stats(struct proc_interface *proc, const struct proc_ftl_stats *f);

#endif /* __HFSSS_PROC_INTERFACE_H */
