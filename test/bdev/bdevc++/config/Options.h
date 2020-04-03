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

#include <functional>
#include <vector>

namespace BdevCpp {

enum IoDevType : std::int8_t { BDEV = 0, JBOD = 1, RAID0 = 2 };

typedef IoDevType SpdkDevConfType;

struct IoDevDescriptor {
    IoDevDescriptor() = default;
    ~IoDevDescriptor() = default;
    std::string devName;
    std::string nvmeAddr = "";
    std::string nvmeName = "";
};

struct IoOptions {
    IoDevType devType = BDEV;
    std::string name; // Unique name
    size_t allocUnitSize =
        16 * 1024; // Allocation unit size shared across the drives in a set
    size_t raid0StripeSize = 128; // Stripe size, applicable to RAIDx only
    std::vector<IoDevDescriptor>
        _devs; // List of individual drives comprising the set
};

struct Options {
  public:
    Options() {}
    explicit Options(const std::string &path);

    IoOptions io;
};

} // namespace BdevCpp
