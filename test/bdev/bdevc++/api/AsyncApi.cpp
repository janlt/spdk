/**
 *  Copyright (c) 2019 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License. 
 */

#include <stdio.h>
#include <time.h>

#include <iostream>
#include <sstream>
#include <string>
#include <mutex>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

#include <string>
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>

#include "spdk/conf.h"
#include "spdk/cpuset.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/ftl.h"
#include "spdk/log.h"
#include "spdk/queue.h"
#include "spdk/stdinc.h"
#include "spdk/thread.h"

#include "Status.h"
#include "Options.h"
#include "BlockingPoller.h"
#include "Poller.h"
#include "IoPoller.h"
#include "Io2Poller.h"
#include "Rqst.h"
#include "SpdkDevice.h"
#include "SpdkConf.h"
#include "SpdkBdev.h"
#include "SpdkIoBuf.h"
#include "SpdkCore.h"

#include "ApiBase.h"
#include "AsyncApi.h"
#include "FileEmu.h"

namespace BdevCpp {

AsyncApi::AsyncApi(IoPoller *_spio)
    : spio(_spio) {}
AsyncApi::~AsyncApi() {}

int AsyncApi::open(const char *name, int flags, mode_t mode) {
    return ApiBase::open(name, flags, mode);
}

int AsyncApi::close(int desc) {
    return ApiBase::close(desc);
}

off_t AsyncApi::lseek(int fd, off_t offset, int whence) {
    return ApiBase::lseek(fd, offset, whence);
}

int AsyncApi::getIoPos(int desc, uint64_t &lba, uint8_t &lun) {
    FileMap &map = FileMap::getInstance();
    FileEmu *femu = map.getFile(desc);
    if (!femu)
        return -1;
    lba = femu->pos.posLba + femu->geom.startLba;
    lun = femu->pos.posLun;
    return 0;
}

int AsyncApi::read(int desc, char *buffer, size_t bufferSize) {
    return 0;
}

int AsyncApi::write(int desc, const char *data, size_t dataSize) {
    return 0;
}

int AsyncApi::fsync(int desc) {
    return 0;
}

} // namespace BdevCpp
