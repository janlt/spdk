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
    : _state(Io2Poller::State::FP_READY), FinPoller() {}

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
                    IoRqst::getPool.put(task->rqst);
                else
                    _processGet(task);
                break;
            case IoOp::UPDATE:
                if (dropIt == true)
                    IoRqst::updatePool.put(task->rqst);
                else
                    _processUpdate(task);
                break;
            case IoOp::DELETE:
                if (dropIt == true)
                    IoRqst::removePool.put(task->rqst);
                else
                    _processRemove(task);
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

void Io2Poller::_processGet(DeviceTask *task) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);

    if (task->result) {
        if (task->clb)
            task->clb(StatusCode::OK,
                      task->buff->getSpdkDmaBuf(), task->rqst->dataSize);
    } else {
        if (task->clb)
            task->clb(StatusCode::UNKNOWN_ERROR, nullptr, 0);
    }

    bdev->ioPoolMgr->putIoReadBuf(task->buff);
    bdev->ioBufsInUse--;
    IoRqst::getPool.put(task->rqst);
}

void Io2Poller::_processUpdate(DeviceTask *task) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);
    DeviceAddr devAddr;
    devAddr.busAddr.pciAddr = bdev->spBdevCtx.pci_addr;
    devAddr.lba = task->freeLba;

    if (task->result == true) {
        if (task->clb)
            task->clb(StatusCode::OK, nullptr, 0);
    } else {
        if (task->clb)
            task->clb(StatusCode::UNKNOWN_ERROR, nullptr, 0);
    }

    IoRqst::updatePool.put(task->rqst);
}

void Io2Poller::_processRemove(DeviceTask *task) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);

    if (task->result == true) {
        if (task->clb)
            task->clb(StatusCode::OK, nullptr, 0);
    } else {
        if (task->clb)
            task->clb(StatusCode::UNKNOWN_ERROR, nullptr, 0);
    }

    IoRqst::removePool.put(task->rqst);
}

} // namespace BdevCpp
