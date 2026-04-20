#ifndef HFSSS_HAL_AER_H
#define HFSSS_HAL_AER_H

/*
 * NVMe Asynchronous Event Request framework (REQ-063, LLD_13 §5.1).
 *
 * The controller maintains two queues:
 *   - outstanding_cids: AER admin commands the host has submitted that
 *     have not yet received a completion.
 *   - pending_events: asynchronous events the controller wants to
 *     signal but had no outstanding AER to absorb at post time.
 *
 * Lifecycle:
 *   Host submits AER   -> if pending_events non-empty, return an
 *                         immediate CQE; otherwise enqueue CID and
 *                         return "queued".
 *   Controller posts   -> if outstanding_cids non-empty, pop oldest
 *                         CID, emit a completion, return the CID and
 *                         CQE to the caller so the admin CQ layer can
 *                         post it; otherwise buffer event in
 *                         pending_events ring.
 *   Controller reset   -> drain outstanding AERs with SC=COMMAND
 *                         ABORTED and return them to the caller.
 */

#include "common/common.h"
#include "pcie/nvme.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* NVMe spec §5.2 Async Event Type (3 bits, CQE DW0[2:0]). */
enum nvme_async_event_type {
    NVME_AER_TYPE_ERROR        = 0,
    NVME_AER_TYPE_SMART_HEALTH = 1,
    NVME_AER_TYPE_NOTICE       = 2,
    NVME_AER_TYPE_NVM_CMD_SET  = 6,
};

/* Async Event Information subcodes (CQE DW0[15:8]). */
enum nvme_async_event_info {
    NVME_AEI_SMART_NVM_SUBSYS_RELIABILITY = 0x00,
    NVME_AEI_SMART_TEMPERATURE_THRESHOLD  = 0x01,
    NVME_AEI_SMART_SPARE_BELOW_THRESHOLD  = 0x02,
    NVME_AEI_NOTICE_NS_ATTR_CHANGED       = 0x00,
    NVME_AEI_NOTICE_FW_ACTIVATION_START   = 0x01,
};

#define AER_PENDING_MAX   16
#define AER_REQUEST_MAX   16

struct nvme_aer_pending {
    enum nvme_async_event_type type;
    enum nvme_async_event_info info;
    u8                         log_page_id;
};

/* One delivered completion produced by post_event / submit when an
 * event and a waiting AER are matched. The CQE's status and command
 * specific (DW0) fields are filled; dword1 is reserved for the
 * calling layer to stamp SQ head / SQ id / phase bits. */
struct nvme_aer_completion {
    u16                  cid;
    struct nvme_cq_entry cqe;
};

struct hal_aer_ctx {
    struct nvme_aer_pending pending[AER_PENDING_MAX];
    u32                     pending_head;   /* oldest slot */
    u32                     pending_count;

    u16                     outstanding_cids[AER_REQUEST_MAX];
    u32                     outstanding_head; /* oldest slot */
    u32                     outstanding_count;

    pthread_mutex_t         lock;
    bool                    initialized;
};

/* Encode the NVMe DW0 payload for an AER completion:
 *   bits [2:0]  = async event type
 *   bits [15:8] = async event info
 *   bits [23:16]= associated log page identifier
 * All other bits are reserved / zero. */
u32 hal_aer_dw0_encode(enum nvme_async_event_type type,
                       enum nvme_async_event_info info,
                       u8 log_page_id);

/* Lifecycle */
int  hal_aer_init(struct hal_aer_ctx *ctx);
void hal_aer_cleanup(struct hal_aer_ctx *ctx);

/* Host submits a new AER admin command with `cid`. If a pending event
 * is available, `*completed` receives an immediate completion and the
 * function returns HFSSS_OK with `*was_immediate = true`. Otherwise
 * the CID is added to the outstanding queue, `*was_immediate = false`.
 * Returns HFSSS_ERR_NOSPC when the outstanding queue is already full. */
int hal_aer_submit_request(struct hal_aer_ctx *ctx, u16 cid,
                           bool *was_immediate,
                           struct nvme_aer_completion *completed);

/* Controller posts an event. If an outstanding AER is waiting, pop
 * the oldest one, stamp a completion in `*completed`, and return
 * HFSSS_OK with `*was_delivered = true`. Otherwise buffer the event
 * in the pending ring; `*was_delivered = false`. Returns
 * HFSSS_ERR_NOSPC only when BOTH queues are full (pending ring
 * overflowed and no outstanding AER to consume). */
int hal_aer_post_event(struct hal_aer_ctx *ctx,
                       enum nvme_async_event_type type,
                       enum nvme_async_event_info info,
                       u8 log_page_id,
                       bool *was_delivered,
                       struct nvme_aer_completion *completed);

/* Drain outstanding AERs on controller reset. Fills `out[0..cap-1]`
 * with completions stamped SC=0x07 (COMMAND ABORTED). Returns the
 * number of AERs aborted; callers that pass cap=0 can use this to
 * count without draining. */
u32 hal_aer_abort_pending(struct hal_aer_ctx *ctx,
                          struct nvme_aer_completion *out, u32 cap);

/* Introspection for tests and observability. */
u32 hal_aer_outstanding_count(struct hal_aer_ctx *ctx);
u32 hal_aer_pending_count(struct hal_aer_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* HFSSS_HAL_AER_H */
