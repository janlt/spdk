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

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "spdk/conf.h"
#include "spdk/cpuset.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/ftl.h"
#include "spdk/log.h"
#include "spdk/queue.h"
#include "spdk/stdinc.h"
#include "spdk/thread.h"

#include "Io2Poller.h"

namespace BdevCpp {

Io2Poller::Io2Poller()
    : FinPoller(), _state(Io2Poller::State::FP_READY) {}

void Io2Poller::process() {
    if (requestCount > 0) {
        for (unsigned short RqstIdx = 0; RqstIdx < requestCount; RqstIdx++) {
            DeviceTask *task = requests[RqstIdx];
            if (!task) // due to possible timeout
                continue;

            bool dropIt = false;
            if (_state != Io2Poller::State::FP_READY)
                dropIt = true;

            switch (task->op) {
            case IoOp::READ:
                if (dropIt == true)
                    IoRqst::readPool.put(task->rqst);
                else
                    _processRead(task);
                break;
            case IoOp::WRITE:
                if (dropIt == true)
                    IoRqst::writePool.put(task->rqst);
                else
                    _processWrite(task);
                break;
            default:
                break;
            }
        }
        requestCount = 0;
    } else {
        if (_state == Io2Poller::State::FP_QUIESCENT)
            _state = Io2Poller::State::FP_QUIESCENT;
    }
}

void Io2Poller::_processRead(DeviceTask *task) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);

    if (task->result == true) {
        if (task->clb)
            task->clb(StatusCode::OK,
                      task->buff->getSpdkDmaBuf(), task->size);
    } else {
        if (task->clb)
            task->clb(StatusCode::UNKNOWN_ERROR, nullptr, 0);
    }

    bdev->ioPoolMgr->putIoReadBuf(task->buff);
    bdev->ioBufsInUse--;
    IoRqst::readPool.put(task->rqst);
}

void Io2Poller::_processWrite(DeviceTask *task) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);
    DeviceAddr devAddr;
    devAddr.busAddr.pciAddr = bdev->spBdevCtx.pci_addr;
    devAddr.lba = task->lba;

    if (task->result == true) {
        if (task->clb)
            task->clb(StatusCode::OK, nullptr, task->size);
    } else {
        if (task->clb)
            task->clb(StatusCode::UNKNOWN_ERROR, nullptr, 0);
    }

    IoRqst::writePool.put(task->rqst);
}

} // namespace BdevCpp
