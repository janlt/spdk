SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk
include $(SPDK_ROOT_DIR)/mk/spdk.app_cxx.mk

#CXXFLAGS += -g $(ENV_CXXFLAGS)
CXXFLAGS += $(ENV_CXXFLAGS)
CXXFLAGS += -I$(SPDK_ROOT_DIR)/lib
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++ 
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++

DIRS-y += common config io bdev
all: $(DIRS-y)
clean: $(DIRS-y)

CXX_SRCS := main.cpp

APP = bdevcpp

SPDK_LIB_LIST = $(ALL_MODULES_LIST)
SPDK_LIB_LIST += event_bdev event_accel event_vmd
SPDK_LIB_LIST += bdev accel event trace log conf thread util sock notify
SPDK_LIB_LIST += rpc jsonrpc json app_rpc log_rpc bdev_rpc

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd nbd
endif

#LDFLAGS += -lboost_thread -lpthread -lrt -lnuma -ldl -luuid -lstdc++ -g
SPDK_LIB_LIST += bdevc++common bdevc++io bdevc++bdev bdevc++config
LDFLAGS += -lboost_thread -lpthread -lrt -lnuma -ldl -luuid -lstdc++ -lboost_filesystem -lconfig++

include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
