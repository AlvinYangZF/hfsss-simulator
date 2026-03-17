#include "ftl/superblock.h"
#include "ftl/ftl.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static int sb_write_page(struct superblock_ctx *sb, u32 block_idx, u32 page,
                         const void *data)
{
    struct sb_block_loc *loc = &sb->blocks[block_idx];
    return hal_nand_program_sync(sb->hal, loc->channel, loc->chip,
                                 loc->die, loc->plane, loc->block_id,
                                 page, data, NULL);
}

static int sb_read_page(struct superblock_ctx *sb, u32 block_idx, u32 page,
                        void *data)
{
    struct sb_block_loc *loc = &sb->blocks[block_idx];
    return hal_nand_read_sync(sb->hal, loc->channel, loc->chip,
                              loc->die, loc->plane, loc->block_id,
                              page, data, NULL);
}

static int sb_erase_block(struct superblock_ctx *sb, u32 block_idx)
{
    struct sb_block_loc *loc = &sb->blocks[block_idx];
    return hal_nand_erase_sync(sb->hal, loc->channel, loc->chip,
                               loc->die, loc->plane, loc->block_id);
}

/* Advance to the next superblock page; erase the next block when needed. */
static int sb_advance_page(struct superblock_ctx *sb)
{
    sb->current_page++;
    if (sb->current_page >= sb->pages_per_block) {
        sb->active_block_idx = (sb->active_block_idx + 1) % sb->block_count;
        sb->current_page = 0;
        return sb_erase_block(sb, sb->active_block_idx);
    }
    return HFSSS_OK;
}

/* Linked-list helpers mirroring the pattern in block.c */
static void sb_list_add_head(struct block_desc **list, struct block_desc *block)
{
    if (!list || !block) {
        return;
    }
    block->next = *list;
    block->prev = NULL;
    if (*list) {
        (*list)->prev = block;
    }
    *list = block;
}

/* Rebuild free / open / closed lists from block state after recovery. */
static void sb_rebuild_block_lists(struct block_mgr *mgr)
{
    u64 i;

    mgr->free_list   = NULL;
    mgr->open_list   = NULL;
    mgr->closed_list = NULL;
    mgr->free_blocks   = 0;
    mgr->open_blocks   = 0;
    mgr->closed_blocks = 0;

    for (i = 0; i < mgr->total_blocks; i++) {
        struct block_desc *bd = &mgr->blocks[i];
        bd->next = NULL;
        bd->prev = NULL;

        switch (bd->state) {
        case FTL_BLOCK_FREE:
            sb_list_add_head(&mgr->free_list, bd);
            mgr->free_blocks++;
            break;
        case FTL_BLOCK_OPEN:
            sb_list_add_head(&mgr->open_list, bd);
            mgr->open_blocks++;
            break;
        case FTL_BLOCK_CLOSED:
            sb_list_add_head(&mgr->closed_list, bd);
            mgr->closed_blocks++;
            break;
        default:
            /* RESERVED, BAD, GC — not on any list */
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* sb_init                                                             */
/* ------------------------------------------------------------------ */

int sb_init(struct superblock_ctx *sb, struct block_mgr *mgr,
            struct hal_ctx *hal, struct ftl_config *config)
{
    u32 count, ch;
    int ret;

    if (!sb || !mgr || !hal || !config) {
        return HFSSS_ERR_INVAL;
    }

    memset(sb, 0, sizeof(*sb));

    count = config->channel_count;
    if (count > SB_MAX_BLOCKS) {
        count = SB_MAX_BLOCKS;
    }
    sb->block_count = count;

    for (ch = 0; ch < count; ch++) {
        u32 block_id = config->blocks_per_plane - 1;
        struct block_desc *bd;

        bd = block_find_by_coords(mgr, ch, 0, 0, 0, block_id);
        if (!bd) {
            return HFSSS_ERR_NOENT;
        }

        ret = block_mark_reserved(mgr, bd);
        if (ret != HFSSS_OK) {
            return ret;
        }

        sb->blocks[ch].channel  = ch;
        sb->blocks[ch].chip     = 0;
        sb->blocks[ch].die      = 0;
        sb->blocks[ch].plane    = 0;
        sb->blocks[ch].block_id = block_id;
    }

    sb->page_size       = config->page_size;
    sb->pages_per_block = config->pages_per_block;

    sb->page_buf = (u8 *)calloc(1, sb->page_size);
    if (!sb->page_buf) {
        return HFSSS_ERR_NOMEM;
    }

    /* Half a block worth of journal entries */
    {
        u32 entries_per_page = (sb->page_size - sizeof(struct sb_page_header))
                               / sizeof(struct journal_entry);
        sb->journal_trigger = sb->pages_per_block * entries_per_page / 2;
    }

    sb->hal         = hal;
    sb->initialized = true;

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* sb_cleanup                                                          */
/* ------------------------------------------------------------------ */

void sb_cleanup(struct superblock_ctx *sb)
{
    if (!sb) {
        return;
    }
    free(sb->page_buf);
    memset(sb, 0, sizeof(*sb));
}

/* ------------------------------------------------------------------ */
/* sb_checkpoint_write                                                 */
/* ------------------------------------------------------------------ */

int sb_checkpoint_write(struct superblock_ctx *sb, struct mapping_ctx *mapping)
{
    u32 entries_per_page;
    u32 valid_count = 0;
    u32 total_pages;
    u32 page_index;
    u32 data_size;
    int ret;
    u64 lba;

    if (!sb || !sb->initialized || !mapping) {
        return HFSSS_ERR_INVAL;
    }

    sb->ckpt_sequence++;

    /* Rotate to next block and erase it */
    sb->active_block_idx = (sb->active_block_idx + 1) % sb->block_count;
    ret = sb_erase_block(sb, sb->active_block_idx);
    if (ret != HFSSS_OK) {
        return ret;
    }
    sb->current_page = 0;

    /* Count valid L2P entries */
    for (lba = 0; lba < mapping->l2p_size; lba++) {
        if (mapping->l2p_table[lba].valid) {
            valid_count++;
        }
    }

    entries_per_page = (sb->page_size - sizeof(struct sb_page_header))
                       / sizeof(struct ckpt_entry);
    /* header page + data pages + end page */
    total_pages = 1 + (valid_count + entries_per_page - 1) / entries_per_page + 1;

    /* --- Write header page --- */
    {
        struct sb_page_header *hdr;

        memset(sb->page_buf, 0xFF, sb->page_size);
        hdr = (struct sb_page_header *)sb->page_buf;
        hdr->magic       = SB_HEADER_MAGIC;
        hdr->page_type   = SB_PAGE_HEADER;
        hdr->sequence    = sb->ckpt_sequence;
        hdr->page_index  = 0;
        hdr->total_pages = total_pages;
        hdr->crc32       = hfsss_crc32(sb->page_buf + sizeof(struct sb_page_header),
                                        sb->page_size - sizeof(struct sb_page_header));
        hdr->reserved    = 0;

        ret = sb_write_page(sb, sb->active_block_idx, sb->current_page,
                            sb->page_buf);
        if (ret != HFSSS_OK) {
            return ret;
        }
        sb->current_page++;
    }

    /* --- Write checkpoint data pages --- */
    page_index = 0;
    data_size  = sb->page_size - sizeof(struct sb_page_header);
    sb->buf_used = 0;
    memset(sb->page_buf, 0xFF, sb->page_size);

    for (lba = 0; lba < mapping->l2p_size; lba++) {
        if (!mapping->l2p_table[lba].valid) {
            continue;
        }

        {
            struct ckpt_entry *entry;
            u32 offset = sizeof(struct sb_page_header) + sb->buf_used;

            entry = (struct ckpt_entry *)(sb->page_buf + offset);
            entry->lba     = lba;
            entry->ppn_raw = mapping->l2p_table[lba].ppn.raw;
            sb->buf_used += sizeof(struct ckpt_entry);
        }

        /* Flush full page */
        if (sb->buf_used + sizeof(struct ckpt_entry) > data_size) {
            struct sb_page_header *hdr = (struct sb_page_header *)sb->page_buf;
            hdr->magic       = SB_CKPT_MAGIC;
            hdr->page_type   = SB_PAGE_CKPT_DATA;
            hdr->sequence    = sb->ckpt_sequence;
            hdr->page_index  = page_index;
            hdr->total_pages = total_pages;
            hdr->crc32       = hfsss_crc32(sb->page_buf + sizeof(struct sb_page_header),
                                            sb->buf_used);
            hdr->reserved    = 0;

            ret = sb_write_page(sb, sb->active_block_idx, sb->current_page,
                                sb->page_buf);
            if (ret != HFSSS_OK) {
                return ret;
            }
            sb->current_page++;
            page_index++;
            sb->buf_used = 0;
            memset(sb->page_buf, 0xFF, sb->page_size);
        }
    }

    /* Flush remaining partial page */
    if (sb->buf_used > 0) {
        struct sb_page_header *hdr = (struct sb_page_header *)sb->page_buf;
        hdr->magic       = SB_CKPT_MAGIC;
        hdr->page_type   = SB_PAGE_CKPT_DATA;
        hdr->sequence    = sb->ckpt_sequence;
        hdr->page_index  = page_index;
        hdr->total_pages = total_pages;
        hdr->crc32       = hfsss_crc32(sb->page_buf + sizeof(struct sb_page_header),
                                        sb->buf_used);
        hdr->reserved    = 0;

        ret = sb_write_page(sb, sb->active_block_idx, sb->current_page,
                            sb->page_buf);
        if (ret != HFSSS_OK) {
            return ret;
        }
        sb->current_page++;
        page_index++;
        sb->buf_used = 0;
    }

    /* --- Write end page --- */
    {
        struct sb_page_header *hdr;

        memset(sb->page_buf, 0xFF, sb->page_size);
        hdr = (struct sb_page_header *)sb->page_buf;
        hdr->magic       = SB_CKPT_MAGIC;
        hdr->page_type   = SB_PAGE_CKPT_END;
        hdr->sequence    = sb->ckpt_sequence;
        hdr->page_index  = page_index;
        hdr->total_pages = total_pages;
        hdr->crc32       = 0;
        hdr->reserved    = 0;

        ret = sb_write_page(sb, sb->active_block_idx, sb->current_page,
                            sb->page_buf);
        if (ret != HFSSS_OK) {
            return ret;
        }
        sb->current_page++;
    }

    /* Reset journal state for fresh journal writes after checkpoint */
    sb->journal_entry_count = 0;
    sb->journal_sequence    = 0;
    sb->buf_used            = 0;

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* sb_checkpoint_read                                                  */
/* ------------------------------------------------------------------ */

int sb_checkpoint_read(struct superblock_ctx *sb, struct mapping_ctx *mapping)
{
    u64 best_seq = 0;
    u32 best_idx = 0;
    bool found = false;
    u32 i;
    int ret;

    if (!sb || !sb->initialized || !mapping) {
        return HFSSS_ERR_INVAL;
    }

    /* Scan page 0 of each superblock to find the latest checkpoint */
    for (i = 0; i < sb->block_count; i++) {
        struct sb_page_header *hdr;

        memset(sb->page_buf, 0xFF, sb->page_size);
        ret = sb_read_page(sb, i, 0, sb->page_buf);
        if (ret != HFSSS_OK) {
            continue;
        }

        hdr = (struct sb_page_header *)sb->page_buf;
        if (hdr->magic != SB_HEADER_MAGIC) {
            continue;
        }

        if (!found || hdr->sequence > best_seq) {
            best_seq = hdr->sequence;
            best_idx = i;
            found = true;
        }
    }

    if (!found) {
        return HFSSS_ERR_NOENT;
    }

    sb->ckpt_sequence   = best_seq;
    sb->active_block_idx = best_idx;

    /* Read checkpoint data pages starting from page 1 */
    {
        u32 page = 1;
        u32 data_size = sb->page_size - sizeof(struct sb_page_header);
        u32 entries_per_page = data_size / sizeof(struct ckpt_entry);

        for (;;) {
            struct sb_page_header *hdr;
            u32 crc;

            if (page >= sb->pages_per_block) {
                break;
            }

            memset(sb->page_buf, 0xFF, sb->page_size);
            ret = sb_read_page(sb, best_idx, page, sb->page_buf);
            if (ret != HFSSS_OK) {
                break;
            }

            hdr = (struct sb_page_header *)sb->page_buf;

            if (hdr->magic != SB_CKPT_MAGIC) {
                break;
            }

            if (hdr->page_type == SB_PAGE_CKPT_END) {
                /* Reached end marker */
                page++;
                break;
            }

            if (hdr->page_type != SB_PAGE_CKPT_DATA) {
                break;
            }

            /* Determine actual data size: for the last data page it may be
             * less than a full page.  We use the stored CRC to verify
             * whatever was written.  Compute CRC over max possible data
             * portion and compare; on partial pages the remaining 0xFF
             * bytes were included in the original CRC calculation. */
            {
                u32 entry_count = entries_per_page;
                u32 used = entry_count * sizeof(struct ckpt_entry);
                u32 j;

                /* Verify CRC */
                crc = hfsss_crc32(sb->page_buf + sizeof(struct sb_page_header),
                                  used);

                /* If CRC does not match with full page, try scanning fewer
                 * entries (partial last page). Walk backwards. */
                if (crc != hdr->crc32) {
                    /* Try all possible smaller sizes */
                    bool crc_ok = false;
                    for (used = sizeof(struct ckpt_entry);
                         used <= entry_count * sizeof(struct ckpt_entry);
                         used += sizeof(struct ckpt_entry)) {
                        crc = hfsss_crc32(
                            sb->page_buf + sizeof(struct sb_page_header),
                            used);
                        if (crc == hdr->crc32) {
                            entry_count = used / sizeof(struct ckpt_entry);
                            crc_ok = true;
                            break;
                        }
                    }
                    if (!crc_ok) {
                        return HFSSS_ERR_IO;
                    }
                }

                for (j = 0; j < entry_count; j++) {
                    struct ckpt_entry *entry;
                    union ppn ppn;
                    u32 off = sizeof(struct sb_page_header)
                              + j * sizeof(struct ckpt_entry);

                    entry = (struct ckpt_entry *)(sb->page_buf + off);
                    if (entry->lba == U64_MAX && entry->ppn_raw == U64_MAX) {
                        /* Erased slot — skip */
                        continue;
                    }
                    ppn.raw = entry->ppn_raw;
                    mapping_direct_set(mapping, entry->lba, ppn);
                }
            }

            page++;
        }

        /* Store position for journal replay */
        sb->current_page = page;
    }

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* sb_journal_append                                                   */
/* ------------------------------------------------------------------ */

int sb_journal_append(struct superblock_ctx *sb, enum journal_op op,
                      u64 lba, u64 ppn_raw)
{
    struct journal_entry je;
    u32 data_size;
    int ret;

    if (!sb || !sb->initialized) {
        return HFSSS_ERR_INVAL;
    }

    je.op       = (u32)op;
    je.reserved = 0;
    je.lba      = lba;
    je.ppn_raw  = ppn_raw;
    je.sequence = sb->journal_sequence++;

    /* First entry on a fresh page: clear buffer */
    if (sb->buf_used == 0) {
        memset(sb->page_buf, 0xFF, sb->page_size);
    }

    memcpy(sb->page_buf + sizeof(struct sb_page_header) + sb->buf_used,
           &je, sizeof(je));
    sb->buf_used += sizeof(struct journal_entry);
    sb->journal_entry_count++;

    data_size = sb->page_size - sizeof(struct sb_page_header);

    /* Flush the page when it cannot hold another entry */
    if (sb->buf_used + sizeof(struct journal_entry) > data_size) {
        struct sb_page_header *hdr = (struct sb_page_header *)sb->page_buf;

        hdr->magic       = SB_JRNL_MAGIC;
        hdr->page_type   = SB_PAGE_JRNL_DATA;
        hdr->sequence    = sb->ckpt_sequence;
        hdr->page_index  = sb->current_page;
        hdr->total_pages = 0; /* unknown for journal */
        hdr->crc32       = hfsss_crc32(sb->page_buf + sizeof(struct sb_page_header),
                                        sb->buf_used);
        hdr->reserved    = 0;

        ret = sb_write_page(sb, sb->active_block_idx, sb->current_page,
                            sb->page_buf);
        if (ret != HFSSS_OK) {
            return ret;
        }

        ret = sb_advance_page(sb);
        if (ret != HFSSS_OK) {
            return ret;
        }

        sb->buf_used = 0;
    }

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* sb_journal_flush                                                    */
/* ------------------------------------------------------------------ */

int sb_journal_flush(struct superblock_ctx *sb)
{
    int ret;

    if (!sb || !sb->initialized) {
        return HFSSS_ERR_INVAL;
    }

    if (sb->buf_used == 0) {
        return HFSSS_OK;
    }

    /* Remaining bytes after the entries are already 0xFF from memset */
    {
        struct sb_page_header *hdr = (struct sb_page_header *)sb->page_buf;

        hdr->magic       = SB_JRNL_MAGIC;
        hdr->page_type   = SB_PAGE_JRNL_DATA;
        hdr->sequence    = sb->ckpt_sequence;
        hdr->page_index  = sb->current_page;
        hdr->total_pages = 0;
        hdr->crc32       = hfsss_crc32(sb->page_buf + sizeof(struct sb_page_header),
                                        sb->buf_used);
        hdr->reserved    = 0;

        ret = sb_write_page(sb, sb->active_block_idx, sb->current_page,
                            sb->page_buf);
        if (ret != HFSSS_OK) {
            return ret;
        }
    }

    ret = sb_advance_page(sb);
    if (ret != HFSSS_OK) {
        return ret;
    }

    sb->buf_used = 0;

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* sb_journal_replay                                                   */
/* ------------------------------------------------------------------ */

int sb_journal_replay(struct superblock_ctx *sb, struct mapping_ctx *mapping)
{
    u32 block_idx;
    u32 page;
    u32 data_size;
    u32 entries_per_page;

    if (!sb || !sb->initialized || !mapping) {
        return HFSSS_ERR_INVAL;
    }

    block_idx = sb->active_block_idx;
    page      = sb->current_page;
    data_size = sb->page_size - sizeof(struct sb_page_header);
    entries_per_page = data_size / sizeof(struct journal_entry);

    /* Scan pages starting from the position after the last CKPT_END */
    for (;;) {
        struct sb_page_header *hdr;
        u32 crc;
        u32 j;
        int ret;

        if (page >= sb->pages_per_block) {
            /* Move to the next superblock */
            block_idx = (block_idx + 1) % sb->block_count;
            page = 0;
            /* If we wrapped all the way around, stop */
            if (block_idx == sb->active_block_idx) {
                break;
            }
        }

        memset(sb->page_buf, 0xFF, sb->page_size);
        ret = sb_read_page(sb, block_idx, page, sb->page_buf);
        if (ret != HFSSS_OK) {
            break;
        }

        hdr = (struct sb_page_header *)sb->page_buf;
        if (hdr->magic != SB_JRNL_MAGIC ||
            hdr->page_type != SB_PAGE_JRNL_DATA) {
            break;
        }

        /* Verify CRC — determine the actual data length.
         * Try full page first, then scan for the matching size. */
        {
            bool crc_ok = false;
            u32 try_used;

            for (try_used = entries_per_page * sizeof(struct journal_entry);
                 try_used >= sizeof(struct journal_entry);
                 try_used -= sizeof(struct journal_entry)) {
                crc = hfsss_crc32(sb->page_buf + sizeof(struct sb_page_header),
                                  try_used);
                if (crc == hdr->crc32) {
                    entries_per_page = try_used / sizeof(struct journal_entry);
                    crc_ok = true;
                    break;
                }
            }

            if (!crc_ok) {
                /* CRC failure marks the crash boundary */
                break;
            }
        }

        for (j = 0; j < entries_per_page; j++) {
            struct journal_entry *je;
            u32 off = sizeof(struct sb_page_header)
                      + j * sizeof(struct journal_entry);

            je = (struct journal_entry *)(sb->page_buf + off);

            if (je->op == (u32)JRNL_OP_WRITE) {
                union ppn ppn;
                ppn.raw = je->ppn_raw;
                mapping_direct_set(mapping, je->lba, ppn);
            } else if (je->op == (u32)JRNL_OP_TRIM) {
                mapping_direct_clear(mapping, je->lba);
            }
        }

        sb->journal_sequence = entries_per_page > 0
            ? ((struct journal_entry *)(sb->page_buf
                + sizeof(struct sb_page_header)
                + (entries_per_page - 1) * sizeof(struct journal_entry)))->sequence + 1
            : sb->journal_sequence;

        page++;

        /* Recalculate entries_per_page for the next iteration */
        entries_per_page = data_size / sizeof(struct journal_entry);
    }

    /* Update position for continued journal writes */
    sb->active_block_idx = block_idx;
    sb->current_page     = page;
    sb->buf_used         = 0;
    sb->journal_entry_count = 0;

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* sb_recover                                                          */
/* ------------------------------------------------------------------ */

int sb_recover(struct superblock_ctx *sb, struct mapping_ctx *mapping,
               struct block_mgr *mgr)
{
    int ret;
    u64 i;

    if (!sb || !mapping || !mgr) {
        return HFSSS_ERR_INVAL;
    }

    /* Load L2P from the latest checkpoint */
    ret = sb_checkpoint_read(sb, mapping);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Apply journal entries on top */
    ret = sb_journal_replay(sb, mapping);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Rebuild P2L from L2P */
    ret = mapping_rebuild_p2l(mapping);
    if (ret != HFSSS_OK) {
        return ret;
    }

    /* Reset valid_page_count for all non-reserved, non-bad blocks */
    for (i = 0; i < mgr->total_blocks; i++) {
        struct block_desc *bd = &mgr->blocks[i];
        if (bd->state != FTL_BLOCK_RESERVED && bd->state != FTL_BLOCK_BAD) {
            bd->valid_page_count   = 0;
            bd->invalid_page_count = 0;
        }
    }

    /* Scan L2P to rebuild valid_page_count per block */
    for (i = 0; i < mapping->l2p_size; i++) {
        if (mapping->l2p_table[i].valid) {
            union ppn ppn = mapping->l2p_table[i].ppn;
            struct block_desc *bd = block_find_by_coords(
                mgr, ppn.bits.channel, ppn.bits.chip, ppn.bits.die,
                ppn.bits.plane, ppn.bits.block);
            if (bd) {
                bd->valid_page_count++;
            }
        }
    }

    /* Assign block states based on valid_page_count */
    for (i = 0; i < mgr->total_blocks; i++) {
        struct block_desc *bd = &mgr->blocks[i];
        if (bd->state == FTL_BLOCK_RESERVED || bd->state == FTL_BLOCK_BAD) {
            continue;
        }
        if (bd->valid_page_count > 0) {
            bd->state = FTL_BLOCK_CLOSED;
        } else {
            bd->state = FTL_BLOCK_FREE;
        }
    }

    /* Rebuild linked lists from scratch */
    sb_rebuild_block_lists(mgr);

    return HFSSS_OK;
}

/* ------------------------------------------------------------------ */
/* sb_has_valid_checkpoint                                             */
/* ------------------------------------------------------------------ */

bool sb_has_valid_checkpoint(struct superblock_ctx *sb)
{
    u32 i;

    if (!sb || !sb->initialized) {
        return false;
    }

    for (i = 0; i < sb->block_count; i++) {
        struct sb_page_header *hdr;
        int ret;

        memset(sb->page_buf, 0xFF, sb->page_size);
        ret = sb_read_page(sb, i, 0, sb->page_buf);
        if (ret != HFSSS_OK) {
            continue;
        }

        hdr = (struct sb_page_header *)sb->page_buf;
        if (hdr->magic == SB_HEADER_MAGIC) {
            return true;
        }
    }

    return false;
}
