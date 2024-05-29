/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_FTL_H
#define SPDK_FTL_H

#include "spdk/stdinc.h"
#include "spdk/uuid.h"
#include "spdk/thread.h"
#include "spdk/bdev.h"

#ifdef __cplusplus
extern "C" {
#endif

struct spdk_ftl_dev;

struct spdk_ftl_conf {
	/* Device's name */
	char					*name;

	/* Device UUID (valid when restoring device from disk) */
	struct spdk_uuid			uuid;

	/* Percentage of base device blocks not exposed to the user */
	uint64_t				overprovisioning;

	/* Core mask - core thread plus additional relocation threads */
	char					*core_mask;

	/* IO pool size per user thread */
	size_t					user_io_pool_size;

	/* FTL startup mode mask, see spdk_ftl_mode enum for possible values */
	uint32_t				mode;

	/* Name of base block device (zoned or non-zoned) */
	char					*base_bdev;

	/* Name of cache block device (must support extended metadata) */
	char					*cache_bdev;
};

enum spdk_ftl_mode {
	/* Create new device */
	SPDK_FTL_MODE_CREATE = (1 << 0),
};

struct spdk_ftl_attrs {
	/* Number of logical blocks */
	uint64_t			num_blocks;
	/* Logical block size */
	uint64_t			block_size;
	/* Number of zones in the underlying device (including any offline ones) */
	uint64_t			num_zones;
	/* Number of logical blocks per zone */
	uint64_t			zone_size;
	/* Optimal IO size - bdev layer will split requests over this size */
	uint64_t			optimum_io_size;
};

typedef void (*spdk_ftl_fn)(void *cb_arg, int status);
typedef void (*spdk_ftl_init_fn)(struct spdk_ftl_dev *dev, void *cb_arg, int status);

/**
 * Initializes the FTL library.
 *
 * @return 0 on success, negative errno otherwise.
 */
int spdk_ftl_init(void);

/**
 * Deinitializes the FTL library.
 */
void spdk_ftl_fini(void);

/**
 * Initialize the FTL on the given pair of bdevs - base and cache bdev.
 * Upon receiving a successful completion callback user is free to use I/O calls.
 *
 * \param conf configuration for new device
 * \param cb callback function to call when the device is created
 * \param cb_arg callback's argument
 *
 * \return 0 if initialization was started successfully, negative errno otherwise.
 */
int spdk_ftl_dev_init(const struct spdk_ftl_conf *conf, spdk_ftl_init_fn cb, void *cb_arg);

/**
 * Deinitialize and free given device.
 *
 * \param dev device
 * \param cb callback function to call when the device is freed
 * \param cb_arg callback's argument
 *
 * \return 0 if deinitialization was started successfully, negative errno otherwise.
 */
int spdk_ftl_dev_free(struct spdk_ftl_dev *dev, spdk_ftl_fn cb, void *cb_arg);

/**
 * Retrieve device’s attributes.
 *
 * \param dev device
 * \param attr Attribute structure to fill
 */
void spdk_ftl_dev_get_attrs(const struct spdk_ftl_dev *dev, struct spdk_ftl_attrs *attr);

/**
 * Retrieve device’s configuration.
 *
 * \param dev device
 * \param conf FTL configuration structure to fill
 */
void spdk_ftl_dev_get_conf(const struct spdk_ftl_dev *dev, struct spdk_ftl_conf *conf);

/**
 * Obtain an I/O channel for the device.
 *
 * \param dev device
 *
 * \return A handle to the I/O channel or NULL on failure.
 */
struct spdk_io_channel *spdk_ftl_get_io_channel(struct spdk_ftl_dev *dev);

/**
 * Make a deep copy of an FTL configuration structure
 *
 * \param dst The destination FTL configuration
 * \param src The source FTL configuration
 */
int spdk_ftl_conf_copy(struct spdk_ftl_conf *dst, const struct spdk_ftl_conf *src);

/**
 * Release the FTL configuration resources. This does not free the structure itself.
 *
 * \param conf FTL configuration to deinitialize
 */
void spdk_ftl_conf_deinit(struct spdk_ftl_conf *conf);

/**
 * Initialize FTL configuration structure with default values.
 *
 * \param conf FTL configuration to initialize
 */
void spdk_ftl_get_default_conf(struct spdk_ftl_conf *conf);

/**
 * Returns the size of ftl_io struct that needs to be passed to spdk_ftl_read/write
 *
 * \return The size of struct
 */
size_t spdk_ftl_io_size(void);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_FTL_H */
