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

class AsyncApi : public ApiBase {
    friend class Api;
    AsyncApi(IoPoller *_spio);
    virtual ~AsyncApi();

    IoPoller *spio;

  public:
    int open(const char *name, int flags, mode_t mode);
    int close(int desc);
    int read(int desc, char *buffer, size_t bufferSize);
    int write(int desc, const char *data, size_t dataSize);
};

} // namespace BdevCpp
