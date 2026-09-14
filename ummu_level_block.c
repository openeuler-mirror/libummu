// SPDX-License-Identifier: MIT
/*
* Copyright (c) 2025 HiSilicon Technologies Co., Ltd. All rights reserved.
* THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
* EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
* MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*/

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ummu_log.h"
#include "ummu_common.h"
#include "ummu_resource.h"
#include "ummu_level_block.h"

static struct ummu_mapt_block *ummu_alloc_new_mapt_block(struct ummu_mapt_info *mapt_info,
	size_t block_size, uint32_t block_index)
{
	struct ummu_mapt_block *block;
	void *block_addr;

	errno = 0;
	if (mapt_info->block_base.table_ctx->expan != true && block_index != 0) {
		errno = ERANGE;
		UMMU_MAPT_DEBUG_LOG("alloc new level_block failed, expan=%d, block_index=%u.\n",
				     mapt_info->block_base.table_ctx->expan, block_index);
		return NULL;
	}

	block = (struct ummu_mapt_block *)calloc(1, sizeof(struct ummu_mapt_block));
	if (block == NULL) {
		UMMU_MAPT_ERROR_LOG("Alloc mapt block failed, errno = %d.\n", errno);
		errno = ENOMEM;
		return NULL;
	}

	block->lvl_block_cnt = mapt_info->block_base.table_ctx->lvl_block_cnt;
	block->level_entry_cnt = (uint16_t *)calloc(block->lvl_block_cnt, sizeof(uint16_t));
	if (block->level_entry_cnt == NULL) {
		UMMU_MAPT_ERROR_LOG("Alloc level_entry_cnt failed.\n");
		free(block);
		errno = ENOMEM;
		return NULL;
	}

	block_addr = ummu_get_core_buf(mapt_info->tid, BASE_MODE_TABLE_BLOCK, block_size,
					   get_ummu_ctx()->shared_fd, block_index);
	if (CORE_BUF_CHECK_INVALID(block_addr)) {
		free(block->level_entry_cnt);
		free(block);
		UMMU_MAPT_ERROR_LOG("Get core buf failed, errno = %d.\n", errno);
		errno = ENOMEM;
		return NULL;
	}

	block->blk_size = block_size;
	block->block_id = block_index;
	block->block_addr = block_addr;
	if (mapt_info->block_base.table_ctx->block_cnt == 0) {
		mapt_info->block_base.table_ctx->mapt_block_base = block;
	}
	mapt_info->block_base.table_ctx->mapt_block_array[block_index] = (void *)block;
	mapt_info->block_base.table_ctx->block_cnt++;

	return block;
}

struct ummu_mapt_table_node *ummu_alloc_level_block(struct ummu_mapt_info *mapt_info,
	struct ummu_mapt_table_node *pre_node, struct ummu_mapt_block *pre_node_mapt_block)
{
	struct ummu_mapt_table_ctx *table_ctx = mapt_info->block_base.table_ctx;
	uint32_t mapt_block_index, next_lv_offset;
	struct ummu_mapt_block *mapt_block;
	size_t block_size;
	uint32_t level_id;

	level_id = find_first_zero_bit(table_ctx->level_block_bitmap,
					   (unsigned long)table_ctx->level_block_bitmap_size);
	if (level_id >= table_ctx->level_block_bitmap_size) {
		errno = ERANGE;
		UMMU_MAPT_ERROR_LOG("Invalid level id.\n");
		return NULL;
	}

	pre_node->next_block = 0;
	mapt_block_index = level_id / table_ctx->lvl_block_cnt;
	block_size = table_ctx->blk_exp_size;

	mapt_block = (struct ummu_mapt_block *)table_ctx->mapt_block_array[mapt_block_index];
	/* the mapt_block corresponding to level_id does not exist. */
	if (mapt_block == NULL) {
		mapt_block = ummu_alloc_new_mapt_block(mapt_info, block_size, mapt_block_index);
		if (mapt_block == NULL) {
			return NULL;
		}
		pre_node->next_block = 1;
	} else {
		if (pre_node_mapt_block != mapt_block) {
			pre_node->next_block = 1;
		}
	}

	pre_node->next_lv_index = mapt_block_index;
	next_lv_offset = MAX_MAPT_ENTRY_INDEX * (level_id % table_ctx->lvl_block_cnt);
	pre_node->next_lv_offset_low = LVL_OFFSET_LOW(next_lv_offset);
	pre_node->next_lv_offset_high = LVL_OFFSET_HIGH(next_lv_offset);

	ummu_set_bit(level_id, table_ctx->level_block_bitmap);
	mapt_block->level_cnt++;

	return (struct ummu_mapt_table_node *)mapt_block->block_addr + next_lv_offset;
}

void ummu_free_level_block(struct ummu_mapt_info *mapt_info, struct ummu_mapt_table_node *prev_node)
{
	struct ummu_mapt_table_ctx *table_ctx = mapt_info->block_base.table_ctx;
	uint32_t lv_offset, level_id, lv_index;
	struct ummu_mapt_block *block;

	lv_offset = 0;
	lv_index = 0;
	if (prev_node != NULL) {
		lv_offset = TABLE_LVL_OFFSET(prev_node->next_lv_offset_low, prev_node->next_lv_offset_high);
		lv_index = prev_node->next_lv_index;
	}
	block = (struct ummu_mapt_block *)table_ctx->mapt_block_array[lv_index];
	level_id = (block->block_id * table_ctx->lvl_block_cnt) + lv_offset / MAX_MAPT_ENTRY_INDEX;
	if (level_id != 0) {
		block->level_cnt--;
		ummu_clear_bit(level_id, table_ctx->level_block_bitmap);
	}
	if (block->level_cnt == 0 && table_ctx->block_cnt > 1) {
		table_ctx->mapt_block_array[block->block_id] = NULL;
		table_ctx->block_cnt--;
		ummu_free_core_buf(BASE_MODE_TABLE_BLOCK, (void *)block->block_addr,
						   table_ctx->blk_exp_size);
		free(block->level_entry_cnt);
		free(block);
	}
}

