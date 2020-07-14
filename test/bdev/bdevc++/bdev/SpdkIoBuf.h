/**
 *  Copyright (c) 2020 Intel Corporation
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

#include "ClassAlloc.h"
#include "GeneralPool.h"

namespace BdevCpp {

class SpdkIoBuf {
  public:
    SpdkIoBuf() = default;
    virtual ~SpdkIoBuf() = default;

    virtual SpdkIoBuf *getWriteBuf() = 0;
    virtual void putWriteBuf(SpdkIoBuf *ioBuf) = 0;
    virtual SpdkIoBuf *getReadBuf() = 0;
    virtual void putReadBuf(SpdkIoBuf *ioBuf) = 0;
    virtual char *getSpdkDmaBuf() = 0;
    virtual void setSpdkDmaBuf(void *spdkBuf) = 0;
    virtual int getIdx() = 0;
    virtual void setIdx(int idx) = 0;
    virtual uint32_t getBufSize() = 0;
    virtual void setAttribs(uint32_t _bufSize, int _backIdx) = 0;
};

template <uint32_t Size, int Inst> class SpdkIoSizedBuf : public SpdkIoBuf {
    friend class SpdkIoBufMgr;

  public:
    SpdkIoSizedBuf(uint32_t _bufSize = 0, int _backIdx = 0);
    virtual ~SpdkIoSizedBuf();

    virtual SpdkIoBuf *getWriteBuf();
    virtual void putWriteBuf(SpdkIoBuf *ioBuf);
    virtual SpdkIoBuf *getReadBuf();
    virtual void putReadBuf(SpdkIoBuf *ioBuf);
    virtual char *getSpdkDmaBuf();
    virtual void setSpdkDmaBuf(void *spdkBuf);
    virtual int getIdx();
    virtual void setIdx(int idx);
    virtual uint32_t getBufSize();
    virtual void setAttribs(uint32_t _bufSize, int _backIdx);

  protected:
    static const uint32_t queueDepth = 16;
    void *spdkDmaBuf;
    uint32_t bufSize;
    int backIdx;

    static BdevCpp::GeneralPool<SpdkIoSizedBuf, BdevCpp::ClassAlloc<SpdkIoSizedBuf>>
        writePool;
    static BdevCpp::GeneralPool<SpdkIoSizedBuf, BdevCpp::ClassAlloc<SpdkIoSizedBuf>>
        readPool;
};

template <uint32_t Size, int Inst>
SpdkIoSizedBuf<Size, Inst>::SpdkIoSizedBuf(uint32_t _bufSize, int _backIdx)
    : spdkDmaBuf(0), bufSize(_bufSize), backIdx(_backIdx) {}

template <uint32_t Size, int Inst> SpdkIoSizedBuf<Size, Inst>::~SpdkIoSizedBuf() {
    if (getSpdkDmaBuf())
        spdk_dma_free(getSpdkDmaBuf());
}

template <uint32_t Size, int Inst> SpdkIoBuf *SpdkIoSizedBuf<Size, Inst>::getWriteBuf() {
    return writePool.get();
}

template <uint32_t Size, int Inst>
void SpdkIoSizedBuf<Size, Inst>::putWriteBuf(SpdkIoBuf *ioBuf) {
    writePool.put(dynamic_cast<SpdkIoSizedBuf<Size, Inst> *>(ioBuf));
}

template <uint32_t Size, int Inst> SpdkIoBuf *SpdkIoSizedBuf<Size, Inst>::getReadBuf() {
    return readPool.get();
}

template <uint32_t Size, int Inst>
void SpdkIoSizedBuf<Size, Inst>::putReadBuf(SpdkIoBuf *ioBuf) {
    readPool.put(dynamic_cast<SpdkIoSizedBuf<Size, Inst> *>(ioBuf));
}

template <uint32_t Size, int Inst> char *SpdkIoSizedBuf<Size, Inst>::getSpdkDmaBuf() {
    return reinterpret_cast<char *>(spdkDmaBuf);
}

template <uint32_t Size, int Inst>
void SpdkIoSizedBuf<Size, Inst>::setSpdkDmaBuf(void *spdkBuf) {
    spdkDmaBuf = spdkBuf;
}

template <uint32_t Size, int Inst> int SpdkIoSizedBuf<Size, Inst>::getIdx() { return backIdx; }

template <uint32_t Size, int Inst> void SpdkIoSizedBuf<Size, Inst>::setIdx(int idx) {
    backIdx = idx;
}

template <uint32_t Size, int Inst> uint32_t SpdkIoSizedBuf<Size, Inst>::getBufSize() {
    return bufSize;
}

template <uint32_t Size, int Inst> void SpdkIoSizedBuf<Size, Inst>::setAttribs(uint32_t _bufSize, int _backIdx) {
    bufSize = _bufSize;
    backIdx = _backIdx;
}



template <uint32_t Size, int Inst>
BdevCpp::GeneralPool<SpdkIoSizedBuf<Size, Inst>,
                   BdevCpp::ClassAlloc<SpdkIoSizedBuf<Size, Inst>>>
    SpdkIoSizedBuf<Size, Inst>::writePool(queueDepth, "writeSpdkIoBufPool");

template <uint32_t Size, int Inst>
BdevCpp::GeneralPool<SpdkIoSizedBuf<Size, Inst>,
                   BdevCpp::ClassAlloc<SpdkIoSizedBuf<Size, Inst>>>
    SpdkIoSizedBuf<Size, Inst>::readPool(queueDepth, "readSpdkIoBufPool");

class SpdkIoBufMgr {
  public:
    SpdkIoBufMgr(int inst = 0);
    virtual ~SpdkIoBufMgr();

    SpdkIoBuf *getIoWriteBuf(uint32_t ioSize, uint32_t align);
    void putIoWriteBuf(SpdkIoBuf *ioBuf);
    SpdkIoBuf *getIoReadBuf(uint32_t ioSize, uint32_t align);
    void putIoReadBuf(SpdkIoBuf *ioBuf);

    static const uint32_t blockSize = 8;
    SpdkIoBuf *block[blockSize];

    static Lock instanceMutex;
    static SpdkIoBufMgr *instance;
    static SpdkIoBufMgr *getSpdkIoBufMgr();
    static void putSpdkIoBufMgr();
};

} // namespace BdevCpp
