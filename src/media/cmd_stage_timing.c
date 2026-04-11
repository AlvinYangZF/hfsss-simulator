#include "media/cmd_engine.h"

/*
 * Current budget partition: the full command budget is attributed to the
 * array-busy phase. Setup and data-transfer phases are zero. Future work
 * (richer timing model) will repartition here without touching the engine.
 */
void nand_cmd_stage_budget(enum nand_cmd_opcode op, u64 total_ns, u64 *setup_ns, u64 *array_ns, u64 *xfer_ns)
{
    (void)op;
    if (setup_ns) {
        *setup_ns = 0;
    }
    if (array_ns) {
        *array_ns = total_ns;
    }
    if (xfer_ns) {
        *xfer_ns = 0;
    }
}
