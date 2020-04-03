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

#include <iostream>

#include "Config.h"
#include <libconfig.h++>

using namespace libconfig;

namespace BdevCpp {

bool readConfig(const std::string &configFile, BdevCpp::Options &options) {
    std::stringstream ss;
    return readConfig(configFile, options, ss);
}

bool readConfig(const std::string &configFile, BdevCpp::Options &options,
                       std::stringstream &ss) {
    Config cfg;
    try {
        cfg.readFile(configFile.c_str());
    } catch (const libconfig::FileIOException &fioex) {
        ss << "I/O error while reading file.\n";
        return false;
    } catch (const libconfig::ParseException &pex) {
        ss << "Parse error at " << pex.getFile() << ":" << pex.getLine()
           << " - " << pex.getError();
        return false;
    }

    int ioAllocUnitSize;
    if (cfg.lookupValue("io_unit_alloc_size", ioAllocUnitSize))
        options.io.allocUnitSize = ioAllocUnitSize;
    std::string dev_type;
    cfg.lookupValue("io_dev_type", dev_type);
    if (dev_type == "bdev")
        options.io.devType = IoDevType::BDEV;
    else if (dev_type == "jbod")
        options.io.devType = IoDevType::JBOD;
    else if (dev_type == "raid0")
        options.io.devType = IoDevType::RAID0;
    else {
        ss << "Parse error, invalid io dev type " << dev_type;
        return false;
    }
    std::string io_name;
    if (cfg.lookupValue("io_dev_name", io_name))
        options.io.name = io_name;
    else
        options.io.name = "anonymous";

    IoDevDescriptor io_desc;
    switch (options.io.devType) {
    case IoDevType::BDEV:
        if (cfg.lookupValue("io_nvme_addr", io_desc.nvmeAddr) &&
            cfg.lookupValue("io_nvme_name", io_desc.nvmeName)) {
            options.io._devs.push_back(io_desc);
        } else {
            ss << "No nvme device found for BDEV in io";
            return false;
        }
        break;
    case IoDevType::JBOD:
    case IoDevType::RAID0: {
        const Setting &root = cfg.getRoot();
        const Setting &devices = root["devices"];
        int count = devices.getLength();
        for (int i = 0; i < count; i++) {
            const Setting &device = devices[i];
            io_desc.devName = device.getName();
            ;
            if (!device.lookupValue("io_nvme_addr",
                                    io_desc.nvmeAddr) ||
                !device.lookupValue("io_nvme_name", io_desc.nvmeName))
                continue;
            options.io._devs.push_back(io_desc);
        }
        if (!options.io._devs.size()) {
            ss << "No nvme device found in devices for io";
            return false;
        }
        int stripeSize = 128;
        if (options.io.devType == IoDevType::RAID0) {
            if (cfg.lookupValue("io_raid0_stripe_size", stripeSize))
                options.io.raid0StripeSize =
                    static_cast<size_t>(stripeSize);
        }
    } break;
    default:
        ss << "Invalid or unsupported device type[" << options.io.devType
           << "] for io";
        return false;
        break;
    }

    return true;
}

} // namespace BdevCpp
