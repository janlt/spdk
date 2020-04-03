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

#pragma once

#include <cstddef>
#include <cstdint>

#include <functional>

#include "ClassAlloc.h"
#include "GeneralPool.h"

namespace BdevCpp {

struct ValCtx {
    size_t size = 0;
    void *data = nullptr;
};

struct ConstValCtx {
    size_t size = 0;
    const void *data = nullptr;
};

using RqstCallback = std::function<void(Status status, const char *data, const size_t dataSize)>;

template <class T>
class Rqst {
  public:
    Rqst(const T op, const char *data,
         size_t dataSize, RqstCallback clb)
        : op(op), data(data),
          dataSize(dataSize), clb(clb) {}
    Rqst()
        : op(T::READ), data(0), dataSize(0),
          clb(0) {};
    virtual ~Rqst() = default;
    void finalizeUpdate(const char *_data, size_t _dataSize, RqstCallback _clb) {
        op = T::UPDATE;
        data = _data;
        dataSize = _dataSize;
        clb = _clb;
    }
    void finalizeGet(const char *_data, size_t _dataSize, RqstCallback _clb) {
        op = T::READ;
        data = _data;
        dataSize = _dataSize;
        clb = _clb;
    }
    void finalizeRemove(const char *_data, size_t _dataSize, RqstCallback _clb) {
        op = T::DELETE;
        data = _data;
        dataSize = _dataSize;
        clb = _clb;
    }

    T op;
    const char *data = nullptr;
    size_t dataSize = 0;

    RqstCallback clb;
    unsigned char taskBuffer[256];
    uint64_t devAddrBuf[2];

    static BdevCpp::GeneralPool<Rqst, BdevCpp::ClassAlloc<Rqst>> updatePool;
    static BdevCpp::GeneralPool<Rqst, BdevCpp::ClassAlloc<Rqst>> getPool;
    static BdevCpp::GeneralPool<Rqst, BdevCpp::ClassAlloc<Rqst>> removePool;
};

template <class T>
BdevCpp::GeneralPool<Rqst<T>, BdevCpp::ClassAlloc<Rqst<T>>>
    Rqst<T>::updatePool(100, "updateRqstPool");

template <class T>
BdevCpp::GeneralPool<Rqst<T>, BdevCpp::ClassAlloc<Rqst<T>>>
    Rqst<T>::getPool(100, "getRqstPool");

template <class T>
BdevCpp::GeneralPool<Rqst<T>, BdevCpp::ClassAlloc<Rqst<T>>>
    Rqst<T>::removePool(100, "removeRqstPool");

} // namespace BdevCpp
