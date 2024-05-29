/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_MNGT_STEPS_H
#define FTL_MNGT_STEPS_H

#include "ftl_mngt.h"

void ftl_mngt_check_conf(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_open_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_close_base_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

#ifdef SPDK_FTL_VSS_EMU
void ftl_mngt_md_init_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_md_deinit_vss_emu(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);
#endif

void ftl_mngt_superblock_init(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_superblock_deinit(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_open_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_close_cache_bdev(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_register_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_unregister_io_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_io_channel(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_scrub_nv_cache(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_finalize_startup(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_start_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_stop_core_poller(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_layout(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_deinit_md(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_rollback_device(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_dump_stats(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_init_default_sb(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

void ftl_mngt_set_dirty(struct spdk_ftl_dev *dev, struct ftl_mngt_process *mngt);

#endif /* FTL_MNGT_STEPS_H */
