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
friend class ReadFuturePolling;
friend class WriteFuturePolling;

  private:
    FutureBase();
    virtual ~FutureBase();

  public:
    virtual int get(char *&data, size_t &_dataSize, unsigned int _timeoutMsec = 100) = 0;
    virtual void signal(StatusCode status, const char *data, size_t _dataSize) = 0;
    virtual char *getData() = 0;
    virtual size_t getDataSize() = 0;
    virtual void setBuffer(char *_buffer) = 0;
    virtual void setBufferSize(size_t _bufferSize) = 0;
    virtual void setDataSize(size_t _dataSize) = 0;
    virtual void sink() = 0;
    virtual void reset() = 0;

  protected:
    int opStatus;
    IoRqst ioRqst;
};

} // namespace BdevCpp
