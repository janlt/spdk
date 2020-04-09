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

namespace BdevCpp {

class FutureBase {
friend class Api;
friend class AsyncApi;
friend class ReadFuture;
friend class WriteFuture;

  private:
    FutureBase();
    virtual ~FutureBase();

  protected:
    virtual int get(char *&data, size_t &_dataSize) = 0;
    virtual void signal(Status status, const char *data, size_t _dataSize) = 0;
    virtual char *getData() = 0;
    virtual size_t getDataSize() = 0;
    virtual void sink() = 0;

  protected:
    std::mutex mtx;
    std::condition_variable cv;
    int opStatus;
};

} // namespace BdevCpp
