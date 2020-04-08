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

class ReadFuture: public FutureBase {
friend class Api;
friend class AsyncApi;

  private:
    ReadFuture(char *_buffer = 0, size_t _bufferSize = 0);
    virtual ~ReadFuture();

  public:
    int get(Status status, const char *data, size_t _dataSize);
    char *getData() {
        return buffer;
    }
    size_t getDataSize() {
        return bufferSize;
    }

  private:
    char *buffer;
    size_t bufferSize;
};

class WriteFuture: public FutureBase {
friend class Api;
friend class AsyncApi;

  private:
    WriteFuture(size_t _dataSize);
    virtual ~WriteFuture();

  public:
    int get(Status status, const char *data, size_t _dataSize);
    char *getData() {
        return 0;
    }
    size_t getDataSize() {
        return dataSize;
    }

  private:
    size_t dataSize;
};

} // namespace BdevCpp
