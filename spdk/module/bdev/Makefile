#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) Intel Corporation.
#  All rights reserved.
#

SPDK_ROOT_DIR := $(abspath $(CURDIR)/../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

DIRS-y += delay error gpt lvol malloc null nvme passthru raid split zone_block afa

DIRS-$(CONFIG_XNVME) += xnvme

DIRS-$(CONFIG_CRYPTO) += crypto

DIRS-$(CONFIG_OCF) += ocf

DIRS-$(CONFIG_REDUCE) += compress

DIRS-$(CONFIG_URING) += uring

ifeq ($(OS),Linux)
DIRS-y += aio ftl
DIRS-$(CONFIG_ISCSI_INITIATOR) += iscsi
DIRS-$(CONFIG_VIRTIO) += virtio
DIRS-$(CONFIG_PMDK) += pmem
endif

DIRS-$(CONFIG_RBD) += rbd

.PHONY: all clean $(DIRS-y)

all: $(DIRS-y)
clean: $(DIRS-y)

include $(SPDK_ROOT_DIR)/mk/spdk.subdirs.mk
