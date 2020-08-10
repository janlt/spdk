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
#include <string>
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

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

using namespace std;

namespace BdevCpp {

GeneralPool<ReadFuture, ClassAlloc<ReadFuture>> ReadFuture::readFuturePool(100, "readFuturePool");
GeneralPool<WriteFuture, ClassAlloc<WriteFuture>> WriteFuture::writeFuturePool(100, "writeFuturePool");

ReadFuture::ReadFuture(char *_buffer, size_t _bufferSize)
: buffer(_buffer), bufferSize(_bufferSize), ready(false) {}

ReadFuture::~ReadFuture() {}

int ReadFuture::get(char *&data, size_t &_dataSize, unsigned long long _timeout) {
    unique_lock<mutex> lk(mtx);
    const chrono::milliseconds timeout(_timeout);
    if (cv.wait_for(lk, timeout, [this] { return !opStatus; }) == false) {
        opStatus = -1;
        lk.unlock();
        return opStatus;
    }

    data = buffer;
    _dataSize = bufferSize;
    lk.unlock();
    return opStatus;
}

void ReadFuture::signal(StatusCode status, const char *data, size_t _dataSize) {
    unique_lock<mutex> lck(mtx);
    opStatus = (status == StatusCode::OK) ? 0 : -1;
    if (!opStatus)
        ::memcpy(buffer, data, _dataSize);
    ready = true;
    lck.unlock();
    cv.notify_all();
}

void ReadFuture::sink() {
    reset();
    ReadFuture::readFuturePool.put(this);
}



WriteFuture::WriteFuture(size_t _dataSize)
    : dataSize(_dataSize), ready(false) {}

WriteFuture::~WriteFuture() {}

int WriteFuture::get(char *&data, size_t &_dataSize, unsigned long long _timeout) {
    unique_lock<mutex> lk(mtx);
    const chrono::milliseconds timeout(_timeout);
    if (cv.wait_for(lk, timeout, [this] { return ready; }) == false) {
        opStatus = -1;
        lk.unlock();
        return opStatus;
    }

    _dataSize = dataSize;
    lk.unlock();
    return opStatus;
}

void WriteFuture::signal(StatusCode status, const char *data, size_t _dataSize) {
    unique_lock<mutex> lck(mtx);
    dataSize = _dataSize;
    opStatus = (status == StatusCode::OK) ? 0 : -1;
    ready = true;
    lck.unlock();
    cv.notify_all();
}

void WriteFuture::sink() {
    reset();
    WriteFuture::writeFuturePool.put(this);
}




GeneralPool<ReadFuturePolling, ClassAlloc<ReadFuturePolling>> ReadFuturePolling::readFuturePollingPool(100, "readFuturePollingPool");
GeneralPool<WriteFuturePolling, ClassAlloc<WriteFuturePolling>> WriteFuturePolling::writeFuturePollingPool(100, "writeFuturePollingPool");

ReadFuturePolling::ReadFuturePolling(char *_buffer, size_t _bufferSize)
    : buffer(_buffer), bufferSize(_bufferSize), state(0) {}

ReadFuturePolling::~ReadFuturePolling() {}

int ReadFuturePolling::get(char *&data, size_t &_dataSize, unsigned long long _timeout) {
    while (!state) ;
    if (opStatus)
        return -1;

    data = buffer;
    _dataSize = bufferSize;
    return opStatus;
}

void ReadFuturePolling::signal(StatusCode status, const char *data, size_t _dataSize) {
    opStatus = (status == StatusCode::OK) ? 0 : -1;
    if (!opStatus)
        ::memcpy(buffer, data, _dataSize);
    state = 1;
}

void ReadFuturePolling::sink() {
    reset();
    ReadFuturePolling::readFuturePollingPool.put(this);
}



WriteFuturePolling::WriteFuturePolling(size_t _dataSize)
    : dataSize(_dataSize), state(0) {}

WriteFuturePolling::~WriteFuturePolling() {}

int WriteFuturePolling::get(char *&data, size_t &_dataSize, unsigned long long _timeout) {
    while (!state) ;
    if (opStatus)
        return -1;

    _dataSize = dataSize;
    return opStatus;
}

void WriteFuturePolling::signal(StatusCode status, const char *data, size_t _dataSize) {
    dataSize = _dataSize;
    opStatus = (status == StatusCode::OK) ? 0 : -1;
    state = 1;
}

void WriteFuturePolling::sink() {
    reset();
    WriteFuturePolling::writeFuturePollingPool.put(this);
}

} // namespace BdevCpp
