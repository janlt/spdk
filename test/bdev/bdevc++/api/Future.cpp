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

int ReadFuture::get(char *&data, size_t &_dataSize) {
    unique_lock<mutex> lk(mtx);
    cv.wait_for(lk, 1s, [this] { return !opStatus; });
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
    ReadFuture::readFuturePool.put(this);
}



WriteFuture::WriteFuture(size_t _dataSize)
    : dataSize(_dataSize) {}

WriteFuture::~WriteFuture() {}

int WriteFuture::get(char *&data, size_t &_dataSize) {
    unique_lock<mutex> lk(mtx);
    cv.wait_for(lk, 1s, [this] { return !opStatus; });
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
    WriteFuture::writeFuturePool.put(this);
}

} // namespace BdevCpp
