SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0
SO_SUFFIX := $(SO_VER).$(SO_MINOR)

CXX_SRCS = Config.cpp
LIBNAME = bdevc++config

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk

