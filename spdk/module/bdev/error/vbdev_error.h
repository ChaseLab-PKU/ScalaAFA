/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_ERROR_H
#define SPDK_VBDEV_ERROR_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"

enum vbdev_error_type {
	VBDEV_IO_FAILURE = 1,
	VBDEV_IO_PENDING,
};

typedef void (*spdk_delete_error_complete)(void *cb_arg, int bdeverrno);

/**
 * Create a vbdev on the base bdev to inject error into it.
 *
 * \param base_bdev_name Name of the base bdev.
 * \return 0 on success or negative on failure.
 */
int vbdev_error_create(const char *base_bdev_name);

/**
 * Delete vbdev used to inject errors.
 *
 * \param error_vbdev_name Name of the error vbdev.
 * \param cb_fn Function to call after deletion.
 * \param cb_arg Arguments to pass to cb_fn.
 */
void vbdev_error_delete(const char *error_vbdev_name, spdk_delete_error_complete cb_fn,
			void *cb_arg);

/**
 * Inject error to the base bdev. Users can specify which IO type error is injected,
 * what type of error is injected, and how many errors are injected.
 *
 * \param name Name of the base bdev into which error is injected.
 * \param io_type IO type into which error is injected.
 * \param error_num Count of injected errors
 */
int vbdev_error_inject_error(char *name, uint32_t io_type, uint32_t error_type,
			     uint32_t error_num);

#endif /* SPDK_VBDEV_ERROR_H */
