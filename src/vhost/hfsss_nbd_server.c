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
#include <inttypes.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "media/nand_profile.h"
#include "pcie/nvme_uspace.h"
#include "ftl/ftl_worker.h"
#include "vhost/nbd_async.h"
#include "common/trace.h"

/* -------------------------------------------------------------------------
 * Portable 64-bit byte-swap helpers
 *
 * macOS (since 10.9) already provides htonll()/ntohll() in <arpa/inet.h>,
 * so we only define our own versions when the system does not supply them.
 * ---------------------------------------------------------------------- */

#ifndef htonll
static inline uint64_t hfsss_htonll(uint64_t v)
{
    union {
        uint64_t u64;
        uint32_t u32[2];
    } s;
    s.u32[0] = htonl((uint32_t)(v >> 32));
    s.u32[1] = htonl((uint32_t)(v & 0xffffffffU));
    return s.u64;
}
#define htonll hfsss_htonll
#define ntohll hfsss_htonll /* byte-swap is self-inverse */
#endif

/* -------------------------------------------------------------------------
 * NBD protocol constants
 * ---------------------------------------------------------------------- */

/* Handshake magic numbers */
#define NBD_MAGIC UINT64_C(0x4e42444d41474943)        /* "NBDMAGIC" */
#define NBD_IHAVEOPT UINT64_C(0x49484156454f5054)     /* "IHAVEOPT" */
#define NBD_OPT_REPLY_MAGIC UINT64_C(0x3e889045565a9) /* option reply */

/* Handshake flags (server -> client) */
#define NBD_FLAG_FIXED_NEWSTYLE (1u << 0)
#define NBD_FLAG_NO_ZEROES (1u << 1)

/* Transmission flags sent with export info */
#define NBD_FLAG_HAS_FLAGS (1u << 0)
#define NBD_FLAG_SEND_FLUSH (1u << 2)
#define NBD_FLAG_SEND_TRIM (1u << 5)
#define NBD_TRANSMISSION_FLAGS (NBD_FLAG_HAS_FLAGS | NBD_FLAG_SEND_FLUSH | NBD_FLAG_SEND_TRIM)

/* Option types (client -> server) */
#define NBD_OPT_EXPORT_NAME 1u
#define NBD_OPT_ABORT 2u

/* Reply types (server -> client, for NBD_OPT_GO etc.) */
#define NBD_REP_ACK 1u
#define NBD_REP_ERR_UNSUP (UINT32_C(0x80000001))

/* Transmission request magic */
#define NBD_REQUEST_MAGIC 0x25609513u
#define NBD_REPLY_MAGIC 0x67446698u

/* Command types */
#define NBD_CMD_READ 0u
#define NBD_CMD_WRITE 1u
#define NBD_CMD_DISC 2u
#define NBD_CMD_FLUSH 3u
#define NBD_CMD_TRIM 4u

/* Verbose I/O logging (set via -v flag) */
static int g_verbose = 0;

/* Multi-threaded mode (set via -m flag) */
static int g_multithread = 0;
static int g_async = 0;
static struct ftl_mt_ctx *g_mt = NULL;

/* NBD error codes (errno-compatible subset) */
#define NBD_EIO 5u
#define NBD_EINVAL 22u

/* -------------------------------------------------------------------------
 * Wire structures (packed, network byte order)
 * ---------------------------------------------------------------------- */

/* Request from client during transmission phase (28 bytes) */
struct __attribute__((packed)) nbd_request {
    uint32_t magic; /* NBD_REQUEST_MAGIC */
    uint16_t flags;
    uint16_t type;   /* NBD_CMD_* */
    uint64_t handle; /* opaque, echo back */
    uint64_t offset; /* byte offset */
    uint32_t length; /* byte count */
};

/* Reply from server during transmission phase (16 bytes) */
struct __attribute__((packed)) nbd_reply {
    uint32_t magic;  /* NBD_REPLY_MAGIC */
    uint32_t error;  /* 0 = success, else errno */
    uint64_t handle; /* from request */
};

/* -------------------------------------------------------------------------
 * Global state for signal handler
 * ---------------------------------------------------------------------- */

static volatile int g_running = 1;
static int g_listen_fd = -1;

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
        p += (size_t)n;
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
        p += (size_t)n;
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
    rep.magic = htonl(NBD_REPLY_MAGIC);
    rep.error = htonl(error);
    rep.handle = handle; /* handle already in wire byte order */
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
    uint64_t magic_wire = htonll(NBD_MAGIC);
    uint64_t ihaveopt_wire = htonll(NBD_IHAVEOPT);
    uint16_t hflags_wire = htons(NBD_FLAG_FIXED_NEWSTYLE | NBD_FLAG_NO_ZEROES);

    if (write_exact(client_fd, &magic_wire, sizeof(magic_wire)) != 0 ||
        write_exact(client_fd, &ihaveopt_wire, sizeof(ihaveopt_wire)) != 0 ||
        write_exact(client_fd, &hflags_wire, sizeof(hflags_wire)) != 0)
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
            read_exact(client_fd, &opt_type_wire, sizeof(opt_type_wire)) != 0 ||
            read_exact(client_fd, &opt_len_wire, sizeof(opt_len_wire)) != 0)
            return -1;

        uint32_t opt_type = ntohl(opt_type_wire);
        uint32_t opt_len = ntohl(opt_len_wire);

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
            uint64_t esz_wire = htonll(export_size);
            uint16_t tflags_wire = htons((uint16_t)NBD_TRANSMISSION_FLAGS);

            if (write_exact(client_fd, &esz_wire, sizeof(esz_wire)) != 0 ||
                write_exact(client_fd, &tflags_wire, sizeof(tflags_wire)) != 0)
                return -1;

            /* Handshake done — enter transmission phase */
            return 0;
        }

        /* Unknown option: send NBD_REP_ERR_UNSUP and keep looping */
        {
            uint64_t reply_magic_wire = htonll(NBD_OPT_REPLY_MAGIC);
            uint32_t opt_echo_wire = opt_type_wire; /* already big-endian */
            uint32_t rep_type_wire = htonl(NBD_REP_ERR_UNSUP);
            uint32_t rep_len_wire = htonl(0);

            if (write_exact(client_fd, &reply_magic_wire, sizeof(reply_magic_wire)) != 0 ||
                write_exact(client_fd, &opt_echo_wire, sizeof(opt_echo_wire)) != 0 ||
                write_exact(client_fd, &rep_type_wire, sizeof(rep_type_wire)) != 0 ||
                write_exact(client_fd, &rep_len_wire, sizeof(rep_len_wire)) != 0)
                return -1;
        }
    }
}

/* -------------------------------------------------------------------------
 * Multi-threaded I/O helpers — submit to worker and wait for completion
 * ---------------------------------------------------------------------- */

static int mt_io(enum io_opcode op, uint64_t lba, uint32_t count, uint8_t *data,
                 uint32_t lba_size)
{
    struct io_request req;
    memset(&req, 0, sizeof(req));
    req.opcode = op;
    req.lba = lba;
    req.count = count;
    req.data = data;
    req.nbd_handle = 0;

#ifdef HFSSS_DEBUG_TRACE
    {
        size_t crc_len = (size_t)count * (size_t)lba_size;
        uint32_t crc = (op == IO_OP_WRITE && data != NULL)
                       ? trace_crc32c(data, crc_len)
                       : 0;
        TRACE_EMIT(TRACE_POINT_T1_NBD_RECV, (uint32_t)op, lba,
                   (uint64_t)count, crc, 0);
    }
#else
    (void)lba_size;
#endif

    while (!ftl_mt_submit(g_mt, &req)) {
        sched_yield();
    }

    struct io_completion cpl;
    while (!ftl_mt_poll_completion(g_mt, &cpl)) {
        sched_yield();
    }
    return cpl.status;
}

/* -------------------------------------------------------------------------
 * Build an NVMe SQE from NBD request parameters.
 * This allows the NBD server to route I/O through the full NVMe command
 * processing path, exercising nvme_ctrl_process_io_cmd() end-to-end.
 * ---------------------------------------------------------------------- */

static void build_nvme_io_sqe(struct nvme_sq_entry *sqe, uint8_t opcode, uint32_t nsid, uint64_t slba, uint32_t nlb)
{
    memset(sqe, 0, sizeof(*sqe));
    sqe->opcode = opcode;
    sqe->nsid = nsid;
    sqe->cdw10 = (uint32_t)(slba & 0xFFFFFFFF);
    sqe->cdw11 = (uint32_t)(slba >> 32);
    sqe->cdw12 = (nlb > 0) ? (nlb - 1) : 0; /* NLB is 0-based */
}

/* -------------------------------------------------------------------------
 * NBD transmission phase — serve I/O until disconnect or error
 *
 * Routes all I/O through nvme_uspace_dispatch_io_cmd() to exercise the
 * full NVMe command processing path (SQE → opcode validation → uspace
 * dispatch → FTL → CQE), except in MT mode which must use the worker
 * pool for TAA shard cache coherency.
 * ---------------------------------------------------------------------- */

static void nbd_serve(int client_fd, struct nvme_uspace_dev *dev, uint32_t lba_size)
{
    uint8_t *iobuf = NULL;
    size_t iobuf_cap = 0;
    static uint16_t cmd_id = 0;

    for (;;) {
        struct nbd_request req;
        if (read_exact(client_fd, &req, sizeof(req)) != 0) {
            if (errno != 0)
                fprintf(stderr, "[NBD] Read request failed: %s\n", strerror(errno));
            break;
        }

        uint32_t magic = ntohl(req.magic);
        uint16_t type = ntohs(req.type);
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
        uint64_t lba = offset / lba_size;
        uint32_t byte_off = (uint32_t)(offset % lba_size); /* offset within first page */
        uint64_t end_byte = offset + length;
        uint64_t end_lba = (end_byte + lba_size - 1) / lba_size;
        uint64_t count64 = end_lba - lba;
        /* `count` is passed to the FTL as u32; reject requests that would
         * overflow that — NBD allows up to 4 GiB per request, the simulator
         * capacity is much smaller than that so a single overflowing request
         * is a client bug or misconfig worth failing fast. */
        if (count64 > (uint64_t)UINT32_MAX) {
            fprintf(stderr, "[NBD] request too large: count=%llu\n",
                    (unsigned long long)count64);
            send_reply(client_fd, handle, NBD_EIO);
            goto done;
        }
        uint32_t count = (uint32_t)count64;
        /* Buffer size must use 64-bit math: count * lba_size can exceed
         * UINT32_MAX for large multi-LBA requests (e.g. count≈1M at 4K
         * wraps a u32 -> 0), which previously caused a truncated/zero-sized
         * malloc and downstream garbage reads/writes. */
        uint64_t full_bytes64 = (uint64_t)count * (uint64_t)lba_size;
        if (full_bytes64 > (uint64_t)SIZE_MAX) {
            fprintf(stderr, "[NBD] full_bytes overflow: %llu\n",
                    (unsigned long long)full_bytes64);
            send_reply(client_fd, handle, NBD_EIO);
            goto done;
        }
        size_t full_bytes = (size_t)full_bytes64;

        /* Ensure iobuf is large enough for full-page I/O */
        if (full_bytes > iobuf_cap) {
            free(iobuf);
            iobuf = (uint8_t *)malloc(full_bytes);
            if (!iobuf) {
                fprintf(stderr, "[NBD] malloc(%zu) failed\n", full_bytes);
                send_reply(client_fd, handle, NBD_EIO);
                goto done;
            }
            iobuf_cap = full_bytes;
        }

        switch (type) {

        /* ----------------------------------------------------------------
         * READ — route through NVMe dispatch for full-path coverage
         * ---------------------------------------------------------------- */
        case NBD_CMD_READ: {
            int rc;
            if (g_multithread) {
                rc = mt_io(IO_OP_READ, lba, count, iobuf, lba_size);
            } else {
                struct nvme_sq_entry sqe;
                struct nvme_cq_entry cqe;
                build_nvme_io_sqe(&sqe, NVME_NVM_READ, 1, lba, count);
                sqe.command_id = cmd_id++;
                rc = nvme_uspace_dispatch_io_cmd(dev, &sqe, &cqe, iobuf, full_bytes);
                if (rc == 0 && cqe.status != 0)
                    rc = -1;
            }
            if (rc != 0) {
                /* Unwritten pages: return zeros instead of error */
                memset(iobuf, 0, full_bytes);
            }
            if (g_verbose)
                fprintf(stderr, "[NBD] READ  off=%-10" PRIu64 " len=%-6u lba=%-8" PRIu64 " cnt=%-4u rc=%d\n", offset,
                        length, lba, count, rc);
            if (send_reply(client_fd, handle, 0) != 0)
                goto done;
            /* Return only the requested slice */
            if (write_exact(client_fd, iobuf + byte_off, length) != 0)
                goto done;
            break;
        }

        /* ----------------------------------------------------------------
         * WRITE — route through NVMe dispatch for full-path coverage
         * ---------------------------------------------------------------- */
        case NBD_CMD_WRITE: {
            /* Read client write data directly into iobuf */
            if (read_exact(client_fd, iobuf, length) != 0) {
                goto done;
            }

            /* If sub-page or unaligned, do read-modify-write in place */
            if (byte_off != 0 || length != full_bytes) {
                /* Save the partial write data */
                uint8_t *partial = (uint8_t *)malloc(length);
                if (!partial) {
                    send_reply(client_fd, handle, NBD_EIO);
                    break;
                }
                memcpy(partial, iobuf, length);

                /* Read existing full pages */
                int rc;
                if (g_multithread) {
                    rc = mt_io(IO_OP_READ, lba, count, iobuf, lba_size);
                } else {
                    struct nvme_sq_entry sqe;
                    struct nvme_cq_entry cqe;
                    build_nvme_io_sqe(&sqe, NVME_NVM_READ, 1, lba, count);
                    sqe.command_id = cmd_id++;
                    rc = nvme_uspace_dispatch_io_cmd(dev, &sqe, &cqe, iobuf, full_bytes);
                    if (rc == 0 && cqe.status != 0)
                        rc = -1;
                }
                if (rc != 0)
                    memset(iobuf, 0, full_bytes);

                /* Overlay the partial write data at the correct offset */
                memcpy(iobuf + byte_off, partial, length);
                free(partial);
            }

            int rc;
            if (g_multithread) {
                rc = mt_io(IO_OP_WRITE, lba, count, iobuf, lba_size);
            } else {
                struct nvme_sq_entry sqe;
                struct nvme_cq_entry cqe;
                build_nvme_io_sqe(&sqe, NVME_NVM_WRITE, 1, lba, count);
                sqe.command_id = cmd_id++;
                rc = nvme_uspace_dispatch_io_cmd(dev, &sqe, &cqe, iobuf, full_bytes);
                if (rc == 0 && cqe.status != 0)
                    rc = -1;
            }
            if (g_verbose)
                fprintf(stderr, "[NBD] WRITE off=%-10" PRIu64 " len=%-6u lba=%-8" PRIu64 " cnt=%-4u rc=%d\n", offset,
                        length, lba, count, rc);
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
         * FLUSH — route through NVMe dispatch
         * ---------------------------------------------------------------- */
        case NBD_CMD_FLUSH: {
            fprintf(stderr, "[NBD] FLUSH\n");

            int rc;
            if (g_multithread) {
                /* MT mode: flush goes through uspace directly since
                 * there is no per-shard state for flush. */
                rc = nvme_uspace_flush(dev, 1);
            } else {
                struct nvme_sq_entry sqe;
                struct nvme_cq_entry cqe;
                build_nvme_io_sqe(&sqe, NVME_NVM_FLUSH, 1, 0, 0);
                sqe.command_id = cmd_id++;
                rc = nvme_uspace_dispatch_io_cmd(dev, &sqe, &cqe, NULL, 0);
                if (rc == 0 && cqe.status != 0)
                    rc = -1;
            }
            uint32_t err = (rc != 0) ? NBD_EIO : 0;
            if (send_reply(client_fd, handle, err) != 0)
                goto done;
            break;
        }

        /* ----------------------------------------------------------------
         * TRIM (discard) — route through NVMe dispatch
         * ---------------------------------------------------------------- */
        case NBD_CMD_TRIM: {
            fprintf(stderr, "[NBD] TRIM  offset=0x%016llx len=%u lba=%llu count=%u\n", (unsigned long long)offset,
                    length, (unsigned long long)lba, count);

            /* In MT mode, READ/WRITE traverse the TAA shard cache via
             * mt_io(), so TRIM must do the same — otherwise trim mutates
             * only the global L2P mapping while TAA shards keep the old
             * PPN, and subsequent reads return stale NAND contents
             * (dlfeat=1 zero-after-trim contract silently broken).
             * The async path already does this via nbd_async.c. */
            int rc;
            if (g_multithread) {
                rc = mt_io(IO_OP_TRIM, lba, count, NULL, lba_size);
            } else {
                struct nvme_dsm_range range;
                memset(&range, 0, sizeof(range));
                range.slba = lba;
                range.nlb = count;

                struct nvme_sq_entry sqe;
                struct nvme_cq_entry cqe;
                build_nvme_io_sqe(&sqe, NVME_NVM_DATASET_MANAGEMENT, 1, 0, 0);
                sqe.cdw10 = 0; /* NR = 0 means 1 range */
                sqe.cdw11 = NVME_DSM_ATTR_DEALLOCATE;
                sqe.command_id = cmd_id++;
                rc = nvme_uspace_dispatch_io_cmd(dev, &sqe, &cqe, &range, sizeof(range));
                if (rc == 0 && cqe.status != 0)
                    rc = -1;
            }
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
            "  -v          Verbose I/O logging\n"
            "  -m          Multi-threaded FTL workers\n"
            "  -a          Async NBD pipeline (SPDK-style SQ/CQ, implies -m)\n"
            "  -P <name>   NAND profile: onfi-tlc (default), onfi-qlc, toggle-tlc, toggle-qlc\n"
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
    /* Disable stdio buffering so logs appear immediately in redirected files */
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    uint16_t port = 10809;
    uint64_t size_mb = 512;
    int verbose = 0;
    enum nand_profile_id profile_id = NAND_PROFILE_GENERIC_ONFI_TLC;
    bool profile_explicit = false;
    int opt;

    while ((opt = getopt(argc, argv, "p:s:vmaP:h")) != -1) {
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
        case 'v':
            verbose = 1;
            g_verbose = 1;
            break;
        case 'm':
            g_multithread = 1;
            break;
        case 'a':
            g_async = 1;
            g_multithread = 1; /* async implies multi-threaded */
            break;
        case 'P': {
            enum nand_profile_id id = nand_profile_id_from_name(optarg);
            if (id == NAND_PROFILE_COUNT) {
                fprintf(stderr, "ERROR: unknown profile '%s' (expected onfi-tlc|onfi-qlc|toggle-tlc|toggle-qlc)\n",
                        optarg ? optarg : "");
                return 1;
            }
            profile_id = id;
            profile_explicit = true;
            break;
        }
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    const uint32_t lba_size = 4096;
    const uint64_t export_size = size_mb * 1024ULL * 1024ULL;
    const uint64_t total_lbas = export_size / lba_size;

    /* ------------------------------------------------------------------ */
    /* Auto-size NAND geometry to fit requested export size                 */
    /* ------------------------------------------------------------------ */
    const uint32_t page_size = 4096;
    const uint32_t spare_size = 64;
    const uint32_t op_pct = 7; /* 7% over-provisioning */

    /* Target raw pages: export + OP headroom */
    uint64_t pages_needed = (total_lbas * (100 + op_pct) + 99) / 100;

    /* Start with a baseline and scale up */
    uint32_t nch = 4, nchip = 4, ndie = 2, nplane = 2;
    uint32_t nblk = 512, npg = 256;

    /* Scale blocks first (up to 4096), then pages (up to 1024),
     * then channels (up to 16), then chips (up to 8) */
    for (;;) {
        uint64_t raw = (uint64_t)nch * nchip * ndie * nplane * nblk * npg;
        if (raw >= pages_needed)
            break;
        if (nblk < 4096) {
            nblk = (nblk < 2048) ? nblk * 2 : 4096;
            continue;
        }
        if (npg < 1024) {
            npg = (npg < 512) ? npg * 2 : 1024;
            continue;
        }
        if (nch < 16) {
            nch *= 2;
            continue;
        }
        if (nchip < 8) {
            nchip *= 2;
            continue;
        }
        if (ndie < 8) {
            ndie *= 2;
            continue;
        }
        if (nplane < 4) {
            nplane *= 2;
            continue;
        }
        break; /* maxed out */
    }

    /* ------------------------------------------------------------------ */
    /* Initialize simulator                                                */
    /* ------------------------------------------------------------------ */
    struct nvme_uspace_config config;
    nvme_uspace_config_default(&config);

    config.sssim_cfg.channel_count = nch;
    config.sssim_cfg.chips_per_channel = nchip;
    config.sssim_cfg.dies_per_chip = ndie;
    config.sssim_cfg.planes_per_die = nplane;
    config.sssim_cfg.blocks_per_plane = nblk;
    config.sssim_cfg.pages_per_block = npg;
    config.sssim_cfg.page_size = page_size;
    config.sssim_cfg.spare_size = spare_size;
    config.sssim_cfg.lba_size = lba_size;
    config.sssim_cfg.total_lbas = total_lbas;
    config.sssim_cfg.profile_id = profile_id;
    config.sssim_cfg.profile_explicit = profile_explicit;

    uint64_t raw_pages = (uint64_t)nch * nchip * ndie * nplane * nblk * npg;
    uint64_t raw_gb = (raw_pages * page_size) >> 30;

    struct nvme_uspace_dev dev;

    fprintf(stderr, "========================================\n");
    fprintf(stderr, "HFSSS NBD Server\n");
    fprintf(stderr, "========================================\n");
    fprintf(stderr, "Port:     %u\n", port);
    fprintf(stderr, "Size:     %llu MB (%llu LBAs x %u B)\n", (unsigned long long)size_mb,
            (unsigned long long)total_lbas, lba_size);
    fprintf(stderr, "NAND:     %uch/%uchip/%udie/%uplane/%ublk/%upg/%uB (%" PRIu64 " GB raw)\n", nch, nchip, ndie,
            nplane, nblk, npg, page_size, raw_gb);
    {
        const struct nand_profile *prof = nand_profile_get(profile_id);
        fprintf(stderr, "Profile:  %s%s\n", prof ? prof->id_string : "<unknown>",
                profile_explicit ? " (explicit)" : " (default)");
    }
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

    /* Exercise the NVMe admin command processing path through the full
     * dispatch layer.  This generates E2E coverage for admin commands
     * (identify, get/set features, log pages, queue management) that
     * cannot be reached through the NBD I/O protocol. */
    nvme_uspace_exercise_admin_path(&dev);

    /* ------------------------------------------------------------------ */
    /* Initialize multi-threaded FTL (if -m flag)                          */
    /* ------------------------------------------------------------------ */
    static struct ftl_mt_ctx mt_ctx;
    if (g_multithread) {
        struct ftl_config ftl_cfg;
        memset(&ftl_cfg, 0, sizeof(ftl_cfg));
        ftl_cfg.total_lbas = total_lbas;
        ftl_cfg.page_size = page_size;
        ftl_cfg.pages_per_block = npg;
        ftl_cfg.blocks_per_plane = nblk;
        ftl_cfg.planes_per_die = nplane;
        ftl_cfg.dies_per_chip = ndie;
        ftl_cfg.chips_per_channel = nchip;
        ftl_cfg.channel_count = nch;
        ftl_cfg.op_ratio = op_pct;
        ftl_cfg.gc_policy = GC_POLICY_GREEDY;
        ftl_cfg.gc_threshold = 10;
        ftl_cfg.gc_hiwater = 20;
        ftl_cfg.gc_lowater = 5;

        /* MT FTL uses the same HAL as the sssim device */
        struct hal_ctx *hal = &dev.sssim.hal;

        if (ftl_mt_init(&mt_ctx, &ftl_cfg, hal) != HFSSS_OK) {
            fprintf(stderr, "ERROR: ftl_mt_init failed\n");
            nvme_uspace_dev_stop(&dev);
            nvme_uspace_dev_cleanup(&dev);
            return 1;
        }
        if (ftl_mt_start(&mt_ctx) != HFSSS_OK) {
            fprintf(stderr, "ERROR: ftl_mt_start failed\n");
            ftl_mt_cleanup(&mt_ctx);
            nvme_uspace_dev_stop(&dev);
            nvme_uspace_dev_cleanup(&dev);
            return 1;
        }
        g_mt = &mt_ctx;
        fprintf(stderr, "Mode:     MULTI-THREADED (%d FTL workers + GC + WL)\n", FTL_NUM_WORKERS);
    } else {
        fprintf(stderr, "Mode:     SINGLE-THREADED\n");
    }

    /* ------------------------------------------------------------------ */
    /* Create listening TCP socket                                         */
    /* ------------------------------------------------------------------ */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        perror("socket");
        if (g_mt) {
            ftl_mt_cleanup(g_mt);
            g_mt = NULL;
        }
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
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(g_listen_fd);
        if (g_mt) {
            ftl_mt_cleanup(g_mt);
            g_mt = NULL;
        }
        nvme_uspace_dev_stop(&dev);
        nvme_uspace_dev_cleanup(&dev);
        return 1;
    }

    if (listen(g_listen_fd, 1) != 0) {
        perror("listen");
        close(g_listen_fd);
        if (g_mt) {
            ftl_mt_cleanup(g_mt);
            g_mt = NULL;
        }
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
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

#ifdef HFSSS_DEBUG_TRACE
    /*
     * Wire the per-thread trace ring to the server lifecycle.
     *
     * Enabled when HFSSS_TRACE_DUMP is set in the environment; its value is
     * the path of the binary dump written on orderly shutdown. When unset
     * the ring is still allocated per emitting thread but no dump is
     * produced (trace_shutdown does nothing useful without a path).
     *
     * This is the runtime seam that lets `analyze_trace.py` consume a
     * real trace.bin produced by an end-to-end run, rather than only by
     * the unit test.
     */
    {
        const char *dump = getenv("HFSSS_TRACE_DUMP");
        if (dump && *dump) {
            trace_init(dump);
            fprintf(stderr, "[trace] binary dump enabled -> %s\n", dump);
        } else {
            fprintf(stderr, "[trace] HFSSS_TRACE_DUMP unset; "
                            "trace points will run but no dump produced\n");
        }
    }
#endif

    fprintf(stderr, "Listening on 0.0.0.0:%u — export size %llu MB\n", port, (unsigned long long)size_mb);
    fprintf(stderr, "Waiting for NBD client...\n");

    /* ------------------------------------------------------------------ */
    /* Accept loop (one client at a time)                                  */
    /* ------------------------------------------------------------------ */
    while (g_running) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        int client_fd = accept(g_listen_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EBADF) {
                /* Interrupted by signal or fd closed by handler */
                break;
            }
            perror("accept");
            break;
        }

        fprintf(stderr, "[NBD] Connected: %s:%u\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        if (nbd_handshake(client_fd, export_size) == 0) {
            fprintf(stderr, "[NBD] Handshake OK, entering transmission phase\n");
            if (g_async && g_mt) {
                fprintf(stderr, "Mode:     ASYNC PIPELINE (SQ + CQ + %d FTL workers)\n", FTL_NUM_WORKERS);
                struct nbd_async_ctx async_ctx;
                if (nbd_async_init(&async_ctx, client_fd, lba_size, g_mt, 256) != 0) {
                    fprintf(stderr, "ERROR: nbd_async_init failed\n");
                } else if (nbd_async_start(&async_ctx) != 0) {
                    fprintf(stderr, "ERROR: nbd_async_start failed\n");
                    nbd_async_cleanup(&async_ctx);
                } else {
                    /* Wait for SQ thread to finish (client disconnect or error) */
                    pthread_join(async_ctx.sq_thread, NULL);
                    pthread_join(async_ctx.cq_thread, NULL);
                    nbd_async_cleanup(&async_ctx);
                }
            } else {
                nbd_serve(client_fd, &dev, lba_size);
            }
        } else {
            fprintf(stderr, "[NBD] Handshake failed\n");
        }

        close(client_fd);
        fprintf(stderr, "[NBD] Client disconnected, waiting for next...\n");
    }

    /* ------------------------------------------------------------------ */
    /* Cleanup                                                             */
    /*                                                                     */
    /* Order matters:                                                      */
    /*   1. Close listen_fd so no new clients are accepted.                */
    /*   2. Stop/join the MT FTL workers (GC + WL + per-worker threads).   */
    /*      These reference dev.sssim.hal and emit TRACE_EMIT records, so  */
    /*      they must be joined before (3) HAL teardown and (4) trace     */
    /*      ring free. Otherwise a still-running worker would UAF on       */
    /*      either the HAL context or the trace ring.                      */
    /*   3. Tear down the NVMe device (HAL, media, PCIe shim).             */
    /*   4. Dump + release the trace rings.                                */
    /* ------------------------------------------------------------------ */
    if (g_listen_fd >= 0)
        close(g_listen_fd);

    if (g_mt) {
        ftl_mt_cleanup(g_mt);
        g_mt = NULL;
    }

    nvme_uspace_dev_stop(&dev);
    nvme_uspace_dev_cleanup(&dev);

#ifdef HFSSS_DEBUG_TRACE
    /*
     * Flush the per-thread trace rings. trace_shutdown walks every
     * registered ring in tsc order and emits a single binary file at the
     * path passed to trace_init. Called after the MT workers have been
     * joined (so no more records are being emitted) and after device
     * shutdown (so any pending in-flight IOs have been drained and their
     * final trace records are already in the rings).
     */
    trace_shutdown();
    fprintf(stderr, "[trace] ring dumped and released\n");
#endif

    fprintf(stderr, "[NBD] Server exited cleanly\n");
    return 0;
}
