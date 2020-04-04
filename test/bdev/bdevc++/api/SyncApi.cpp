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
#include <time.h>

#include <iostream>
#include <sstream>
#include <string>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

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
#include "spdk/queue.h"

#include "Status.h"
#include "Options.h"
#include "BlockingPoller.h"
#include "Poller.h"
#include "IoPoller.h"
#include "Io2Poller.h"
#include "Rqst.h"
#include "SpdkDevice.h"
#include "SpdkConf.h"
#include "SpdkBdev.h"
#include "SpdkIoBuf.h"
#include "SpdkCore.h"

#include "ApiBase.h"
#include "SyncApi.h"

using namespace std;

namespace BdevCpp {

SyncApi::SyncApi(const Options &options) {
    SpdkCore *spc = new SpdkCore(options.io);
    if ( spc->isBdevFound() == true ) {
        cout << "Io functionality is enabled" << endl;
    } else {
        cerr << "Io functionality is disabled" << endl;
        return;
    }

    if (spc->isBdevFound() == false) {
        cerr << "Bdev not found" << endl;
        return;
    }

    IoPoller *spio = new IoPoller(spc);

    spc->setPoller(spio);
    if (spc->isSpdkReady() == true) {
        spc->startSpdk();
        spc->waitReady(); // synchronize until SpdkCore is done initializing SPDK framework
    }
}

SyncApi::~SyncApi() {}

} // namespace BdevCpp
