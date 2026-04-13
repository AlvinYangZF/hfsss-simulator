#include "media/cmd_engine.h"

/*
 * Stage-budget partition. For most commands the full budget goes to
 * array-busy. Cache commands use the cache_overlap_ns parameter
 * (tDCBSYR1 for cache read, tCBSY for cache program) to split the
 * budget so the overlap window is driven by actual timing knobs.
 */
void nand_cmd_stage_budget(enum nand_cmd_opcode op, u64 total_ns, u64 cache_overlap_ns, u64 *setup_ns, u64 *array_ns,
                           u64 *xfer_ns)
{
    if (setup_ns) {
        *setup_ns = 0;
    }

    switch (op) {
    case NAND_OP_CACHE_READ:
        /* Array reads the next page (tDCBSYR1) while data transfers
         * on the bus. The overlap window is the array portion. */
        if (array_ns) {
            *array_ns = cache_overlap_ns < total_ns ? cache_overlap_ns : total_ns;
        }
        if (xfer_ns) {
            *xfer_ns = cache_overlap_ns < total_ns ? total_ns - cache_overlap_ns : 0;
        }
        break;

    case NAND_OP_CACHE_PROG:
        /* The cache program's overlap cost is tCBSY; the remainder
         * of tPROG runs in the background after the caller returns. */
        if (array_ns) {
            *array_ns = cache_overlap_ns < total_ns ? cache_overlap_ns : total_ns;
        }
        if (xfer_ns) {
            *xfer_ns = 0;
        }
        break;

    default:
        /* Non-cache ops and CACHE_READ_END (terminal page) pay the
         * full budget as array time. */
        if (array_ns) {
            *array_ns = total_ns;
        }
        if (xfer_ns) {
            *xfer_ns = 0;
        }
        break;
    }
}
