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

#include <string>
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>

#include "spdk/conf.h"
#include "spdk/cpuset.h"
#include "spdk/env.h"
#include "spdk/event.h"
#include "spdk/ftl.h"
#include "spdk/log.h"
#include "spdk/queue.h"
#include "spdk/stdinc.h"
#include "spdk/thread.h"

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
#include "AsyncApi.h"
#include "Api.h"

using namespace std;

namespace BdevCpp {

SyncApi *Api::syncApi = 0;
AsyncApi *Api::asyncApi = 0;

Api::Api(const Options &options)
    : spc(0), spio(0) {
    spc = new SpdkCore(options.io);
    if ( spc->isBdevFound() == true ) {
        cout << "Io functionality is enabled" << endl;
    } else {
        cerr << "Io functionality is disabled" << endl;
        return;
    }

    if (spc->isBdevFound() == false) {
        cerr << "Bdev not found" << endl;
        delete spc;
        return;
    }

    spio = new IoPoller(spc);

    spc->setPoller(spio);
    if (spc->isSpdkReady() == true) {
        spc->startSpdk();
        spc->waitReady(); // synchronize until SpdkCore is done initializing SPDK framework
    }

    spc->getBdev()->getBdevGeom(storageGeom);

    unique_lock<Lock> r_lock(instanceMutex);

    if (!Api::syncApi) {
        try {
            Api::syncApi = new SyncApi(spio);
            Api::syncApi->storageGeom = &storageGeom;
        } catch (...) {
            cerr << "Instantiating SyncApi failed" << endl;
            Api::syncApi = 0;
        }
    }

    if (!Api::asyncApi) {
        try {
            Api::asyncApi = new AsyncApi(spio);
            Api::asyncApi->storageGeom = &storageGeom;
        } catch (...) {
            cerr << "Instantiating AsyncApi failed" << endl;
            Api::asyncApi = 0;
        }
    }
}

Api::~Api() {
    unique_lock<Lock> r_lock(instanceMutex);

    if (Api::syncApi) {
        delete Api::syncApi;
        Api::syncApi = 0;
    }
    if (Api::asyncApi) {
        delete Api::asyncApi;
        Api::asyncApi = 0;
    }
}

} // namespace BdevCpp
