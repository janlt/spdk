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

APP_DIRS = common config io bdev

CXX_SRCS := main.cpp

SPDK_LIB_LIST = $(ALL_MODULES_LIST)
SPDK_LIB_LIST += event_bdev event_accel event_vmd
SPDK_LIB_LIST += bdev accel event trace log conf thread util sock notify
SPDK_LIB_LIST += rpc jsonrpc json app_rpc log_rpc bdev_rpc

ifeq ($(OS),Linux)
SPDK_LIB_LIST += event_nbd nbd
endif

#LDFLAGS += -lboost_thread -lpthread -lrt -lnuma -ldl -luuid -lstdc++ -g
APP_LIBS = bdevc++common bdevc++io bdevc++bdev bdevc++config
SPDK_LIB_LIST += $(APP_LIBS)
LDFLAGS += -lboost_thread -lpthread -lrt -lnuma -ldl -luuid -lstdc++ -lboost_filesystem -lconfig++

APP = bdevcpp


include $(SPDK_ROOT_DIR)/mk/spdk.app.mk
