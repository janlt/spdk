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
#include <memory>
#include <thread>

#include "spdk/bdev.h"

namespace BdevCpp {

struct BdevStats {
    uint64_t write_compl_cnt;
    uint64_t write_err_cnt;
    uint64_t read_compl_cnt;
    uint64_t read_err_cnt;
    bool periodic = true;
    uint64_t quant_per = (1 << 18);
    uint64_t outstanding_io_cnt;

    BdevStats()
        : write_compl_cnt(0), write_err_cnt(0), read_compl_cnt(0),
          read_err_cnt(0), outstanding_io_cnt(0) {}
    std::ostringstream &formatWriteBuf(std::ostringstream &buf,
                                       const char *bdev_addr);
    std::ostringstream &formatReadBuf(std::ostringstream &buf,
                                      const char *bdev_addr);
    void printWritePer(std::ostream &os, const char *bdev_addr);
    void printReadPer(std::ostream &os, const char *bdev_addr);
};

} // namespace BdevCpp
