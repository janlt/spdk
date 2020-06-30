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

typedef std::mutex Lock;

struct FileGeom {
    uint64_t startLba = 0;
    uint64_t endLba = 0;
    uint8_t startLun = 0;
    uint8_t numLuns = 0;
    uint64_t LbaSize = 512;
    uint64_t optLbaSize = 4096;
    uint64_t blocksPerOptIo = 8;
};

struct FilePos {
    uint64_t pos = -1; // absolute pos
    uint64_t posLba = -1; // Lba for pos within lun
    uint8_t posLun = -1; // which lun pos is on
};

class FileEmu {
  public:
    FileEmu(const char *_name, int _flags, mode_t _mode, uint64_t _size = 0);
    virtual ~FileEmu();

    enum State {
        eUnknown = 0,
        eOpened = 1,
        eClosed = 2
    };

    off_t lseek(off_t off, int whence);
    off_t adjustPos(int64_t delta);
    void setGeom(uint64_t _startLba, uint64_t _endLba, uint8_t _startLun, uint8_t _numLuns);
    void resetPos() {
        pos.pos = 0;
        pos.posLba = 0;
        pos.posLun = 0;
    }

    std::string name;
    int flags;
    mode_t mode;
    int desc;
    int fd; // real one
    uint64_t size;
    FilePos pos;
    FileGeom geom;
    uint32_t fileSlot;
    FileEmu::State state;
};

class FileMap {
  private:
    FileMap(size_t size = (1 << 16));
    virtual ~FileMap();

  public:
    FileEmu *getFile(int desc);
    int putFile(FileEmu *fileEmu, uint64_t numBlk, uint32_t numLun);
    void setBottomSlot(uint32_t start);
    int closeFile(int desc);
    FileEmu *searchClosedFiles(const std::string &name);

    static FileMap &getInstance();

  private:
    std::vector<FileEmu *> files;
    std::vector<FileEmu *> closedFiles;
    static const uint32_t maxFiles = 1024;
    uint8_t fileMap[maxFiles];
    uint32_t bottomFreeSlot;
    uint32_t topFreeSlot;
    uint32_t startLun;
    std::mutex opMutex;

    static FileMap *map;
    static std::mutex instanceMutex;
};

} // namespace BdevCpp
