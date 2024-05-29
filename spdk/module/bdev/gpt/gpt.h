/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

/** \file
 * GPT internal Interface
 */

#ifndef SPDK_INTERNAL_GPT_H
#define SPDK_INTERNAL_GPT_H

#include "spdk/stdinc.h"

#include "spdk/gpt_spec.h"

#define SPDK_GPT_PART_TYPE_GUID SPDK_GPT_GUID(0x7c5222bd, 0x8f5d, 0x4087, 0x9c00, 0xbf9843c7b58c)
#define SPDK_GPT_BUFFER_SIZE 32768  /* 32KB */
#define	SPDK_GPT_GUID_EQUAL(x,y) (memcmp(x, y, sizeof(struct spdk_gpt_guid)) == 0)

enum spdk_gpt_parse_phase {
	SPDK_GPT_PARSE_PHASE_INVALID = 0,
	SPDK_GPT_PARSE_PHASE_PRIMARY,
	SPDK_GPT_PARSE_PHASE_SECONDARY,
};

struct spdk_gpt {
	uint8_t parse_phase;
	unsigned char *buf;
	uint64_t buf_size;
	uint64_t lba_start;
	uint64_t lba_end;
	uint64_t total_sectors;
	uint32_t sector_size;
	struct spdk_gpt_header *header;
	struct spdk_gpt_partition_entry *partitions;
};

int gpt_parse_mbr(struct spdk_gpt *gpt);
int gpt_parse_partition_table(struct spdk_gpt *gpt);

#endif  /* SPDK_INTERNAL_GPT_H */
