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

using RqstCallback = std::function<void(StatusCode status, const char *data, const size_t dataSize)>;

template <class T>
class Rqst {
  public:
    Rqst(const T _op, const char *_data,
         size_t _dataSize, RqstCallback _clb,
         uint64_t _lba = -1, uint32_t _lun = -1)
        : op(_op), data(_data),
          dataSize(_dataSize), clb(_clb), lba(_lba), lun(_lun) {}
    Rqst()
        : op(T::NONE), data(0), dataSize(0),
          clb(0), lba(-1), lun(-1) {};
    virtual ~Rqst() = default;
    void finalizeWrite(const char *_data, size_t _dataSize, RqstCallback _clb,
            uint64_t _lba, uint32_t _lun) {
        op = T::WRITE;
        data = _data;
        dataSize = _dataSize;
        clb = _clb;
        lba = _lba;
        lun = _lun;
    }
    void finalizeRead(const char *_data, size_t _dataSize, RqstCallback _clb,
            uint64_t _lba, uint32_t _lun) {
        op = T::READ;
        data = _data;
        dataSize = _dataSize;
        clb = _clb;
        lba = _lba;
        lun = _lun;
    }

    T op;
    const char *data = nullptr;
    size_t dataSize = 0;

    RqstCallback clb;
    uint64_t lba;
    uint32_t lun;
    unsigned char taskBuffer[256];
    uint64_t devAddrBuf[2];

    static BdevCpp::GeneralPool<Rqst, BdevCpp::ClassAlloc<Rqst>> writePool;
    static BdevCpp::GeneralPool<Rqst, BdevCpp::ClassAlloc<Rqst>> readPool;
};

template <class T>
BdevCpp::GeneralPool<Rqst<T>, BdevCpp::ClassAlloc<Rqst<T>>>
    Rqst<T>::writePool(100, "writeRqstPool");

template <class T>
BdevCpp::GeneralPool<Rqst<T>, BdevCpp::ClassAlloc<Rqst<T>>>
    Rqst<T>::readPool(100, "readRqstPool");

} // namespace BdevCpp
