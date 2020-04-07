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


#define LAYOUT "queue"
#define CREATE_MODE_RW (S_IWUSR | S_IRUSR)

namespace BdevCpp {

IoPoller::IoPoller(SpdkCore *_spdkCore)
    : Poller<IoRqst>(false), spdkCore(_spdkCore) {}

IoPoller::~IoPoller() {
    isRunning = 0;
}

void IoPoller::_processRead(IoRqst *rqst) {
    SpdkDevice *spdkDev = getBdev();

    DeviceTask *ioTask =
        new (rqst->taskBuffer) DeviceTask{0,
            rqst->dataSize,
            spdkDev->getBlockSize(),
            0,
            rqst->clb,
            spdkDev,
            rqst,
            IoOp::READ,
            rqst->lba,
            rqst->lun};

    if (spdkDev->read(ioTask) != true) {
        _rqstClb(rqst, StatusCode::UNKNOWN_ERROR);
        IoRqst::readPool.put(rqst);
    }
}

void IoPoller::_processWrite(IoRqst *rqst) {
    DeviceTask *ioTask = nullptr;

    if (rqst == nullptr) {
        _rqstClb(rqst, StatusCode::UNKNOWN_ERROR);
        return;
    }

    SpdkDevice *spdkDev = getBdev();

    if (rqst->dataSize == 0) {
        _rqstClb(rqst, StatusCode::OK);
        IoRqst::writePool.put(rqst);
        return;
    }

    ioTask = new (rqst->taskBuffer)
        DeviceTask{0,
            rqst->dataSize,
            spdkDev->getBlockSize(),
            new (rqst->devAddrBuf) DeviceAddr,
            rqst->clb,
            spdkDev,
            rqst,
            IoOp::WRITE,
            rqst->lba,
            rqst->lun};

    if (spdkDev->write(ioTask) != true) {
        _rqstClb(rqst, StatusCode::UNKNOWN_ERROR);
        IoRqst::writePool.put(rqst);
    }
}

void IoPoller::process() {
    if (requestCount > 0) {
        for (unsigned short RqstIdx = 0; RqstIdx < requestCount; RqstIdx++) {
            IoRqst *rqst = requests[RqstIdx];
            switch (rqst->op) {
            case IoOp::READ:
                _processRead(rqst);
                break;
            case IoOp::WRITE: {
                _processWrite(rqst);
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
