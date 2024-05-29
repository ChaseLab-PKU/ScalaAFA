/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 */

#ifndef FTL_DEFS_H
#define FTL_DEFS_H

#include "spdk/stdinc.h"

#ifndef KiB
#define KiB (1ULL << 10)
#endif

#ifndef MiB
#define MiB (1ULL << 20)
#endif

#ifndef GiB
#define GiB (1ULL << 30)
#endif

#ifndef TiB
#define TiB (1ULL << 40)
#endif

#define ftl_abort()		\
	do {			\
		assert(false);	\
		abort();	\
	} while (0)

#define ftl_bug(cond)				\
	do {					\
		if (spdk_unlikely((cond))) {	\
			ftl_abort();		\
		}				\
	} while (0)

#endif /* FTL_DEFS_H */
