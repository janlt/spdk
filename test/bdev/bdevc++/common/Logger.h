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

/** @todo implement logging categories */
#ifdef IO_DEBUG
#define IOP_INFO(msg) gLog.log(msg)
#else
#define IOP_INFO(msg) 
#endif

#ifdef IO_DEBUG
#define IOP_CRITICAL(msg) gLog.log(msg)
#else
#define IOP_CRITICAL(msg) 
#endif

#ifdef IO_DEBUG
#define IOP_DEBUG(msg) gLog.log(msg)
#else
#define IOP_DEBUG(msg)
#endif

#include <functional>

namespace BdevCpp {

class Logger {
  public:
    Logger();
    virtual ~Logger();

    void setLogFunc(const std::function<void(std::string)> &fn);
    void log(std::string);

  private:
    std::function<void(std::string)> _logFunc = nullptr;
};

} // namespace BdevCpp

extern BdevCpp::Logger gLog;
