/*
 * hfsss_vhost_main.c - HFSSS vhost-user-blk server entry point
 *
 * Starts the HFSSS NVMe simulator as a vhost-user block device backend so
 * QEMU can attach it as a virtio-blk / vhost-user-blk-pci device.
 *
 * Usage: hfsss-vhost-blk [-s socket_path] [-h]
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>

#include "vhost/vhost_user_blk.h"
#include "pcie/nvme_uspace.h"

/* -------------------------------------------------------------------------
 * Globals for signal handler
 * ---------------------------------------------------------------------- */

static struct vhost_user_blk_server g_srv;

/* -------------------------------------------------------------------------
 * Signal handler
 * ---------------------------------------------------------------------- */

static void handle_signal(int signo)
{
    (void)signo;
    fprintf(stderr, "\n[hfsss-vhost] Caught signal %d, shutting down...\n", signo);
    vhost_blk_server_stop(&g_srv);
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Options:\n"
        "  -s <socket>   Unix socket path (default: /tmp/hfsss-vhost.sock)\n"
        "  -h            Show this help message\n"
        "\n"
        "Example:\n"
        "  %s -s /tmp/hfsss-vhost.sock\n",
        prog, prog);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    const char *socket_path = "/tmp/hfsss-vhost.sock";
    int opt;

    /* --- Parse CLI arguments ------------------------------------------- */
    while ((opt = getopt(argc, argv, "s:h")) != -1) {
        switch (opt) {
        case 's':
            socket_path = optarg;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    /* --- Startup banner ------------------------------------------------- */
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "HFSSS vhost-user-blk server starting on %s\n", socket_path);
    fprintf(stderr, "========================================\n");

    /* --- Initialise NVMe uspace device ---------------------------------- */
    struct nvme_uspace_config config;
    struct nvme_uspace_dev    dev;

    nvme_uspace_config_default(&config);

    /* Override to a practical geometry for vhost testing.
     * The full 4TB default allocates ~2GB for L2P alone.
     * Use a smaller profile that boots in seconds.
     * Users can override via config file later. */
    config.sssim_cfg.channel_count     = 4;
    config.sssim_cfg.chips_per_channel = 4;
    config.sssim_cfg.dies_per_chip     = 2;
    config.sssim_cfg.planes_per_die    = 2;
    config.sssim_cfg.blocks_per_plane  = 512;
    config.sssim_cfg.pages_per_block   = 256;
    config.sssim_cfg.page_size         = 4096;
    config.sssim_cfg.spare_size        = 64;
    /* 4*4*2*2*512*256 = 16M pages * 4KB = 64GB raw, ~60GB user */
    config.sssim_cfg.total_lbas = 4ULL * 4 * 2 * 2 * 512 * 256;

    fprintf(stderr, "NAND: %uch/%uchip/%udie/%uplane, %u blk, %u pg, %u B/pg\n",
            config.sssim_cfg.channel_count, config.sssim_cfg.chips_per_channel,
            config.sssim_cfg.dies_per_chip, config.sssim_cfg.planes_per_die,
            config.sssim_cfg.blocks_per_plane, config.sssim_cfg.pages_per_block,
            config.sssim_cfg.page_size);
    fprintf(stderr, "Capacity: %llu LBAs (~%llu GB)\n",
            (unsigned long long)config.sssim_cfg.total_lbas,
            (unsigned long long)(config.sssim_cfg.total_lbas * config.sssim_cfg.lba_size
                                 / (1024ULL * 1024 * 1024)));

    if (nvme_uspace_dev_init(&dev, &config) != 0) {
        fprintf(stderr, "[ERROR] nvme_uspace_dev_init failed\n");
        return 1;
    }

    if (nvme_uspace_dev_start(&dev) != 0) {
        fprintf(stderr, "[ERROR] nvme_uspace_dev_start failed\n");
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    /* --- Initialise vhost-user-blk server ------------------------------- */
    if (vhost_blk_server_init(&g_srv, socket_path, &dev) != 0) {
        fprintf(stderr, "[ERROR] vhost_blk_server_init failed\n");
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    /* --- Install signal handlers ---------------------------------------- */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "Waiting for QEMU connection...\n");

    /* --- Run (blocks until stopped) ------------------------------------ */
    int rc = vhost_blk_server_run(&g_srv);

    /* --- Cleanup ------------------------------------------------------- */
    vhost_blk_server_cleanup(&g_srv);
    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);

    fprintf(stderr, "[hfsss-vhost] Server exited (rc=%d)\n", rc);
    return (rc == 0) ? 0 : 1;
}
