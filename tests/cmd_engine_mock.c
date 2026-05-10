/*
 * Mock cmd_engine for die_dispatcher L2 multi-thread tests. See header for
 * the contract.
 */

#include "cmd_engine_mock.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct mock_engine {
    u32             channel_count;
    u32             chips_per_channel;
    u32             dies_per_chip;
    u32             cell_count;
    pthread_mutex_t lock;
    bool           *busy;
};

static u32 me_dev_chips_per_channel(const struct nand_device *dev)
{
    return (dev->channel_count == 0) ? 0 : dev->channels[0].chip_count;
}

static u32 me_dev_dies_per_chip(const struct nand_device *dev)
{
    if (dev->channel_count == 0 || dev->channels[0].chip_count == 0) {
        return 0;
    }
    return dev->channels[0].chips[0].die_count;
}

static u64 me_index(const struct mock_engine *m, u32 ch, u32 chip, u32 die)
{
    return (u64)ch * m->chips_per_channel * m->dies_per_chip
         + (u64)chip * m->dies_per_chip
         + die;
}

struct mock_engine *mock_engine_create(struct nand_device *dev)
{
    if (!dev) {
        return NULL;
    }
    struct mock_engine *m = calloc(1, sizeof(*m));
    if (!m) {
        return NULL;
    }
    m->channel_count     = dev->channel_count;
    m->chips_per_channel = me_dev_chips_per_channel(dev);
    m->dies_per_chip     = me_dev_dies_per_chip(dev);
    m->cell_count        = m->channel_count * m->chips_per_channel * m->dies_per_chip;
    pthread_mutex_init(&m->lock, NULL);
    if (m->cell_count > 0) {
        m->busy = calloc(m->cell_count, sizeof(*m->busy));
        if (!m->busy) {
            pthread_mutex_destroy(&m->lock);
            free(m);
            return NULL;
        }
    }
    return m;
}

void mock_engine_destroy(struct mock_engine *m)
{
    if (!m) {
        return;
    }
    pthread_mutex_destroy(&m->lock);
    free(m->busy);
    free(m);
}

void mock_engine_set_busy(struct mock_engine *m, u32 ch, u32 chip, u32 die)
{
    if (!m || !m->busy) {
        return;
    }
    u64 idx = me_index(m, ch, chip, die);
    if (idx >= m->cell_count) {
        return;
    }
    pthread_mutex_lock(&m->lock);
    m->busy[idx] = true;
    pthread_mutex_unlock(&m->lock);
}

bool mock_engine_is_busy(struct mock_engine *m, u32 ch, u32 chip, u32 die)
{
    if (!m || !m->busy) {
        return false;
    }
    u64 idx = me_index(m, ch, chip, die);
    if (idx >= m->cell_count) {
        return false;
    }
    pthread_mutex_lock(&m->lock);
    bool b = m->busy[idx];
    pthread_mutex_unlock(&m->lock);
    return b;
}

void mock_engine_complete(struct nand_device *dev, u32 ch, u32 chip, u32 die)
{
    if (!dev) {
        return;
    }
    void (*cb)(struct nand_device *, u32, u32, u32) = dev->die_ready_notifier;
    if (cb) {
        cb(dev, ch, chip, die);
    }
}
