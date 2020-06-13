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

#include "FutureBase.h"
#include "Future.h"
#include "ApiBase.h"
#include "SyncApi.h"
#include "FileEmu.h"

using namespace std;

namespace BdevCpp {

SyncApi::SyncApi(IoPoller *_spio)
    : spio(_spio) {}

SyncApi::~SyncApi() {}

int SyncApi::open(const char *name, int flags, mode_t mode) {
    return ApiBase::open(name, flags, mode);
}

int SyncApi::close(int desc) {
    return ApiBase::close(desc);
}

off_t SyncApi::lseek(int fd, off_t offset, int whence) {
    return ApiBase::lseek(fd, offset, whence);
}

int SyncApi::getIoPosLinear(int desc, uint64_t &lba, uint8_t &lun) {
    FileEmu *femu = FileMap::getInstance().getFile(desc);
    if (!femu)
        return -1;
    lba = (femu->pos.posLba + femu->geom.startLba)*femu->geom.blocksPerOptIo;
    lun = femu->pos.posLun;
    return 0;
}

int SyncApi::getIoPosLinear(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) {
    return -1;
}

int SyncApi::getIoPosStriped(int desc, uint64_t &lba, uint8_t &lun) {
    return 0;
}

int SyncApi::getIoPosStriped(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) {
    return -1;
}

int SyncApi::read(int desc, char *buffer, size_t bufferSize) {
    mutex mtx;
    condition_variable cv;
    bool ready = false;

    uint64_t lba;
    uint8_t lun;
    if (getIoPosLinear(desc, lba, lun) < 0)
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
    ApiBase::lseek(desc, bufferSize, SEEK_CUR);

    return bufferSize;
}

int SyncApi::pread(int desc, char *buffer, size_t bufferSize, off_t offset) {
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

    return bufferSize;
}

int SyncApi::write(int desc, const char *data, size_t dataSize) {
    mutex mtx;
    condition_variable cv;
    bool ready = false;

    uint64_t lba;
    uint8_t lun;
    if (getIoPosLinear(desc, lba, lun) < 0)
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
    ApiBase::lseek(desc, dataSize, SEEK_CUR);

    return dataSize;
}

int SyncApi::pwrite(int desc, const char *data, size_t dataSize, off_t offset) {
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

    return dataSize;
}

int SyncApi::fsync(int desc) {
    return 0;
}

FutureBase *SyncApi::read(int desc, uint64_t pos, char *buffer, size_t bufferSize, bool polling) {
    return 0;
}

FutureBase *SyncApi::write(int desc, uint64_t pos, const char *data, size_t dataSize, bool polling) {
    return 0;
}

} // namespace BdevCpp
