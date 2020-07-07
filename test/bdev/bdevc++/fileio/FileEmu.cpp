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
#include <fstream>

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
    : name(_name), flags(_flags), mode(_mode), desc(-1),
      fd(-1), size(_size), fileSlot(-1), state(FileEmu::eUnknown) {
    fd = ::open(_name, flags, mode);
    if (fd >= 0)
        desc = fd;
}

FileEmu::~FileEmu() {}


int FileEmu::stat(struct stat *buf) {
    buf->st_blksize = geom.LbaSize;
    buf->st_blocks = size/geom.LbaSize;
    buf->st_size = size;
    return 0;
}

off_t FileEmu::lseek(off_t off, int whence) {
    switch (whence) {
    case SEEK_SET: // absolute pos
        return adjustPos(static_cast<int64_t>(off) - pos.pos);
        break;
    case SEEK_CUR: // from current pos
        return adjustPos(static_cast<int64_t>(off));
        break;
    case SEEK_END: // end of file
        return adjustPos(static_cast<int64_t>(size - pos.pos));
        break;
    default:
        return -1;
        break;
    }
}

off_t FileEmu::adjustPos(int64_t delta) {
    if (delta < 0 && static_cast<uint64_t>(abs(delta)) > pos.pos)
        return -1;
    //cout << "adjustPos ENTER p.pos " << pos.pos << " delta " << delta <<
            //" p.posLba " << pos.posLba << " p.posLun " << static_cast<uint32_t>(pos.posLun) << endl;
    pos.pos += delta;
    uint64_t apos = pos.pos >> 9;  // pos in 512 blocks
    uint64_t pos4k = apos >> 3;
    pos.posLun = pos4k%geom.numLuns;
    pos.posLba = apos + geom.startLba;

    if (pos.pos > size)
        size = pos.pos;
    //cout << "adjustPos EXIT p.pos " << pos.pos << " delta " << delta <<
            //" p.posLba " << pos.posLba << " p.posLun " << static_cast<uint32_t>(pos.posLun) << endl;
    return pos.pos;
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

    savedFiles.open("/tmp/emu.saved.files", fstream::in | fstream::out | fstream::app);
    initFromSavedFiles();
}

FileMap::~FileMap() {
    savedFiles.close();
}

int FileMap::unlinkFile(const char *path) {
    if (!path)
        return -1;

    unique_lock<Lock> w_lock(opMutex);

    FileEmu *feNieladnie = 0;
    for (size_t i = 0 ; i < files.size() ; i++) {
        if (files[i] && (files[i]->name == basename(path) || files[i]->name == path)) {
            feNieladnie = files[i];
            files[i] = 0;
            break;
        }
    }

    if (!feNieladnie) {
        for (size_t i = 0 ; i < closedFiles.size() ; i++)
            if (closedFiles[i] && (closedFiles[i]->name == basename(path) || closedFiles[i]->name == path)) {
                feNieladnie = closedFiles[i];
                closedFiles.erase(closedFiles.begin() + i);
                break;
            }
    }

    if (!feNieladnie)
        return -1;

    addSavedFiles(feNieladnie, 0);
    delete feNieladnie;

    return 0;
}

void FileMap::initFromSavedFiles() {
    unique_lock<Lock> w_lock(opMutex);

    savedFiles.seekp(0);

    while (true) {
        if (savedFiles.eof() == true)
            break;

        int exists = 1;
        FileEmu *emu = new FileEmu("asd090a9si0i09i0adddds_+09asd", 0, 0);
        savedFiles >> emu->name >>
            emu->fileSlot >>
            emu->size >>
            emu->geom.startLba >>
            emu->geom.endLba >>
            emu->geom.startLun >>
            emu->geom.numLuns >>
            exists;

        if (savedFiles.fail() == true)
            break;

        if (exists) {
            emu->state = FileEmu::eExistent;
            updateClosedFiles(emu);
        } else {
            removeFromClosedFiles(emu);
            delete emu;
        }
    }

    savedFiles.clear();
    savedFiles.seekp(ios_base::end);
    savedFiles.clear();
}

void FileMap::removeFromClosedFiles(const FileEmu *fileEmu) {
    for (size_t i = 0 ; i < closedFiles.size() ; i++) {
        if (closedFiles[i]->name == fileEmu->name && closedFiles[i]->fileSlot == fileEmu->fileSlot) {
            closedFiles.erase(closedFiles.begin() + i);
            return;
        }
    }
}

void FileMap::updateClosedFiles(FileEmu *fileEmu) {
    for (auto &p : closedFiles) {
        if (p->name == fileEmu->name && p->fileSlot == fileEmu->fileSlot) {
            p->size = fileEmu->size;
            p->geom.startLba = fileEmu->geom.startLba;
            p->geom.endLba = fileEmu->geom.endLba;
            p->geom.startLun = fileEmu->geom.startLun;
            p->geom.numLuns = fileEmu->geom.numLuns;

            delete fileEmu;
            return;
        }
    }

    closedFiles.push_back(fileEmu);
}

FileEmu *FileMap::getFile(int desc) {
    if (desc < 0 || static_cast<size_t>(desc) >= files.size())
        return 0;
    unique_lock<Lock> w_lock(opMutex);
    return files[desc];
}

FileEmu *FileMap::getFile(const char *path) {
    if (!path)
        return 0;
    unique_lock<Lock> w_lock(opMutex);

    for (auto &fe : files) {
        if (fe && (fe->name == basename(path) || fe->name == path))
            return fe;
    }
    for (auto &fe : closedFiles)
        if (fe && (fe->name == basename(path) || fe->name == path))
            return fe;

    return 0;
}

void FileMap::setBottomSlot(uint32_t start) {
    for (uint32_t i = start ; i < maxFiles ; i++)
        if (!fileMap[i]) {
            bottomFreeSlot = i;
            return;
        }

    bottomFreeSlot = -1;
}

uint32_t FileMap::getFreeSlot() {
    uint32_t freeSlot = -1;

    if (bottomFreeSlot != static_cast<uint32_t>(-1)) {
        freeSlot = bottomFreeSlot;
        fileMap[bottomFreeSlot] = 1;
        setBottomSlot(bottomFreeSlot);
    } else {
        if (topFreeSlot >= maxFiles)
            return freeSlot;
        freeSlot = topFreeSlot;
        fileMap[topFreeSlot] = 1;
        topFreeSlot++;
    }

    return freeSlot;
}

int FileMap::putFile(FileEmu *fileEmu, uint64_t numBlk, uint32_t numLun) {
    if (!fileEmu || fileEmu->desc < 0 || files[fileEmu->desc])
        return -1;

    unique_lock<Lock> w_lock(opMutex);

    FileEmu *closedFile = searchClosedFiles(fileEmu->name);
    uint32_t freeSlot;
    if (!closedFile) {
        freeSlot = getFreeSlot();
        if (freeSlot == static_cast<uint32_t>(-1))
            return -1;

        fileEmu->geom.startLba = numBlk/maxFiles * freeSlot;
        fileEmu->geom.endLba = numBlk/maxFiles * (freeSlot + 1);
        fileEmu->geom.numLuns = numLun;
        fileEmu->geom.startLun = startLun%numLun;
    } else {
        fileEmu->size = closedFile->size;
        fileEmu->geom = closedFile->geom;
        switch (closedFile->size) {
        case FileEmu::eClosed:
            freeSlot = closedFile->fileSlot;
            break;
        case FileEmu::eExistent:
            freeSlot = getFreeSlot();
            if (freeSlot == static_cast<uint32_t>(-1))
                return -1;
            closedFile->fileSlot = freeSlot;
            break;
        default:
            cerr << "File wrong state name: " << closedFile->name << endl;
            return -1;
            break;
        }

    }

    startLun++;
    fileEmu->fileSlot = freeSlot;
    fileEmu->state = FileEmu::eOpened;

    fileEmu->resetPos();
    fileEmu->lseek(0, SEEK_SET);

    if (files[fileEmu->desc])
        return -1;
    files[fileEmu->desc] = fileEmu;

    return fileEmu->desc;
}

int FileMap::closeFile(int desc) {
    if (desc < 0 || static_cast<size_t>(desc) >= files.size())
        return 0;

    unique_lock<Lock> w_lock(opMutex);

    FileEmu *femu = files[desc];
    if (!femu)
        return -1;

    femu->state = FileEmu::eClosed;
    FileEmu *closedFile = searchClosedFiles(femu->name);
    if (!closedFile)
        closedFiles.push_back(femu);
    else
        closedFile->size = femu->size;

    addSavedFiles(femu, 1);

    files[desc] = 0;
    return 0;
}

FileEmu *FileMap::searchClosedFiles(const std::string &name) {
    for (auto &p : closedFiles) {
        if (p->name == name)
            return p;
    }
    return 0;
}

void FileMap::addSavedFiles(const FileEmu *fileEmu, int exists) {
    savedFiles << fileEmu->name << " " <<
            fileEmu->fileSlot << " " <<
            fileEmu->size << " " <<
            fileEmu->geom.startLba << " " <<
            fileEmu->geom.endLba << " " <<
            static_cast<int>(fileEmu->geom.startLun) << " " <<
            static_cast<int>(fileEmu->geom.numLuns) << " " <<
            exists << endl << flush;
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
