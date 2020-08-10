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
    int get(char *&data, size_t &_dataSize, unsigned long long _timeout = -1);
    void signal(StatusCode status, const char *data, size_t _dataSize);
    char *getData() {
        return buffer;
    }
    size_t getDataSize() {
        return bufferSize;
    }
    void setBuffer(char *_buffer) {
        buffer = _buffer;
    }
    void setBufferSize(size_t _bufferSize) {
        bufferSize = _bufferSize;
    }
    void setDataSize(size_t _dataSize) {}
    void sink();
    void reset() {
        ready = false;
    }

  private:
    char *buffer;
    size_t bufferSize;
    bool ready;
    std::mutex mtx;
    std::condition_variable cv;

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
    int get(char *&data, size_t &_dataSize, unsigned long long _timeout = -1);
    void signal(StatusCode status, const char *data, size_t _dataSize);
    char *getData() {
        return 0;
    }
    size_t getDataSize() {
        return dataSize;
    }
    void setBuffer(char *_buffer) {}
    void setBufferSize(size_t _bufferSize) {}
    void setDataSize(size_t _dataSize) {
        dataSize = _dataSize;
    }
    void sink();
    void reset() {
        ready = false;
    }

  private:
    size_t dataSize;
    bool ready;
    std::mutex mtx;
    std::condition_variable cv;

    static BdevCpp::GeneralPool<WriteFuture, BdevCpp::ClassAlloc<WriteFuture>> writeFuturePool;
};



class ReadFuturePolling: public FutureBase {
friend class Api;
friend class AsyncApi;
friend class ClassAlloc<ReadFuturePolling>;

  private:
    ReadFuturePolling(char *_buffer = 0, size_t _bufferSize = 0);
    virtual ~ReadFuturePolling();

  public:
    int get(char *&data, size_t &_dataSize, unsigned long long _timeout = -1);
    void signal(StatusCode status, const char *data, size_t _dataSize);
    char *getData() {
        return buffer;
    }
    size_t getDataSize() {
        return bufferSize;
    }
    void setBuffer(char *_buffer) {
       buffer = _buffer;
    }
    void setBufferSize(size_t _bufferSize) {
        bufferSize = _bufferSize;
    }
    void setDataSize(size_t _dataSize) {}
    void sink();
    void reset() {
        state = 0;
    }

  private:
    char *buffer;
    size_t bufferSize;
    std::atomic<int> state;

    static BdevCpp::GeneralPool<ReadFuturePolling, BdevCpp::ClassAlloc<ReadFuturePolling>> readFuturePollingPool;
};

class WriteFuturePolling: public FutureBase {
friend class Api;
friend class AsyncApi;
friend class ClassAlloc<WriteFuturePolling>;

  private:
    WriteFuturePolling(size_t _dataSize = 0);
    virtual ~WriteFuturePolling();

  public:
    int get(char *&data, size_t &_dataSize, unsigned long long _timeout = -1);
    void signal(StatusCode status, const char *data, size_t _dataSize);
    char *getData() {
        return 0;
    }
    size_t getDataSize() {
        return dataSize;
    }
    void setBuffer(char *_buffer) {}
    void setBufferSize(size_t _bufferSize) {}
    void setDataSize(size_t _dataSize) {
        dataSize = _dataSize;
    }
    void sink();
    void reset() {
        state = 0;
    }

  private:
    size_t dataSize;
    std::atomic<int> state;

    static BdevCpp::GeneralPool<WriteFuturePolling, BdevCpp::ClassAlloc<WriteFuturePolling>> writeFuturePollingPool;
};

} // namespace BdevCpp
