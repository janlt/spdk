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

int AsyncApi::getIoPosLinear(int desc, uint64_t &lba, uint8_t &lun) {
    return ApiBase::getIoPosLinear(desc, lba, lun);
}

int AsyncApi::getIoPosLinear(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) {
    return ApiBase::getIoPosLinear(desc, pos, lba, lun);
}

int AsyncApi::getIoPosStriped(int desc, uint64_t &lba, uint8_t &lun) {
    return ApiBase::getIoPosStriped(desc, lba, lun);
}

int AsyncApi::getIoPosStriped(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) {
    return ApiBase::getIoPosStriped(desc, pos, lba, lun);
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

FutureBase *AsyncApi::read(int desc, uint64_t pos, char *buffer, size_t bufferSize, bool polling) {
    uint64_t lba;
    uint8_t lun;
    if (getIoPosStriped(desc, pos, lba, lun) < 0)
        return 0;

    FutureBase *rfut;
    if (polling == true)
        rfut = ReadFuturePolling::readFuturePollingPool.get();
    else
        rfut = ReadFuture::readFuturePool.get();
    rfut->setBuffer(buffer);
    rfut->setBufferSize(bufferSize);

    IoRqst *getRqst = &rfut->ioRqst;
    getRqst->finalizeRead(nullptr, bufferSize,
         [rfut](
             StatusCode status, const char *data, size_t dataSize) {
             rfut->signal(status, data, dataSize);
         },
         lba, lun, true);

    if (spio->enqueue(getRqst) == false) {
        rfut->sink();
        return 0;
    }
    ApiBase::lseek(desc, pos + bufferSize, SEEK_CUR);

    return rfut;
}

FutureBase *AsyncApi::write(int desc, uint64_t pos, const char *data, size_t dataSize, bool polling) {
    uint64_t lba;
    uint8_t lun;
    if (getIoPosStriped(desc, pos, lba, lun) < 0)
        return 0;

    FutureBase *wfut;
    if (polling == true)
        wfut = WriteFuturePolling::writeFuturePollingPool.get();
    else
        wfut = WriteFuture::writeFuturePool.get();
    wfut->setDataSize(dataSize);

    IoRqst *writeRqst = &wfut->ioRqst;
    writeRqst->finalizeWrite(data, dataSize,
        [wfut](StatusCode status,
                const char *data, size_t dataSize) {
            wfut->signal(status, data, dataSize);
        },
        lba, lun, true);

    if (spio->enqueue(writeRqst) == false) {
        wfut->sink();
        return 0;
    }
    ApiBase::lseek(desc, pos + dataSize, SEEK_CUR);

    return wfut;
}

int AsyncApi::pread(int desc, char *buffer, size_t bufferSize, off_t offset) {
    mutex mtx;
    condition_variable cv;
    bool ready = false;

    uint64_t lba;
    uint8_t lun;
    if (getIoPosStriped(desc, static_cast<uint64_t>(offset), lba, lun) < 0)
        return -1;

    IoRqst *getRqst = IoRqst::readPool.get();
    getRqst->finalizeRead(nullptr, bufferSize,
         [&mtx, &cv, &ready, buffer, bufferSize](
             StatusCode status, const char *data, size_t dataSize) {
             unique_lock<mutex> lck(mtx);
             ready = status == StatusCode::OK ? true : false;
             if (ready == true)
                 memcpy(buffer, data, dataSize);
             cv.notify_all();
         },
         lba, lun);

    if (spio->enqueue(getRqst) == false) {
        IoRqst::readPool.put(getRqst);
        return -1;
    }
    // wait for completion
    {
        unique_lock<mutex> lk(mtx);
        cv.wait_for(lk, 1s, [&ready] { return ready; });
        if (ready == false)
            return -1;
    }
    ApiBase::lseek(desc, offset + bufferSize, SEEK_CUR);

    return bufferSize;
}

int AsyncApi::pwrite(int desc, const char *data, size_t dataSize, off_t offset) {
    mutex mtx;
    condition_variable cv;
    bool ready = false;

    uint64_t lba;
    uint8_t lun;
    if (getIoPosStriped(desc, static_cast<uint64_t>(offset), lba, lun) < 0)
        return -1;

    IoRqst *writeRqst = IoRqst::writePool.get();
    writeRqst->finalizeWrite(data, dataSize,
        [&mtx, &cv, &ready](StatusCode status,
                const char *data, size_t dataSize) {
            unique_lock<mutex> lck(mtx);
            ready = status == StatusCode::OK ? true : false;
            cv.notify_all();
        },
        lba, lun);

    if (spio->enqueue(writeRqst) == false) {
        IoRqst::writePool.put(writeRqst);
        return -1;
    }
    // wait for completion
    {
        unique_lock<mutex> lk(mtx);
        cv.wait_for(lk, 1s, [&ready] { return ready; });
        if (ready == false)
            return -1;
    }
    ApiBase::lseek(desc, offset + dataSize, SEEK_CUR);

    return dataSize;
}

} // namespace BdevCpp
