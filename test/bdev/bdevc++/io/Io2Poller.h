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

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "spdk/bdev.h"
#include "spdk/env.h"
#include "spdk/io_channel.h"
#include "spdk/queue.h"

#include "BlockingPoller.h"
#include "Poller.h"
#include "Rqst.h"
#include "SpdkDevice.h"
#include "SpdkBdev.h"
#include "SpdkConf.h"
#include "SpdkIoBuf.h"

namespace BdevCpp {

typedef Poller<DeviceTask> FinPoller;

class Io2Poller : public FinPoller {
  public:
    Io2Poller();
    virtual ~Io2Poller() = default;

    void process() final;

    enum State { FP_READY = 0, FP_QUIESCING, FP_QUIESCENT };

    void Quiesce() { _state = Io2Poller::State::FP_QUIESCING; }
    bool isQuiescent() {
        return _state == Io2Poller::State::FP_QUIESCENT ? true : false;
    }

  protected:
    void _processGet(DeviceTask *task);
    void _processUpdate(DeviceTask *task);
    void _processRemove(DeviceTask *task);

  private:
    std::atomic<State> _state;
};

} // namespace BdevCpp
