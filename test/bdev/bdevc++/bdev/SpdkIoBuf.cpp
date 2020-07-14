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

#include "spdk/env.h"

#include "SpdkIoBuf.h"

namespace BdevCpp {

SpdkIoBufMgr::~SpdkIoBufMgr() {}

SpdkIoBuf *SpdkIoBufMgr::getIoWriteBuf(uint32_t ioSize, uint32_t align) {
    SpdkIoBuf *buf = 0;
    uint32_t bufIdx = !(ioSize%4096) ? (ioSize/4096 - 1) : ioSize/4096;
    if (bufIdx < 8) {
        buf = block[bufIdx]->getWriteBuf();
        if (!buf->getSpdkDmaBuf())
            buf->setSpdkDmaBuf(
                spdk_dma_zmalloc(static_cast<size_t>(block[bufIdx]->getBufSize()), align, NULL));
    } else {
        buf = new SpdkIoSizedBuf<1 << 16, 0>(1 << 16, -1);
        buf->setSpdkDmaBuf(
            spdk_dma_zmalloc(static_cast<size_t>(ioSize), align, NULL));
    }
    return buf;
}

void SpdkIoBufMgr::putIoWriteBuf(SpdkIoBuf *ioBuf) {
    if (ioBuf->getIdx() != -1)
        ioBuf->putWriteBuf(ioBuf);
    else {
        delete ioBuf;
    }
}

SpdkIoBuf *SpdkIoBufMgr::getIoReadBuf(uint32_t ioSize, uint32_t align) {
    SpdkIoBuf *buf = 0;
    uint32_t bufIdx = !(ioSize%4096) ? (ioSize/4096 - 1) : ioSize/4096;
    if (bufIdx < 8) {
        buf = block[bufIdx]->getReadBuf();
        if (!buf->getSpdkDmaBuf())
            buf->setSpdkDmaBuf(
                spdk_dma_zmalloc(static_cast<size_t>(block[bufIdx]->getBufSize()), align, NULL));
    } else {
        buf = new SpdkIoSizedBuf<1 << 16, 0>(1 << 16, -1);
        buf->setSpdkDmaBuf(
            spdk_dma_zmalloc(static_cast<size_t>(ioSize), align, NULL));
    }
    return buf;
}

void SpdkIoBufMgr::putIoReadBuf(SpdkIoBuf *ioBuf) {
    if (ioBuf->getIdx() != -1)
        ioBuf->putReadBuf(ioBuf);
    else
        delete ioBuf;
}

Lock SpdkIoBufMgr::instanceMutex;
SpdkIoBufMgr *SpdkIoBufMgr::instance = 0;
SpdkIoBufMgr *SpdkIoBufMgr::getSpdkIoBufMgr() {
    if (!SpdkIoBufMgr::instance) {
        WriteLock r_lock(instanceMutex);
        SpdkIoBufMgr::instance = new SpdkIoBufMgr();
    }
    return SpdkIoBufMgr::instance;
}

void SpdkIoBufMgr::putSpdkIoBufMgr() {
    if (SpdkIoBufMgr::instance) {
        WriteLock r_lock(instanceMutex);
        delete SpdkIoBufMgr::instance;
        SpdkIoBufMgr::instance = 0;
    }
}

SpdkIoBufMgr::SpdkIoBufMgr(int inst) {
    switch (inst) {
    case 0:
        block[0] = new SpdkIoSizedBuf<4096, 0>(4096, 0);
        block[1] = new SpdkIoSizedBuf<2 * 4096, 0>(2 * 4096, 1);
        block[2] = new SpdkIoSizedBuf<3 * 4096, 0>(3 * 4096, 2);
        block[3] = new SpdkIoSizedBuf<4 * 4096, 0>(4 * 4096, 3);
        block[4] = new SpdkIoSizedBuf<5 * 4096, 0>(5 * 4096, 4);
        block[5] = new SpdkIoSizedBuf<6 * 4096, 0>(6 * 4096, 5);
        block[6] = new SpdkIoSizedBuf<7 * 4096, 0>(7 * 4096, 6);
        block[7] = new SpdkIoSizedBuf<8 * 4096, 0>(8 * 4096, 7);
        break;
    case 1:
        block[0] = new SpdkIoSizedBuf<4096, 1>(4096, 0);
        block[1] = new SpdkIoSizedBuf<2 * 4096, 1>(2 * 4096, 1);
        block[2] = new SpdkIoSizedBuf<3 * 4096, 1>(3 * 4096, 2);
        block[3] = new SpdkIoSizedBuf<4 * 4096, 1>(4 * 4096, 3);
        block[4] = new SpdkIoSizedBuf<5 * 4096, 1>(5 * 4096, 4);
        block[5] = new SpdkIoSizedBuf<6 * 4096, 1>(6 * 4096, 5);
        block[6] = new SpdkIoSizedBuf<7 * 4096, 1>(7 * 4096, 6);
        block[7] = new SpdkIoSizedBuf<8 * 4096, 1>(8 * 4096, 7);
        break;
    case 2:
        block[0] = new SpdkIoSizedBuf<4096, 2>(4096, 0);
        block[1] = new SpdkIoSizedBuf<2 * 4096, 2>(2 * 4096, 1);
        block[2] = new SpdkIoSizedBuf<3 * 4096, 2>(3 * 4096, 2);
        block[3] = new SpdkIoSizedBuf<4 * 4096, 2>(4 * 4096, 3);
        block[4] = new SpdkIoSizedBuf<5 * 4096, 2>(5 * 4096, 4);
        block[5] = new SpdkIoSizedBuf<6 * 4096, 2>(6 * 4096, 5);
        block[6] = new SpdkIoSizedBuf<7 * 4096, 2>(7 * 4096, 6);
        block[7] = new SpdkIoSizedBuf<8 * 4096, 2>(8 * 4096, 7);
        break;
    case 3:
        block[0] = new SpdkIoSizedBuf<4096, 3>(4096, 0);
        block[1] = new SpdkIoSizedBuf<2 * 4096, 3>(2 * 4096, 1);
        block[2] = new SpdkIoSizedBuf<3 * 4096, 3>(3 * 4096, 2);
        block[3] = new SpdkIoSizedBuf<4 * 4096, 3>(4 * 4096, 3);
        block[4] = new SpdkIoSizedBuf<5 * 4096, 3>(5 * 4096, 4);
        block[5] = new SpdkIoSizedBuf<6 * 4096, 3>(6 * 4096, 5);
        block[6] = new SpdkIoSizedBuf<7 * 4096, 3>(7 * 4096, 6);
        block[7] = new SpdkIoSizedBuf<8 * 4096, 3>(8 * 4096, 7);
        break;
    case 4:
        block[0] = new SpdkIoSizedBuf<4096, 4>(4096, 0);
        block[1] = new SpdkIoSizedBuf<2 * 4096, 4>(2 * 4096, 1);
        block[2] = new SpdkIoSizedBuf<3 * 4096, 4>(3 * 4096, 2);
        block[3] = new SpdkIoSizedBuf<4 * 4096, 4>(4 * 4096, 3);
        block[4] = new SpdkIoSizedBuf<5 * 4096, 4>(5 * 4096, 4);
        block[5] = new SpdkIoSizedBuf<6 * 4096, 4>(6 * 4096, 5);
        block[6] = new SpdkIoSizedBuf<7 * 4096, 4>(7 * 4096, 6);
        block[7] = new SpdkIoSizedBuf<8 * 4096, 4>(8 * 4096, 7);
        break;
    case 5:
        block[0] = new SpdkIoSizedBuf<4096, 5>(4096, 0);
        block[1] = new SpdkIoSizedBuf<2 * 4096, 5>(2 * 4096, 1);
        block[2] = new SpdkIoSizedBuf<3 * 4096, 5>(3 * 4096, 2);
        block[3] = new SpdkIoSizedBuf<4 * 4096, 5>(4 * 4096, 3);
        block[4] = new SpdkIoSizedBuf<5 * 4096, 5>(5 * 4096, 4);
        block[5] = new SpdkIoSizedBuf<6 * 4096, 5>(6 * 4096, 5);
        block[6] = new SpdkIoSizedBuf<7 * 4096, 5>(7 * 4096, 6);
        block[7] = new SpdkIoSizedBuf<8 * 4096, 5>(8 * 4096, 7);
        break;
    case 6:
        block[0] = new SpdkIoSizedBuf<4096, 6>(4096, 0);
        block[1] = new SpdkIoSizedBuf<2 * 4096, 6>(2 * 4096, 1);
        block[2] = new SpdkIoSizedBuf<3 * 4096, 6>(3 * 4096, 2);
        block[3] = new SpdkIoSizedBuf<4 * 4096, 6>(4 * 4096, 3);
        block[4] = new SpdkIoSizedBuf<5 * 4096, 6>(5 * 4096, 4);
        block[5] = new SpdkIoSizedBuf<6 * 4096, 6>(6 * 4096, 5);
        block[6] = new SpdkIoSizedBuf<7 * 4096, 6>(7 * 4096, 6);
        block[7] = new SpdkIoSizedBuf<8 * 4096, 6>(8 * 4096, 7);
        break;
    case 7:
        block[0] = new SpdkIoSizedBuf<4096, 7>(4096, 0);
        block[1] = new SpdkIoSizedBuf<2 * 4096, 7>(2 * 4096, 1);
        block[2] = new SpdkIoSizedBuf<3 * 4096, 7>(3 * 4096, 2);
        block[3] = new SpdkIoSizedBuf<4 * 4096, 7>(4 * 4096, 3);
        block[4] = new SpdkIoSizedBuf<5 * 4096, 7>(5 * 4096, 4);
        block[5] = new SpdkIoSizedBuf<6 * 4096, 7>(6 * 4096, 5);
        block[6] = new SpdkIoSizedBuf<7 * 4096, 7>(7 * 4096, 6);
        block[7] = new SpdkIoSizedBuf<8 * 4096, 7>(8 * 4096, 7);
        break;
    }
}

} // namespace BdevCpp
