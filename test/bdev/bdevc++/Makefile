SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../..)
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk
include $(SPDK_ROOT_DIR)/mk/spdk.modules.mk
include $(SPDK_ROOT_DIR)/mk/spdk.app_cxx.mk
include $(SPDK_ROOT_DIR)/mk/spdk.lib_deps.mk

#CXXFLAGS += -g $(ENV_CXXFLAGS)
CXXFLAGS += $(ENV_CXXFLAGS)
CXXFLAGS += -I$(SPDK_ROOT_DIR)/lib
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++ 
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/common
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/config
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/io 
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/bdev 
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/api
CXXFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/fileio
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/common
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/config
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/io
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/bdev
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/api
CFLAGS += -I$(SPDK_ROOT_DIR)/test/bdev/bdevc++/fileio

APP_DIRS = common config io bdev api fileio

CXX_SRCS := main.cpp

SPDK_LIB_LIST = $(ALL_MODULES_LIST)
SPDK_LIB_LIST += event_bdev event_accel event_vmd
SPDK_LIB_LIST += bdev accel event trace log conf thread util sock notify
SPDK_LIB_LIST += rpc jsonrpc json app_rpc log_rpc bdev_rpc

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd nbd
endif

#LDFLAGS += -lboost_thread -lpthread -lrt -lnuma -ldl -luuid -lstdc++ -g
APP_LIBS = bdevc++common bdevc++io bdevc++bdev bdevc++config bdevc++api bdevc++fileio
SPDK_LIB_LIST += $(APP_LIBS)
LDFLAGS += -lboost_thread -lpthread -lrt -lnuma -ldl -luuid -lstdc++ -lboost_filesystem -lconfig++

APP = bdevcpp


include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
