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
#include "SyncApi.h"
#include "FileEmu.h"

using namespace std;

namespace BdevCpp {

SyncApi::SyncApi(IoPoller *_spio)
    : spio(_spio) {}

SyncApi::~SyncApi() {}

int SyncApi::open(const char *name, int flags, mode_t mode) {
    FileEmu *femu = new FileEmu(name, flags, mode);
    if (femu->desc < 0)
        return -1;
    FileMap &map = FileMap::getInstance();
    if (map.putFile(femu) < 0) {
        delete femu;
        return -1;
    }
    return femu->desc;
}

int SyncApi::close(int desc) {
    FileMap &map = FileMap::getInstance();
    FileEmu *femu = map.getFile(desc);
    if (!femu)
        return -1;

    map.closeFile(desc);
    int sts = ::close(fd);
    delete femu;
    return sts;
}

int SyncApi::read(int desc, char *buffer, size_t bufferSize) {
    mutex mtx;
    condition_variable cv;
    bool ready = false;

    FileMap &map = FileMap::getInstance();
    FileEmu *femu = map.getFile(desc);
    if (!femu)
        return -1;


    IoRqst *getRqst = IoRqst::readPool.get();
    getRqst->finalizeRead(nullptr, 0,
                         [&mtx, &cv, &ready, buffer, bufferSize](
                             Status status, const char *data, size_t dataSize) {
                             unique_lock<mutex> lck(mtx);
                             memcpy(buffer, data, dataSize);
                             ready = true;
                             cv.notify_all();
                         });

    if (!spio->enqueue(getRqst)) {
        IoRqst::readPool.put(getRqst);
        return -1;
    }
    // wait for completion
    {
        unique_lock<mutex> lk(mtx);
        cv.wait_for(lk, 1s, [&ready] { return ready; });
    }
    if (ready == false)
        return -1;

    return bufferSize;
}

int SyncApi::write(int desc, const char *data, size_t dataSize) {
    mutex mtx;
    condition_variable cv;
    bool ready = false;

    IoRqst *writeRqst = IoRqst::writePool.get();
    writeRqst->finalizeWrite(data, dataSize,
        [&mtx, &cv, &ready](Status status,
                            const char *data, size_t dataSize) {
            unique_lock<mutex> lck(mtx);
            ready = true;
            cv.notify_all();
        });

    if (!spio->enqueue(writeRqst)) {
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
} // namespace BdevCpp
