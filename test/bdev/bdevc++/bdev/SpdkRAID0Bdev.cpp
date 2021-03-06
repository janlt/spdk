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

#include "spdk/stdinc.h"
#include "spdk/cpuset.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/ftl.h"
#include "spdk/conf.h"
#include "spdk/env.h"

#include "Status.h"
#include "Options.h"
#include "Rqst.h"
#include "Poller.h"
#include "SpdkDevice.h"
#include "SpdkConf.h"
#include "SpdkBdev.h"
#include "SpdkRAID0Bdev.h"
#include "Logger.h"

namespace BdevCpp {

SpdkDeviceClass SpdkRAID0Bdev::bdev_class = SpdkDeviceClass::RAID0;

SpdkRAID0Bdev::SpdkRAID0Bdev() : isRunning(0) {}

bool SpdkRAID0Bdev::read(DeviceTask *task) { return true; }

bool SpdkRAID0Bdev::write(DeviceTask *task) { return true; }

int SpdkRAID0Bdev::reschedule(DeviceTask *task) { return 0; }

void SpdkRAID0Bdev::deinit() {}

bool SpdkRAID0Bdev::init(const SpdkConf &conf) { return true; }

void SpdkRAID0Bdev::enableStats(bool en) {}

void SpdkRAID0Bdev::setMaxQueued(uint32_t io_cache_size, uint32_t blk_size) {
    IoBytesMaxQueued = io_cache_size * 128;
}

void SpdkRAID0Bdev::getBdevGeom(BdevGeom &geom) { return; }

} // namespace BdevCpp
