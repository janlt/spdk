/*
 * SpdkIoEngine.h
 *
 *  Created on: Dec 17, 2019
 *      Author: parallels
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
#include "spdk/queue.h"

namespace BdevCpp {

class IoPoller;

class SpdkIoEngine : public Poller<DeviceTask> {
  public:
    SpdkIoEngine();
    virtual ~SpdkIoEngine() = default;

    void process() final;
    inline void rqstClb(const IoRqst *rqst, StatusCode status) {
        if (rqst->clb)
            rqst->clb(status, nullptr, 0);
    }
};

} // namespace BdevCpp
