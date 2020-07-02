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
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <string>
#include <mutex>

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

#include "FutureBase.h"
#include "ApiBase.h"
#include "FileEmu.h"

namespace BdevCpp {

int ApiBase::open(const char *name, int flags, mode_t mode) {
    FileEmu *femu = new FileEmu(name, flags, mode);
    if (femu->desc < 0)
        return -1;
    if (FileMap::getInstance().putFile(femu, storageGeom->blk_num[0], storageGeom->dev_num) < 0) {
        delete femu;
        return -1;
    }
    return femu->desc;
}

int ApiBase::close(int desc) {
    FileEmu *femu = FileMap::getInstance().getFile(desc);
    if (!femu)
        return -1;
    int fd = femu->fd;
    FileMap::getInstance().closeFile(desc);
    return ::close(fd);
}

off_t ApiBase::lseek(int desc, off_t offset, int whence) {
    FileEmu *femu = FileMap::getInstance().getFile(desc);
    if (!femu)
        return -1;
    return femu->lseek(offset, whence);
}

int ApiBase::getIoPosLinear(int desc, uint64_t &lba, uint8_t &lun) {
    FileEmu *femu = FileMap::getInstance().getFile(desc);
    if (!femu)
        return -1;
    lba = femu->pos.posLba + femu->geom.startLba;
    lun = femu->pos.posLun;
    return 0;
}

int ApiBase::getIoPosLinear(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) {
    FileEmu *femu = FileMap::getInstance().getFile(desc);
    if (!femu)
        return -1;
    femu->lseek(static_cast<off_t>(pos), SEEK_SET);
    lba = femu->pos.posLba + femu->geom.startLba;
    lun = femu->pos.posLun;
    return 0;
}

int ApiBase::getIoPosStriped(int desc, uint64_t pos, uint64_t &lba, uint8_t &lun) {
    FileEmu *femu = FileMap::getInstance().getFile(desc);
    if (!femu)
        return -1;
    FilePos apos = femu->pos;
    apos.pos = (pos >> 9);  // pos in 512 blocks
    uint64_t pos4k = apos.pos >> 3; // pos in 4k
    lun = pos4k%femu->geom.numLuns;
    lba = apos.pos + femu->geom.startLba;
    return 0;
}

int ApiBase::getIoPosStriped(int desc, uint64_t &lba, uint8_t &lun) {
    return 0;
}

int ApiBase::stat(const char *path, struct stat *buf) {
    FileEmu *femu = FileMap::getInstance().getFile(path);
    if (!femu)
        return -1;
    return femu->stat(buf);
}

} // namespace BdevCpp
