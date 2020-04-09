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

class FileEmu;

class ApiBase {
  public:
    ApiBase()
        : storageGeom(0), femu(0) {}
    virtual ~ApiBase() = default;

  protected:
    virtual int getIoPosLinear(int desc, uint64_t &lba, uint8_t &lun) = 0;
    virtual int getIoPosLinear(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) = 0;
    virtual int getIoPosStriped(int desc, uint64_t &lba, uint8_t &lun) = 0;
    virtual int getIoPosStriped(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) = 0;

  public:
    virtual int open(const char *name, int flags, mode_t mode = S_IRUSR | S_IWUSR) = 0;
    virtual int close(int desc) = 0;
    virtual int read(int desc, char *buffer, size_t bufferSize) = 0;
    virtual int write(int desc, const char *data, size_t dataSize) = 0;
    virtual off_t lseek(int fd, off_t offset, int whence) = 0;
    virtual int fsync(int desc) = 0;

    virtual FutureBase *read(int desc, uint64_t pos, char *buffer, size_t bufferSize) = 0;
    virtual FutureBase *write(int desc, uint64_t pos, const char *data, size_t dataSize) = 0;

    BdevGeom *storageGeom;
    FileEmu *femu;
};

} // namespace BdevCpp
