/*
 * hfsss_nbd_server.c - NBD (Network Block Device) server for HFSSS simulator
 *
 * Exposes the HFSSS FTL/NAND simulator as a live NBD block device so that
 * QEMU can use it as a backend.  Every read/write/flush/trim request goes
 * through the full FTL stack.
 *
 * NBD new-style fixed handshake is used.  Supported options:
 *   NBD_OPT_EXPORT_NAME  - export the device under any name
 *   NBD_OPT_ABORT        - clean abort during negotiation
 *
 * Usage:
 *   hfsss-nbd-server [-p port] [-s size_mb]
 *
 * QEMU invocation (after the server is up):
 *   qemu-system-aarch64 ... \
 *     -drive driver=nbd,host=127.0.0.1,port=10809,id=nvm0 \
 *     -device nvme,serial=HFSSS0001,drive=nvm0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "pcie/nvme_uspace.h"

/* -------------------------------------------------------------------------
 * Portable 64-bit byte-swap helpers
 *
 * macOS (since 10.9) already provides htonll()/ntohll() in <arpa/inet.h>,
 * so we only define our own versions when the system does not supply them.
 * ---------------------------------------------------------------------- */

#ifndef htonll
static inline uint64_t hfsss_htonll(uint64_t v)
{
    union { uint64_t u64; uint32_t u32[2]; } s;
    s.u32[0] = htonl((uint32_t)(v >> 32));
    s.u32[1] = htonl((uint32_t)(v & 0xffffffffU));
    return s.u64;
}
#define htonll hfsss_htonll
#define ntohll hfsss_htonll   /* byte-swap is self-inverse */
#endif

/* -------------------------------------------------------------------------
 * NBD protocol constants
 * ---------------------------------------------------------------------- */

/* Handshake magic numbers */
#define NBD_MAGIC           UINT64_C(0x4e42444d41474943)  /* "NBDMAGIC" */
#define NBD_IHAVEOPT        UINT64_C(0x49484156454f5054)  /* "IHAVEOPT" */
#define NBD_OPT_REPLY_MAGIC UINT64_C(0x3e889045565a9)    /* option reply */

/* Handshake flags (server -> client) */
#define NBD_FLAG_FIXED_NEWSTYLE (1u << 0)
#define NBD_FLAG_NO_ZEROES      (1u << 1)

/* Transmission flags sent with export info */
#define NBD_FLAG_HAS_FLAGS      (1u << 0)
#define NBD_FLAG_SEND_FLUSH     (1u << 2)
#define NBD_FLAG_SEND_TRIM      (1u << 5)
#define NBD_TRANSMISSION_FLAGS  (NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_TRIM)

/* Option types (client -> server) */
#define NBD_OPT_EXPORT_NAME  1u
#define NBD_OPT_ABORT        2u

/* Reply types (server -> client, for NBD_OPT_GO etc.) */
#define NBD_REP_ACK     1u
#define NBD_REP_ERR_UNSUP  (UINT32_C(0x80000001))

/* Transmission request magic */
#define NBD_REQUEST_MAGIC  0x25609513u
#define NBD_REPLY_MAGIC    0x67446698u

/* Command types */
#define NBD_CMD_READ   0u
#define NBD_CMD_WRITE  1u
#define NBD_CMD_DISC   2u
#define NBD_CMD_FLUSH  3u
#define NBD_CMD_TRIM   4u

/* NBD error codes (errno-compatible subset) */
#define NBD_EIO   5u
#define NBD_EINVAL 22u

/* -------------------------------------------------------------------------
 * Wire structures (packed, network byte order)
 * ---------------------------------------------------------------------- */

/* Request from client during transmission phase (28 bytes) */
struct __attribute__((packed)) nbd_request {
    uint32_t magic;     /* NBD_REQUEST_MAGIC */
    uint16_t flags;
    uint16_t type;      /* NBD_CMD_* */
    uint64_t handle;    /* opaque, echo back */
    uint64_t offset;    /* byte offset */
    uint32_t length;    /* byte count */
};

/* Reply from server during transmission phase (16 bytes) */
struct __attribute__((packed)) nbd_reply {
    uint32_t magic;     /* NBD_REPLY_MAGIC */
    uint32_t error;     /* 0 = success, else errno */
    uint64_t handle;    /* from request */
};

/* -------------------------------------------------------------------------
 * Global state for signal handler
 * ---------------------------------------------------------------------- */

static volatile int g_running = 1;
static int          g_listen_fd = -1;

static void handle_signal(int signo)
{
    (void)signo;
    g_running = 0;
    /* Interrupt accept() */
    if (g_listen_fd >= 0)
        close(g_listen_fd);
}

/* -------------------------------------------------------------------------
 * I/O helpers: read/write exact byte counts
 * ---------------------------------------------------------------------- */

static int read_exact(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int write_exact(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Send a transmission reply (error=0 means success)
 * ---------------------------------------------------------------------- */

static int send_reply(int fd, uint64_t handle, uint32_t error)
{
    struct nbd_reply rep;
    rep.magic  = htonl(NBD_REPLY_MAGIC);
    rep.error  = htonl(error);
    rep.handle = handle;          /* handle already in wire byte order */
    return write_exact(fd, &rep, sizeof(rep));
}

/* -------------------------------------------------------------------------
 * NBD handshake: newstyle fixed
 *
 * Returns 0 on success (client ready for transmission).
 * Returns -1 on error or if client sent NBD_OPT_ABORT.
 * ---------------------------------------------------------------------- */

static int nbd_handshake(int client_fd, uint64_t export_size)
{
    /* --- Server sends NBDMAGIC + IHAVEOPT + handshake_flags ----------- */
    uint64_t magic_wire    = htonll(NBD_MAGIC);
    uint64_t ihaveopt_wire = htonll(NBD_IHAVEOPT);
    uint16_t hflags_wire   = htons(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);

    if (write_exact(client_fd, &magic_wire,    sizeof(magic_wire))    != 0 ||
        write_exact(client_fd, &ihaveopt_wire, sizeof(ihaveopt_wire)) != 0 ||
        write_exact(client_fd, &hflags_wire,   sizeof(hflags_wire))   != 0)
        return -1;

    /* --- Client sends client_flags (4 bytes) -------------------------- */
    uint32_t client_flags_wire;
    if (read_exact(client_fd, &client_flags_wire, sizeof(client_flags_wire)) != 0)
        return -1;
    /* We accept whatever flags the client claims. */

    /* --- Option negotiation loop -------------------------------------- */
    for (;;) {
        /* Client option header: IHAVEOPT (8) + option (4) + length (4) */
        uint64_t opt_magic_wire;
        uint32_t opt_type_wire;
        uint32_t opt_len_wire;

        if (read_exact(client_fd, &opt_magic_wire, sizeof(opt_magic_wire)) != 0 ||
            read_exact(client_fd, &opt_type_wire,  sizeof(opt_type_wire))  != 0 ||
            read_exact(client_fd, &opt_len_wire,   sizeof(opt_len_wire))   != 0)
            return -1;

        uint32_t opt_type = ntohl(opt_type_wire);
        uint32_t opt_len  = ntohl(opt_len_wire);

        /* Drain any option data (we don't care about the export name) */
        if (opt_len > 0) {
            /* Sanity-cap to avoid huge mallocs from a misbehaving client */
            if (opt_len > 4096) {
                fprintf(stderr, "[NBD] Option data too large (%u), aborting\n", opt_len);
                return -1;
            }
            uint8_t scratch[4096];
            if (read_exact(client_fd, scratch, opt_len) != 0)
                return -1;
        }

        if (opt_type == NBD_OPT_ABORT) {
            fprintf(stderr, "[NBD] Client sent NBD_OPT_ABORT\n");
            return -1;
        }

        if (opt_type == NBD_OPT_EXPORT_NAME) {
            /*
             * Reply: export_size (8) + transmission_flags (2)
             * Because we set NO_ZEROES in handshake_flags we do NOT
             * send the trailing 124 zero bytes.
             */
            uint64_t esz_wire   = htonll(export_size);
            uint16_t tflags_wire = htons((uint16_t)NBD_TRANSMISSION_FLAGS);

            if (write_exact(client_fd, &esz_wire,    sizeof(esz_wire))    != 0 ||
                write_exact(client_fd, &tflags_wire, sizeof(tflags_wire)) != 0)
                return -1;

            /* Handshake done — enter transmission phase */
            return 0;
        }

        /* Unknown option: send NBD_REP_ERR_UNSUP and keep looping */
        {
            uint64_t reply_magic_wire = htonll(NBD_OPT_REPLY_MAGIC);
            uint32_t opt_echo_wire    = opt_type_wire;   /* already big-endian */
            uint32_t rep_type_wire    = htonl(NBD_REP_ERR_UNSUP);
            uint32_t rep_len_wire     = htonl(0);

            if (write_exact(client_fd, &reply_magic_wire, sizeof(reply_magic_wire)) != 0 ||
                write_exact(client_fd, &opt_echo_wire,    sizeof(opt_echo_wire))    != 0 ||
                write_exact(client_fd, &rep_type_wire,    sizeof(rep_type_wire))    != 0 ||
                write_exact(client_fd, &rep_len_wire,     sizeof(rep_len_wire))     != 0)
                return -1;
        }
    }
}

/* -------------------------------------------------------------------------
 * NBD transmission phase — serve I/O until disconnect or error
 * ---------------------------------------------------------------------- */

static void nbd_serve(int client_fd, struct nvme_uspace_dev *dev,
                      uint32_t lba_size)
{
    uint8_t *iobuf = NULL;
    size_t   iobuf_cap = 0;

    for (;;) {
        struct nbd_request req;
        if (read_exact(client_fd, &req, sizeof(req)) != 0) {
            if (errno != 0)
                fprintf(stderr, "[NBD] Read request failed: %s\n",
                        strerror(errno));
            break;
        }

        uint32_t magic  = ntohl(req.magic);
        uint16_t type   = ntohs(req.type);
        uint64_t offset = ntohll(req.offset);
        uint32_t length = ntohl(req.length);
        uint64_t handle = req.handle; /* echo verbatim */

        if (magic != NBD_REQUEST_MAGIC) {
            fprintf(stderr, "[NBD] Bad request magic 0x%08x\n", magic);
            break;
        }

        /* Compute LBA and count from byte offset/length.
         * The simulator works in lba_size units (4096B), but NBD clients
         * may request sub-page I/O (e.g. 512B).  We always read/write
         * full pages and slice/merge as needed. */
        uint64_t lba        = offset / lba_size;
        uint32_t byte_off   = (uint32_t)(offset % lba_size);  /* offset within first page */
        uint64_t end_byte   = offset + length;
        uint64_t end_lba    = (end_byte + lba_size - 1) / lba_size;
        uint32_t count      = (uint32_t)(end_lba - lba);
        uint32_t full_bytes = count * lba_size;

        /* Ensure iobuf is large enough for full-page I/O */
        if (full_bytes > iobuf_cap) {
            free(iobuf);
            iobuf = (uint8_t *)malloc(full_bytes);
            if (!iobuf) {
                fprintf(stderr, "[NBD] malloc(%u) failed\n", full_bytes);
                send_reply(client_fd, handle, NBD_EIO);
                goto done;
            }
            iobuf_cap = full_bytes;
        }

        switch (type) {

        /* ----------------------------------------------------------------
         * READ — read full pages, return only the requested byte range
         * ---------------------------------------------------------------- */
        case NBD_CMD_READ: {
            int rc = nvme_uspace_read(dev, 1, lba, count, iobuf);
            if (rc != 0) {
                /* Unwritten pages: return zeros instead of error */
                memset(iobuf, 0, full_bytes);
            }
            if (send_reply(client_fd, handle, 0) != 0)
                goto done;
            /* Return only the requested slice */
            if (write_exact(client_fd, iobuf + byte_off, length) != 0)
                goto done;
            break;
        }

        /* ----------------------------------------------------------------
         * WRITE — for sub-page writes, do read-modify-write
         * ---------------------------------------------------------------- */
        case NBD_CMD_WRITE: {
            /* Read client data into a temp buffer first */
            uint8_t *wbuf = (uint8_t *)malloc(length);
            if (!wbuf) {
                send_reply(client_fd, handle, NBD_EIO);
                goto done;
            }
            if (read_exact(client_fd, wbuf, length) != 0) {
                free(wbuf);
                goto done;
            }

            /* If sub-page or unaligned, do read-modify-write */
            if (byte_off != 0 || length != full_bytes) {
                /* Read existing pages */
                int rc = nvme_uspace_read(dev, 1, lba, count, iobuf);
                if (rc != 0)
                    memset(iobuf, 0, full_bytes);
                /* Overlay the write data */
                memcpy(iobuf + byte_off, wbuf, length);
                free(wbuf);
                wbuf = NULL;
            } else {
                /* Aligned write — use client data directly */
                memcpy(iobuf, wbuf, length);
                free(wbuf);
                wbuf = NULL;
            }

            int rc = nvme_uspace_write(dev, 1, lba, count, iobuf);
            if (rc != 0) {
                fprintf(stderr, "[NBD] write failed rc=%d\n", rc);
                if (send_reply(client_fd, handle, NBD_EIO) != 0)
                    goto done;
                break;
            }
            if (send_reply(client_fd, handle, 0) != 0)
                goto done;
            break;
        }

        /* ----------------------------------------------------------------
         * FLUSH
         * ---------------------------------------------------------------- */
        case NBD_CMD_FLUSH: {
            fprintf(stderr, "[NBD] FLUSH\n");

            int rc = nvme_uspace_flush(dev, 1);
            uint32_t err = (rc != 0) ? NBD_EIO : 0;
            if (send_reply(client_fd, handle, err) != 0)
                goto done;
            break;
        }

        /* ----------------------------------------------------------------
         * TRIM (discard)
         * ---------------------------------------------------------------- */
        case NBD_CMD_TRIM: {
            fprintf(stderr,
                    "[NBD] TRIM  offset=0x%016llx len=%u lba=%llu count=%u\n",
                    (unsigned long long)offset, length,
                    (unsigned long long)lba, count);

            struct nvme_dsm_range range;
            memset(&range, 0, sizeof(range));
            range.slba  = lba;
            range.nlb   = count;

            int rc = nvme_uspace_trim(dev, 1, &range, 1);
            uint32_t err = (rc != 0) ? NBD_EIO : 0;
            if (send_reply(client_fd, handle, err) != 0)
                goto done;
            break;
        }

        /* ----------------------------------------------------------------
         * DISCONNECT
         * ---------------------------------------------------------------- */
        case NBD_CMD_DISC:
            fprintf(stderr, "[NBD] Client disconnected cleanly\n");
            goto done;

        default:
            fprintf(stderr, "[NBD] Unknown command type %u\n", type);
            if (send_reply(client_fd, handle, NBD_EINVAL) != 0)
                goto done;
            break;
        }
    }

done:
    free(iobuf);
}

/* -------------------------------------------------------------------------
 * Usage
 * ---------------------------------------------------------------------- */

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Expose HFSSS simulator as an NBD block device for QEMU.\n"
        "\n"
        "Options:\n"
        "  -p <port>   TCP port to listen on (default: 10809)\n"
        "  -s <MB>     Export size in MB (default: 512)\n"
        "  -h          Show this help\n"
        "\n"
        "Connect QEMU with:\n"
        "  -drive driver=nbd,host=127.0.0.1,port=10809,id=nvm0\n"
        "  -device nvme,serial=HFSSS0001,drive=nvm0\n",
        prog);
}

/* -------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    uint16_t port    = 10809;
    uint64_t size_mb = 512;
    int opt;

    while ((opt = getopt(argc, argv, "p:s:h")) != -1) {
        switch (opt) {
        case 'p':
            port = (uint16_t)atoi(optarg);
            break;
        case 's':
            size_mb = (uint64_t)atol(optarg);
            if (size_mb == 0 || size_mb > 65536) {
                fprintf(stderr, "ERROR: size must be 1-65536 MB\n");
                return 1;
            }
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    const uint32_t lba_size   = 4096;
    const uint64_t export_size = size_mb * 1024ULL * 1024ULL;
    const uint64_t total_lbas  = export_size / lba_size;

    /* ------------------------------------------------------------------ */
    /* Initialize simulator                                                */
    /* ------------------------------------------------------------------ */
    struct nvme_uspace_config config;
    nvme_uspace_config_default(&config);

    config.sssim_cfg.channel_count     = 4;
    config.sssim_cfg.chips_per_channel = 4;
    config.sssim_cfg.dies_per_chip     = 2;
    config.sssim_cfg.planes_per_die    = 2;
    config.sssim_cfg.blocks_per_plane  = 512;
    config.sssim_cfg.pages_per_block   = 256;
    config.sssim_cfg.page_size         = 4096;
    config.sssim_cfg.spare_size        = 64;
    config.sssim_cfg.lba_size          = lba_size;
    config.sssim_cfg.total_lbas        = total_lbas;

    struct nvme_uspace_dev dev;

    fprintf(stderr, "========================================\n");
    fprintf(stderr, "HFSSS NBD Server\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:     %u\n", port);
    fprintf(stderr, "Size:     %llu MB (%llu LBAs x %u B)\n",
            (unsigned long long)size_mb,
            (unsigned long long)total_lbas, lba_size);
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

    /* ------------------------------------------------------------------ */
    /* Create listening TCP socket                                         */
    /* ------------------------------------------------------------------ */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    {
        int reuse = 1;
        setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(g_listen_fd);
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    if (listen(g_listen_fd, 1) != 0) {
        perror("listen");
        close(g_listen_fd);
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    /* ------------------------------------------------------------------ */
    /* Install signal handlers                                             */
    /* ------------------------------------------------------------------ */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    fprintf(stderr, "Listening on 0.0.0.0:%u — export size %llu MB\n",
            port, (unsigned long long)size_mb);
    fprintf(stderr, "Waiting for NBD client...\n");

    /* ------------------------------------------------------------------ */
    /* Accept loop (one client at a time)                                  */
    /* ------------------------------------------------------------------ */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        int client_fd = accept(g_listen_fd,
                               (struct sockaddr *)&client_addr,
                               &client_addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF) {
                /* Interrupted by signal or fd closed by handler */
                break;
            }
            perror("accept");
            break;
        }

        fprintf(stderr, "[NBD] Connected: %s:%u\n",
                inet_ntoa(client_addr.sin_addr),
                ntohs(client_addr.sin_port));

        if (nbd_handshake(client_fd, export_size) == 0) {
            fprintf(stderr, "[NBD] Handshake OK, entering transmission phase\n");
            nbd_serve(client_fd, &dev, lba_size);
        } else {
            fprintf(stderr, "[NBD] Handshake failed\n");
        }

        close(client_fd);
        fprintf(stderr, "[NBD] Client disconnected, waiting for next...\n");
    }

    /* ------------------------------------------------------------------ */
    /* Cleanup                                                             */
    /* ------------------------------------------------------------------ */
    if (g_listen_fd >= 0)
        close(g_listen_fd);

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);

    fprintf(stderr, "[NBD] Server exited cleanly\n");
    return 0;
}
