/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_ACCEL_ENGINE_IOAT_H
#define SPDK_ACCEL_ENGINE_IOAT_H

#include "spdk/stdinc.h"

#define IOAT_MAX_CHANNELS	64

void accel_engine_ioat_enable_probe(void);

#endif /* SPDK_ACCEL_ENGINE_IOAT_H */
