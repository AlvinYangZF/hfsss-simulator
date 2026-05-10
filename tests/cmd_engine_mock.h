/*
 * Minimal mock for cmd_engine, used by die_dispatcher L2 multi-thread tests.
 *
 * The real cmd_engine fires struct nand_device::die_ready_notifier when a
 * die transitions to DIE_IDLE. The mock simulates that single observable
 * effect without involving NAND timing, channel locks, or page state. It
 * lets the test harness drive completion events directly:
 *
 *   - mock_engine_set_busy(ch, chip, die)  marks a die "busy" so subsequent
 *     submit attempts can be modeled as failing-fast in the test.
 *   - mock_engine_complete(dev, ch, chip, die)  fires the notifier hook
 *     installed on the device, which is exactly what the real cmd_engine
 *     does at anchor A and anchor B.
 *
 * The mock owns no memory beyond a small per-(ch,chip,die) busy flag table;
 * the device pointer it operates on belongs to the caller.
 */
#ifndef __HFSSS_TESTS_CMD_ENGINE_MOCK_H
#define __HFSSS_TESTS_CMD_ENGINE_MOCK_H

#include "common/common.h"
#include "media/nand.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mock_engine;

/*
 * Construct a mock that mirrors the geometry of `dev`. The mock does not
 * install any callback on the device; tests are expected to install their
 * dispatcher first and then call mock_engine_complete to fire the hook.
 */
struct mock_engine *mock_engine_create(struct nand_device *dev);
void                mock_engine_destroy(struct mock_engine *m);

/* Set the (ch,chip,die) "busy" flag. Test-only inspection helper. */
void mock_engine_set_busy(struct mock_engine *m, u32 ch, u32 chip, u32 die);
bool mock_engine_is_busy(struct mock_engine *m, u32 ch, u32 chip, u32 die);

/*
 * Simulate a NAND completion. Clears the die's busy flag and fires the
 * notifier registered on `dev` (typically die_dispatcher_on_die_ready).
 * Safe to call when the die is already idle and when no notifier is
 * installed.
 */
void mock_engine_complete(struct nand_device *dev, u32 ch, u32 chip, u32 die);

#ifdef __cplusplus
}
#endif

#endif /* __HFSSS_TESTS_CMD_ENGINE_MOCK_H */
