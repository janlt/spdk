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
friend class ClassAlloc<ReadFuture>;

  private:
    ReadFuture(char *_buffer = 0, size_t _bufferSize = 0);
    virtual ~ReadFuture();

  public:
    int get(char *&data, size_t &_dataSize);
    void signal(Status status, const char *data, size_t _dataSize);
    char *getData() {
        return buffer;
    }
    size_t getDataSize() {
        return bufferSize;
    }
    void sink();

  private:
    char *buffer;
    size_t bufferSize;

    static BdevCpp::GeneralPool<ReadFuture, BdevCpp::ClassAlloc<ReadFuture>> readFuturePool;
};

class WriteFuture: public FutureBase {
friend class Api;
friend class AsyncApi;
friend class ClassAlloc<WriteFuture>;

  private:
    WriteFuture(size_t _dataSize = 0);
    virtual ~WriteFuture();

  public:
    int get(char *&data, size_t &_dataSize);
    void signal(Status status, const char *data, size_t _dataSize);
    char *getData() {
        return 0;
    }
    size_t getDataSize() {
        return dataSize;
    }
    void sink();

  private:
    size_t dataSize;

    static BdevCpp::GeneralPool<WriteFuture, BdevCpp::ClassAlloc<WriteFuture>> writeFuturePool;
};

} // namespace BdevCpp
