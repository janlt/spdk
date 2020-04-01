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

#include <iostream>
#include <sstream>
#include <string>
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/queue.h"

#include <Poller.h>
#include <Rqst.h>
#include <SpdkBdevFactory.h>
#include <Status.h>
#include <SpdkCore.h>

namespace BdevCpp {

class IoPoller : public Poller<IoRqst> {
  public:
    IoPoller(SpdkCore *_spdkCore);
    virtual ~IoPoller();

    void process() final;

    virtual SpdkDevice *getBdev() { return spdkCore->spBdev; }
    virtual SpdkBdevCtx *getBdevCtx() { return spdkCore->spBdev->getBdevCtx(); }
    virtual spdk_bdev_desc *getBdevDesc() {
        return spdkCore->spBdev->getBdevCtx()->bdev_desc;
    }
    virtual spdk_io_channel *getBdevIoChannel() {
        return spdkCore->spBdev->getBdevCtx()->io_channel;
    }

    SpdkCore *spdkCore;
    std::atomic<int> isRunning;

    void signalReady();
    bool waitReady();

    virtual void setRunning(int rn) { isRunning = rn; }
    virtual bool isIoRunning() { return isRunning; }

  private:

    void _processGet(IoRqst *rqst);
    void _processUpdate(IoRqst *rqst);
    void _processRemove(IoRqst *rqst);

    inline void _rqstClb(const IoRqst *rqst, StatusCode status) {
        if (rqst->clb)
            rqst->clb(status, nullptr, 0);
    }
};

} // namespace BdevCpp
