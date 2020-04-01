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
#include <pthread.h>

#include <thread>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

#include "spdk/stdinc.h"
#include "spdk/cpuset.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/ftl.h"
#include "spdk/conf.h"
#include "spdk/env.h"

#include "IoPoller.h"
#include <Logger.h>
#include <Status.h>


#define LAYOUT "queue"
#define CREATE_MODE_RW (S_IWUSR | S_IRUSR)

namespace BdevCpp {

IoPoller::IoPoller(SpdkCore *_spdkCore)
    : Poller<IoRqst>(false), spdkCore(_spdkCore) {}

IoPoller::~IoPoller() {
    isRunning = 0;
}

void IoPoller::_processGet(IoRqst *rqst) {
    SpdkDevice *spdkDev = getBdev();

    DeviceTask *ioTask =
        new (rqst->taskBuffer) DeviceTask{0,
                                          spdkDev->getOptimalSize(rqst->dataSize),
                                          0,
                                          static_cast<DeviceAddr *>(valCtx.val),
                                          nullptr,
                                          false,
                                          rqst->clb,
                                          spdkDev,
                                          rqst,
                                          IoOp::READ};

    if (spdkDev->read(ioTask) != true) {
        _rqstClb(rqst, StatusCode::UNKNOWN_ERROR);
        IoRqst::getPool.put(rqst);
    }
}

void IoPoller::_processUpdate(IoRqst *rqst) {
    DeviceTask *ioTask = nullptr;

    if (rqst == nullptr) {
        _rqstClb(rqst, StatusCode::UNKNOWN_ERROR);
        return;
    }

    SpdkDevice *spdkDev = getBdev();
    auto valSizeAlign = spdkDev->getAlignedSize(rqst->valueSize);

    if (rqst->valueSize == 0) {
        _rqstClb(rqst, StatusCode::OK);
        IoRqst::updatePool.put(rqst);
        return;
    }

    ioTask = new (rqst->taskBuffer)
        DeviceTask{0,
                   spdkDev->getOptimalSize(rqst->dataSize),
                   spdkDev->getSizeInBlk(valSizeAlign),
                   new (rqst->devAddrBuf) DeviceAddr,
                   false,
                   rqst->clb,
                   spdkDev,
                   rqst,
                   IoOp::UPDATE};

    if (spdkDev->write(ioTask) != true) {
        _rqstClb(rqst, StatusCode::UNKNOWN_ERROR);
        IoRqst::updatePool.put(rqst);
    }
}

void IoPoller::_processRemove(IoRqst *rqst) {

    ValCtx valCtx;

    auto rc = _getValCtx(rqst, valCtx);
    if (rc != StatusCode::OK) {
        _rqstClb(rqst, rc);
        if (rqst->valueSize > 0)
            delete[] rqst->value;
        IoRqst::removePool.put(rqst);
        return;
    }

    if (valCtx.location == LOCATIONS::PMEM) {
        _rqstClb(rqst, StatusCode::KEY_NOT_FOUND);
        if (rqst->valueSize > 0)
            delete[] rqst->value;
        IoRqst::removePool.put(rqst);
        return;
    }

    uint64_t lba = *(static_cast<uint64_t *>(valCtx.val));

    SpdkDevice *spdkDev = getBdev();
    DeviceTask *ioTask = new (rqst->taskBuffer)
        DeviceTask{0,
                   spdkDev->getOptimalSize(rqst->valueSize),
                   0,
                   static_cast<DeviceAddr *>(valCtx.val),
                   false,
                   rqst->clb,
                   spdkDev,
                   rqst,
                   IoOp::DELETE};

    if (spdkDev->remove(ioTask) != true) {
        _rqstClb(rqst, StatusCode::UNKNOWN_ERROR);
        IoRqst::removePool.put(rqst);
    }
}

void IoPoller::process() {
    if (requestCount > 0) {
        for (unsigned short RqstIdx = 0; RqstIdx < requestCount; RqstIdx++) {
            IoRqst *rqst = requests[RqstIdx];
            switch (rqst->op) {
            case IoOp::READ:
                _processGet(rqst);
                break;
            case IoOp::UPDATE: {
                _processUpdate(rqst);
                break;
            }
            case IoOp::DELETE: {
                _processRemove(rqst);
                break;
            }
            default:
                break;
            }
        }
        requestCount = 0;
    }
}

} // namespace BdevCpp
