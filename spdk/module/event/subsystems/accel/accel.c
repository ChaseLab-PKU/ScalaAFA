/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/accel_engine.h"

#include "spdk_internal/init.h"
#include "spdk/env.h"

static void
accel_engine_subsystem_initialize(void)
{
	int rc;

	rc = spdk_accel_engine_initialize();

	spdk_subsystem_init_next(rc);
}

static void
accel_engine_subsystem_finish_done(void *cb_arg)
{
	spdk_subsystem_fini_next();
}

static void
accel_engine_subsystem_finish(void)
{
	spdk_accel_engine_finish(accel_engine_subsystem_finish_done, NULL);
}

static struct spdk_subsystem g_spdk_subsystem_accel = {
	.name = "accel",
	.init = accel_engine_subsystem_initialize,
	.fini = accel_engine_subsystem_finish,
	.write_config_json = spdk_accel_write_config_json,
};

SPDK_SUBSYSTEM_REGISTER(g_spdk_subsystem_accel);
