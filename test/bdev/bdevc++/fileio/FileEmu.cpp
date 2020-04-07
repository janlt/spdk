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

#include <string>
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

#include <string>
#include <atomic>
#include <cstdint>
#include <thread>
#include <mutex>
#include <chrono>
#include <condition_variable>

#include "FileEmu.h"

using namespace std;

namespace BdevCpp {

FileEmu::FileEmu(const char *_name, int _flags, mode_t _mode, uint64_t _size)
    : name(_name), flags(_flags), mode(_mode), desc(-1), fd(-1), size(_size), fileSlot(-1) {
    fd = ::open(_name, flags, mode);
    if (fd >= 0)
        desc = fd;
}

FileEmu::~FileEmu() {
}


int FileEmu::lseek(off_t off, int whence) {
    switch (whence) {
    case SEEK_SET: // absolute pos
        return adjustPos(static_cast<int64_t>(off) - pos.pos);
        break;
    case SEEK_CUR: // from current pos
        return adjustPos(static_cast<int64_t>(off));
        break;
    case SEEK_END: // end of file
        return adjustPos(static_cast<int64_t>(size));
        break;
    default:
        return -1;
        break;
    }
}

int FileEmu::adjustPos(int64_t delta) {
    if (delta < 0 && static_cast<uint64_t>(abs(delta)) > pos.pos)
        return -1;
    uint64_t deltaLbas;
    if (delta > 0)
        deltaLbas = delta/geom.optLbaSize + 1;
    else if (delta < 0)
        deltaLbas = delta/geom.optLbaSize - 1;
    else
        deltaLbas = 0;
    if (pos.posLba + deltaLbas > geom.endLba) {
        pos.posLun++;
        pos.posLun %= geom.numLuns;
        pos.posLba = geom.startLba;
    } else {
        pos.posLba += deltaLbas;
    }
    return 0;
}

void FileEmu::setGeom(uint64_t _startLba, uint64_t _endLba, uint8_t _startLun, uint8_t _numLuns) {
    geom.startLba = _startLba;
    geom.endLba = _endLba;
    geom.startLun = _startLun;
    geom.numLuns = _numLuns;
}


FileMap *FileMap::map = 0;
std::mutex FileMap::instanceMutex;

FileMap::FileMap(size_t size)
    : bottomFreeSlot(-1), topFreeSlot(0), startLun(0) {
    files.resize(size);
    ::memset(fileMap, '\0', sizeof(fileMap));
}

FileMap::~FileMap() {
}

FileEmu *FileMap::getFile(int desc) {
    if (desc < 0 || static_cast<size_t>(desc) >= files.size())
        return 0;
    unique_lock<Lock> w_lock(opMutex);
    return files[desc].get();
}

void FileMap::setBottomSlot(uint32_t start) {
    for (uint32_t i = start ; i < maxFiles ; i++)
        if (!fileMap[i]) {
            bottomFreeSlot = i;
            return;
        }

    bottomFreeSlot = -1;
}

int FileMap::putFile(FileEmu *fileEmu, uint64_t numBlk, uint32_t numLun) {
    if (!fileEmu || fileEmu->desc < 0 || files[fileEmu->desc])
        return -1;

    unique_lock<Lock> w_lock(opMutex);

    uint32_t freeSlot;
    if (bottomFreeSlot != static_cast<uint32_t>(-1)) {
        freeSlot = bottomFreeSlot;
        fileMap[bottomFreeSlot] = 1;
        setBottomSlot(bottomFreeSlot);
    } else {
        if (topFreeSlot >= maxFiles)
            return -1;
        freeSlot = topFreeSlot;
        fileMap[topFreeSlot] = 1;
        topFreeSlot++;
    }
    fileEmu->geom.startLba = numBlk/maxFiles * freeSlot;
    fileEmu->geom.endLba = numBlk/maxFiles * (freeSlot + 1);
    fileEmu->geom.numLuns = numLun;
    fileEmu->geom.startLun = startLun%numLun;
    startLun++;
    fileEmu->fileSlot = freeSlot;

    fileEmu->resetPos();
    fileEmu->lseek(0, SEEK_SET);

    files[fileEmu->desc].reset(fileEmu);

    return fileEmu->desc;
}

int FileMap::closeFile(int desc) {
    if (desc < 0 || static_cast<size_t>(desc) >= files.size())
        return 0;

    unique_lock<Lock> w_lock(opMutex);

    FileEmu *femu = files[desc].get();
    fileMap[femu->fileSlot] = 0;
    if (topFreeSlot - 1 == femu->fileSlot)
        topFreeSlot = femu->fileSlot;
    else
        setBottomSlot(0);

    files[desc].reset();
    return 0;
}

FileMap &FileMap::getInstance() {
    if (!map) {
        unique_lock<Lock> w_lock(instanceMutex);
        if (!map)
            map = new FileMap();
    }
    return *map;
}

} // namespace BdevCpp
