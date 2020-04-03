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

#include "BdevStats.h"
#include "SpdkBdev.h"
#include "SpdkIoEngine.h"
#include <Io2Poller.h>
#include <Status.h>

namespace BdevCpp {

SpdkIoEngine::SpdkIoEngine() : Poller<DeviceTask>(true, SPDK_RING_TYPE_SP_SC) {}

void SpdkIoEngine::process() {
    if (requestCount > 0) {
        for (unsigned short RqstIdx = 0; RqstIdx < requestCount; RqstIdx++) {
            DeviceTask *task = requests[RqstIdx];
            task->routing = false;
            SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);
            switch (task->op) {
            case IoOperation::GET: {
                bool ret = bdev->read(task);
                if (ret != true) {
                    rqstClb(task->rqst, StatusCode::UNKNOWN_ERROR);
                    IoRqst::getPool.put(task->rqst);
                }
            } break;
            case IoOperation::UPDATE: {
                bool ret = bdev->write(task);
                if (ret != true) {
                    rqstClb(task->rqst, StatusCode::UNKNOWN_ERROR);
                    IoRqst::updatePool.put(task->rqst);
                }
            } break;
            case IoOperation::REMOVE: {
                bool ret = bdev->remove(task);
                if (ret != true) {
                    rqstClb(task->rqst, StatusCode::UNKNOWN_ERROR);
                    IoRqst::removePool.put(task->rqst);
                }
            } break;
            default:
                break;
            }
        }
        requestCount = 0;
    }
}

} // namespace BdevCpp
