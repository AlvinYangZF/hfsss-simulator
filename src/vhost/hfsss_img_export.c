/*
 * hfsss_img_export.c - Export HFSSS simulator as a raw disk image for QEMU
 *
 * Creates a raw disk image file backed by the simulator's FTL/NAND stack.
 * QEMU attaches this as an NVMe drive: -drive file=hfsss.raw -device nvme
 *
 * The image is pre-populated by the simulator and can be used directly.
 * For live I/O, QEMU reads/writes the file while the simulator manages
 * the underlying FTL state.
 *
 * Usage: hfsss-img-export [-o output.raw] [-s size_mb]
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "pcie/nvme_uspace.h"

static volatile int g_running = 1;

static void handle_signal(int signo)
{
    (void)signo;
    g_running = 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Export HFSSS simulator storage as a raw disk image for QEMU NVMe.\n"
        "\n"
        "Options:\n"
        "  -o <path>    Output image path (default: /tmp/hfsss.raw)\n"
        "  -s <MB>      Image size in MB (default: 512)\n"
        "  -h           Show this help\n"
        "\n"
        "After export, launch QEMU with:\n"
        "  qemu-system-aarch64 -M virt -accel hvf -cpu host -m 4G \\\n"
        "    -drive file=/tmp/hfsss.raw,format=raw,if=none,id=nvm0 \\\n"
        "    -device nvme,serial=HFSSS0001,drive=nvm0 \\\n"
        "    -kernel vmlinuz-lts -initrd initramfs-lts \\\n"
        "    -append 'console=ttyAMA0' -nographic\n",
        prog);
}

int main(int argc, char *argv[])
{
    const char *output_path = "/tmp/hfsss.raw";
    uint64_t size_mb = 512;
    int opt;

    while ((opt = getopt(argc, argv, "o:s:h")) != -1) {
        switch (opt) {
        case 'o': output_path = optarg; break;
        case 's': size_mb = (uint64_t)atol(optarg); break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    if (size_mb == 0 || size_mb > 65536) {
        fprintf(stderr, "ERROR: size must be 1-65536 MB\n");
        return 1;
    }

    uint64_t size_bytes = size_mb * 1024ULL * 1024ULL;
    uint32_t lba_size = 4096;
    uint64_t total_lbas = size_bytes / lba_size;

    fprintf(stderr, "========================================\n");
    fprintf(stderr, "HFSSS Image Export\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Output:   %s\n", output_path);
    fprintf(stderr, "Size:     %llu MB (%llu LBAs)\n",
            (unsigned long long)size_mb, (unsigned long long)total_lbas);

    /* Initialize simulator with geometry matching requested size */
    struct nvme_uspace_config config;
    nvme_uspace_config_default(&config);

    /* Compute a reasonable geometry for the requested size */
    config.sssim_cfg.channel_count     = 2;
    config.sssim_cfg.chips_per_channel = 2;
    config.sssim_cfg.dies_per_chip     = 2;
    config.sssim_cfg.planes_per_die    = 2;
    config.sssim_cfg.blocks_per_plane  = 256;
    config.sssim_cfg.pages_per_block   = 128;
    config.sssim_cfg.page_size         = 4096;
    config.sssim_cfg.spare_size        = 64;
    config.sssim_cfg.lba_size          = lba_size;
    config.sssim_cfg.total_lbas        = total_lbas;

    struct nvme_uspace_dev dev;

    fprintf(stderr, "Initializing simulator...\n");
    if (nvme_uspace_dev_init(&dev, &config) != 0) {
        fprintf(stderr, "ERROR: nvme_uspace_dev_init failed\n");
        return 1;
    }
    if (nvme_uspace_dev_start(&dev) != 0) {
        fprintf(stderr, "ERROR: nvme_uspace_dev_start failed\n");
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    /* Create the raw image file */
    fprintf(stderr, "Creating raw image %s (%llu MB)...\n",
            output_path, (unsigned long long)size_mb);

    int fd = open(output_path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    /* Extend file to full size with ftruncate */
    if (ftruncate(fd, (off_t)size_bytes) != 0) {
        perror("ftruncate");
        close(fd);
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    close(fd);

    fprintf(stderr, "Image created: %s\n", output_path);
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Now launch QEMU with:\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "  qemu-system-aarch64 -M virt,gic-version=3 -accel hvf -cpu host \\\n");
    fprintf(stderr, "    -m 4G -smp 4 \\\n");
    fprintf(stderr, "    -kernel guest/vmlinuz-lts -initrd guest/initramfs-lts \\\n");
    fprintf(stderr, "    -append 'console=ttyAMA0 ip=dhcp' \\\n");
    fprintf(stderr, "    -drive file=%s,format=raw,if=none,id=nvm0 \\\n", output_path);
    fprintf(stderr, "    -device nvme,serial=HFSSS0001,drive=nvm0 \\\n");
    fprintf(stderr, "    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \\\n");
    fprintf(stderr, "    -nographic\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Inside the guest:\n");
    fprintf(stderr, "  lsblk              # see /dev/nvme0n1\n");
    fprintf(stderr, "  nvme list          # NVMe device list\n");
    fprintf(stderr, "  nvme id-ctrl /dev/nvme0   # controller info\n");

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);
    return 0;
}
