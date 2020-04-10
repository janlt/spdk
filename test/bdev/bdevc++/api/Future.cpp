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

#include "ClassAlloc.h"
#include "GeneralPool.h"
#include "Status.h"

#include "FutureBase.h"
#include "Future.h"

using namespace std;

namespace BdevCpp {

GeneralPool<ReadFuture, ClassAlloc<ReadFuture>> ReadFuture::readFuturePool(100, "readFuturePool");
GeneralPool<WriteFuture, ClassAlloc<WriteFuture>> WriteFuture::writeFuturePool(100, "writeFuturePool");

ReadFuture::ReadFuture(char *_buffer, size_t _bufferSize)
: buffer(_buffer), bufferSize(_bufferSize) {}

ReadFuture::~ReadFuture() {}

int ReadFuture::get(char *&data, size_t &_dataSize, unsigned int _timeoutMsec) {
    unique_lock<mutex> lk(mtx);
    const chrono::milliseconds timeout(_timeoutMsec);
    cv.wait_for(lk, timeout, [this] { return !opStatus; });
    if (opStatus)
        return -1;

    data = buffer;
    _dataSize = bufferSize;
    return opStatus;
}

void ReadFuture::signal(Status status, const char *data, size_t _dataSize) {
    unique_lock<mutex> lck(mtx);
    opStatus = (status.ok() == true) ? 0 : -1;
    if (!opStatus)
        ::memcpy(buffer, data, _dataSize);
    cv.notify_all();
}

void ReadFuture::sink() {
    reset();
    ReadFuture::readFuturePool.put(this);
}



WriteFuture::WriteFuture(size_t _dataSize)
    : dataSize(_dataSize) {}

WriteFuture::~WriteFuture() {}

int WriteFuture::get(char *&data, size_t &_dataSize, unsigned int _timeoutMsec) {
    unique_lock<mutex> lk(mtx);
    const chrono::milliseconds timeout(_timeoutMsec);
    cv.wait_for(lk, timeout, [this] { return !opStatus; });
    if (opStatus)
        return -1;

    _dataSize = dataSize;
    return opStatus;
}

void WriteFuture::signal(Status status, const char *data, size_t _dataSize) {
    unique_lock<mutex> lck(mtx);
    dataSize = _dataSize;
    opStatus = (status.ok() == true) ? 0 : -1;
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

int ReadFuturePolling::get(char *&data, size_t &_dataSize, unsigned int _timeoutMsec) {
    while (!state) ;
    if (opStatus)
        return -1;

    data = buffer;
    _dataSize = bufferSize;
    return opStatus;
}

void ReadFuturePolling::signal(Status status, const char *data, size_t _dataSize) {
    opStatus = (status.ok() == true) ? 0 : -1;
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

int WriteFuturePolling::get(char *&data, size_t &_dataSize, unsigned int _timeoutMsec) {
    while (!state) ;
    if (opStatus)
        return -1;

    _dataSize = dataSize;
    return opStatus;
}

void WriteFuturePolling::signal(Status status, const char *data, size_t _dataSize) {
    dataSize = _dataSize;
    opStatus = (status.ok() == true) ? 0 : -1;
    state = 1;
}

void WriteFuturePolling::sink() {
    reset();
    WriteFuturePolling::writeFuturePollingPool.put(this);
}

} // namespace BdevCpp
