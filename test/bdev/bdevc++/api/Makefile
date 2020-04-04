SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/common 
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/config
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/io
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/bdev
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/common 
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/config
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/io
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/bdev
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0
SO_SUFFIX := $(SO_VER).$(SO_MINOR)

CXX_SRCS = SyncApi.cpp AsyncApi.cpp
LIBNAME = bdevc++api

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

