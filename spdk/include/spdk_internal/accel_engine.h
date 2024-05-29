/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef SPDK_INTERNAL_ACCEL_ENGINE_H
#define SPDK_INTERNAL_ACCEL_ENGINE_H

#include "spdk/stdinc.h"

#include "spdk/accel_engine.h"
#include "spdk/queue.h"
#include "spdk/config.h"

#ifdef SPDK_CONFIG_ISAL
#include "../isa-l/include/igzip_lib.h"
#endif

struct engine_info {
	struct spdk_json_write_ctx *w;
	const char *name;
	enum accel_opcode ops[ACCEL_OPC_LAST];
	uint32_t num_ops;
};

typedef void (*_accel_for_each_engine_fn)(struct engine_info *info);
void _accel_for_each_engine(struct engine_info *info, _accel_for_each_engine_fn fn);

struct spdk_accel_task;

void spdk_accel_task_complete(struct spdk_accel_task *task, int status);

struct accel_io_channel {
	struct spdk_io_channel		*engine_ch[ACCEL_OPC_LAST];
	/* for ISAL */
#ifdef SPDK_CONFIG_ISAL
	struct isal_zstream		stream;
	struct inflate_state		state;
#endif
	void				*task_pool_base;
	TAILQ_HEAD(, spdk_accel_task)	task_pool;
};

struct sw_accel_io_channel {
	struct spdk_poller		*completion_poller;
	TAILQ_HEAD(, spdk_accel_task)	tasks_to_complete;
};

struct spdk_accel_task {
	struct accel_io_channel		*accel_ch;
	spdk_accel_completion_cb	cb_fn;
	void				*cb_arg;
	union {
		struct {
			struct iovec		*iovs; /* iovs passed by the caller */
			uint32_t		iovcnt; /* iovcnt passed by the caller */
		} v;
		void				*src;
	};
	union {
		void			*dst;
		void			*src2;
	};
	union {
		void				*dst2;
		uint32_t			seed;
		uint64_t			fill_pattern;
	};
	union {
		uint32_t		*crc_dst;
		uint32_t		*output_size;
	};
	enum accel_opcode		op_code;
	uint64_t			nbytes;
	uint64_t			nbytes_dst;
	int				flags;
	int				status;
	TAILQ_ENTRY(spdk_accel_task)	link;
};

struct spdk_accel_engine {
	const char *name;
	bool (*supports_opcode)(enum accel_opcode);
	struct spdk_io_channel *(*get_io_channel)(void);
	int (*submit_tasks)(struct spdk_io_channel *ch, struct spdk_accel_task *accel_task);
	TAILQ_ENTRY(spdk_accel_engine) tailq;
};

struct spdk_accel_module_if {
	/** Initialization function for the module.  Called by the spdk
	 *   application during startup.
	 *
	 *  Modules are required to define this function.
	 */
	int	(*module_init)(void);

	/** Finish function for the module.  Called by the spdk application
	 *   before the spdk application exits to perform any necessary cleanup.
	 *
	 *  Modules are not required to define this function.
	 */
	void	(*module_fini)(void *ctx);

	/**
	 * Write Acceleration module configuration into provided JSON context.
	 */
	void	(*write_config_json)(struct spdk_json_write_ctx *w);

	/**
	 * Returns the allocation size required for the modules to use for context.
	 */
	size_t	(*get_ctx_size)(void);

	TAILQ_ENTRY(spdk_accel_module_if)	tailq;
};

void spdk_accel_engine_register(struct spdk_accel_engine *accel_engine);
void spdk_accel_module_list_add(struct spdk_accel_module_if *accel_module);

#define SPDK_ACCEL_MODULE_REGISTER(init_fn, fini_fn, config_json, ctx_size_fn)				\
	static struct spdk_accel_module_if init_fn ## _if = {						\
	.module_init		= init_fn,								\
	.module_fini		= fini_fn,								\
	.write_config_json	= config_json,								\
	.get_ctx_size		= ctx_size_fn,								\
	};												\
	__attribute__((constructor)) static void init_fn ## _init(void)					\
	{												\
		spdk_accel_module_list_add(&init_fn ## _if);						\
	}

#endif
