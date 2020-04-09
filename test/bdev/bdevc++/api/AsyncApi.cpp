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

#include "ClassAlloc.h"
#include "GeneralPool.h"
#include "FutureBase.h"
#include "Future.h"
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
    return -1;
}

int AsyncApi::getIoPos(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) {
    lba = femu->pos.posLba + femu->geom.startLba;
    lun = femu->pos.posLun;
    return 0;
}

int AsyncApi::read(int desc, char *buffer, size_t bufferSize) {
    return -1;
}

int AsyncApi::write(int desc, const char *data, size_t dataSize) {
    return -1;
}

int AsyncApi::fsync(int desc) {
    return 0;
}

FutureBase *AsyncApi::read(int desc, uint64_t pos, char *buffer, size_t bufferSize) {
    uint64_t lba;
    uint8_t lun;
    if (getIoPos(desc, pos, lba, lun) < 0)
        return 0;

    ReadFuture *rfut = ReadFuture::readFuturePool.get();
    rfut->buffer = buffer;
    rfut->bufferSize = bufferSize;

    IoRqst *getRqst = IoRqst::readPool.get();
    getRqst->finalizeRead(nullptr, bufferSize,
         [rfut](
             Status status, const char *data, size_t dataSize) {
             rfut->signal(status, data, dataSize);
         },
         lba, lun);

    if (spio->enqueue(getRqst) == false) {
        IoRqst::readPool.put(getRqst);
        ReadFuture::readFuturePool.put(rfut);
        return 0;
    }

    return rfut;
}

FutureBase *AsyncApi::write(int desc, uint64_t pos, const char *data, size_t dataSize) {
    uint64_t lba;
    uint8_t lun;
    if (getIoPos(desc, pos, lba, lun) < 0)
        return 0;

    WriteFuture *wfut = WriteFuture::writeFuturePool.get();
    wfut->dataSize = dataSize;

    IoRqst *writeRqst = IoRqst::writePool.get();
    writeRqst->finalizeWrite(data, dataSize,
        [wfut](Status status,
                const char *data, size_t dataSize) {
            wfut->signal(status, data, dataSize);
        },
        lba, lun);

    if (spio->enqueue(writeRqst) == false) {
        IoRqst::writePool.put(writeRqst);
        WriteFuture::writeFuturePool.put(wfut);
        return 0;
    }

    return wfut;
}

} // namespace BdevCpp
