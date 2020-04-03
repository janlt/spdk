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
#include <string.h>
#include <string>

#include <vector>

#include "Status.h"
#include "Options.h"
#include "Rqst.h"
#include "SpdkDevice.h"
#include "SpdkConf.h"
#include "SpdkBdev.h"

namespace BdevCpp {

SpdkConf::SpdkConf(const IoOptions &_ioOptions)
    : _devType(_ioOptions.devType), _name(_ioOptions.name),
      _raid0StripeSize(_ioOptions.raid0StripeSize), _bdev(0),
      _bdevNum(-1) {
    copyDevs(_ioOptions._devs);
}

SpdkConf::SpdkConf(IoDevType devType, std::string name,
                   size_t raid0StripeSize)
    : _devType(devType), _name(name), _raid0StripeSize(raid0StripeSize) {}

struct PciAddr SpdkConf::parsePciAddr(const std::string &nvmeAddr) {
    struct PciAddr addr;
    int ret = sscanf(nvmeAddr.c_str(), "%x:%2hhx:%2hhx.%2hhx", &addr.domain, &addr.bus,
                     &addr.dev, &addr.func);
    if (ret != 4)
        memset(&addr, 0xff, sizeof(addr));
    return addr;
}

void SpdkConf::copyDevs(const std::vector<IoDevDescriptor> &_ioDevs) {
    for (auto d : _ioDevs) {
        _devs.push_back(SpdkBdevConf{d.devName, d.nvmeAddr,
                                     parsePciAddr(d.nvmeAddr), d.nvmeName});
    }
}

const std::string &SpdkConf::getBdevNvmeName() const {
    return _devs[0].nvmeName;
}

const std::string &SpdkConf::getBdevNvmeAddr() const {
    return _devs[0].nvmeAddr;
}

struct PciAddr SpdkConf::getBdevSpdkPciAddr() const {
    return _devs[0].pciAddr;
}

SpdkConf &SpdkConf::operator=(const SpdkConf &_r) {
    if (this == &_r)
        return *this;
    this->_devType = _r._devType;
    this->_name = _r._name;
    this->_devs = _r._devs;
    this->_bdevNum = _r._bdevNum;
    return *this;
}

void SpdkConf::addDev(SpdkBdevConf dev) { _devs.push_back(dev); }

} // namespace BdevCpp
