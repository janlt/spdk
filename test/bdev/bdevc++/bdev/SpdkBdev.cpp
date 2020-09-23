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

#include <iostream>
#include <sstream>
#include <string>

#include <boost/asio.hpp>
#include <boost/filesystem.hpp>

#include "spdk/stdinc.h"
#include "spdk/cpuset.h"
#include "spdk/queue.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include "spdk/event.h"
#include "spdk/ftl.h"
#include "spdk/conf.h"
#include "spdk/env.h"

#include "Status.h"
#include "Options.h"
#include "Logger.h"
#include "BlockingPoller.h"
#include "Poller.h"
#include "Io2Poller.h"
#include "SpdkIoEngine.h"
#include "Rqst.h"
#include "SpdkDevice.h"
#include "SpdkConf.h"
#include "SpdkBdev.h"
#include "SpdkIoBuf.h"

namespace BdevCpp {

//#define AVG_IO_LAT

#ifdef AVG_IO_LAT
static int dt_read_count = 1;
static int dt_write_count = 1;

static timespec io_diff_read_t;
static timespec io_diff_write_t;

static timespec io_avg_read_t;
static timespec io_avg_write_t;

timespec start_t, end_t, diff_t;


#define IO_LAT_AVG_INCR 256
#define IO_LAT_AVG_INTERVAL (1 << 16)

#define JANS_CLOCK_MODE CLOCK_MONOTONIC

static timespec io_time_diff(timespec start, timespec end) {
    timespec temp;
    if ((end.tv_nsec-start.tv_nsec) < 0) {
        temp.tv_sec = end.tv_sec-start.tv_sec - 1;
        temp.tv_nsec = 1000000000 + end.tv_nsec - start.tv_nsec;
    } else {
        temp.tv_sec = end.tv_sec - start.tv_sec;
        temp.tv_nsec = end.tv_nsec - start.tv_nsec;
    }
    return temp;
}

inline void io_update_read_avg(int dt_read_count) {
    if (!(dt_read_count%IO_LAT_AVG_INCR)) {
        io_avg_read_t.tv_nsec += (io_diff_read_t.tv_nsec/IO_LAT_AVG_INCR);
        io_avg_read_t.tv_sec += (io_diff_read_t.tv_sec/IO_LAT_AVG_INCR);
        io_diff_read_t.tv_sec = 0;
        io_diff_read_t.tv_nsec = 0;
    }
}

inline void io_update_write_avg(int dt_write_count) {
    if (!(dt_write_count%IO_LAT_AVG_INCR)) {
        io_avg_write_t.tv_nsec += (io_diff_write_t.tv_nsec/IO_LAT_AVG_INCR);
        io_avg_write_t.tv_sec += (io_diff_write_t.tv_sec/IO_LAT_AVG_INCR);
        io_diff_write_t.tv_sec = 0;
        io_diff_write_t.tv_nsec = 0;
    }
}

inline void io_get_read_avg(int &dt_read_count) {
    if (!((dt_read_count++)%IO_LAT_AVG_INTERVAL)) {
        timespec tmp_t;
        int tmp_count = dt_read_count/IO_LAT_AVG_INCR;
        tmp_t.tv_sec = io_avg_read_t.tv_sec/tmp_count;
        tmp_t.tv_nsec = io_avg_read_t.tv_nsec/tmp_count;

        cout << "spdk_io_read_lat.sec: " << tmp_t.tv_sec << " spdk_io_read_lat.nsec: " << tmp_t.tv_nsec << endl;
        dt_read_count = 1;
        io_diff_read_t.tv_sec = 0;
        io_diff_read_t.tv_nsec = 0;
        io_avg_read_t.tv_sec = 0;
        io_avg_read_t.tv_nsec = 0;
    }
}

inline void io_get_write_avg(int &dt_write_count) {
    if (!((dt_write_count++)%IO_LAT_AVG_INTERVAL)) {
        timespec tmp_t;
        int tmp_count = dt_write_count/IO_LAT_AVG_INCR;
        tmp_t.tv_sec = io_avg_write_t.tv_sec/tmp_count;
        tmp_t.tv_nsec = io_avg_write_t.tv_nsec/tmp_count;

        cout << "spdk_io_write_lat.sec: " << tmp_t.tv_sec << " spdk_io_write_lat.nsec: " << tmp_t.tv_nsec << endl;
        dt_write_count = 1;
        io_diff_write_t.tv_sec = 0;
        io_diff_write_t.tv_nsec = 0;
        io_avg_write_t.tv_sec = 0;
        io_avg_write_t.tv_nsec = 0;
    }

}
#endif

//#define TEST_RAW_IOPS

const char *poolLayout = "queue";
#define CREATE_MODE_RW (S_IWUSR | S_IRUSR)

/*
 * SpdkBdev
 */
const std::string DEFAULT_SPDK_CONF_FILE = "spdk.conf";

SpdkDeviceClass SpdkBdev::bdev_class = SpdkDeviceClass::BDEV;
size_t SpdkBdev::cpuCoreCounter = SpdkBdev::cpuCoreStart;

int SpdkBdev::ioBufMgrInst = 0;

SpdkBdev::SpdkBdev(bool enableStats)
    : state(SpdkBdevState::SPDK_BDEV_INIT), _spdkPoller(0), confBdevNum(-1),
      cpuCore(SpdkBdev::getCoreNum()), cpuCoreFin(cpuCore + 1),
      cpuCoreIoEng(cpuCoreFin + 1), ioEngine(0), ioEngineThread(0),
      io2(0), io2Thread(0), ioEngineInitDone(0), maxIoBufs(0), maxCacheIoBufs(0), ioBufsInUse(0),
      ioPoolMgr(new SpdkIoBufMgr(ioBufMgrInst++)), isRunning(0), statsEnabled(enableStats) {}

SpdkBdev::~SpdkBdev() {
    if (io2Thread != nullptr)
        io2Thread->join();
    if (io2)
        delete io2;

    if (ioEngineThread != nullptr)
        ioEngineThread->join();
    if (ioEngine)
        delete ioEngine;

    delete ioPoolMgr;
}

size_t SpdkBdev::getCoreNum() {
    SpdkBdev::cpuCoreCounter += 3;
    return SpdkBdev::cpuCoreCounter;
}

void SpdkBdev::IOQuiesce() {
    if (io2)
        io2->Quiesce();
    _IoState = SpdkBdev::IOState::BDEV_IO_QUIESCING;
}

bool SpdkBdev::isIOQuiescent() {
    if (io2)
        return ((_IoState == SpdkBdev::IOState::BDEV_IO_QUIESCENT ||
             _IoState == SpdkBdev::IOState::BDEV_IO_ABORTED) &&
             io2->isQuiescent() == true)
               ? true
               : false;
    else
        return (_IoState == SpdkBdev::IOState::BDEV_IO_QUIESCENT ||
              _IoState == SpdkBdev::IOState::BDEV_IO_ABORTED)
               ? true
               : false;
}

void SpdkBdev::IOAbort() { _IoState = SpdkBdev::IOState::BDEV_IO_ABORTED; }

/*
 * Callback function for a write IO completion.
 */
void SpdkBdev::writeComplete(struct spdk_bdev_io *bdev_io, bool success,
                             void *cb_arg) {
#ifdef AVG_IO_LAT
    clock_gettime(JANS_CLOCK_MODE, &end_t);
    diff_t = io_time_diff(start_t, end_t);

    io_diff_write_t.tv_sec += diff_t.tv_sec;
    io_diff_write_t.tv_nsec += diff_t.tv_nsec;

    io_update_write_avg(dt_write_count);
    io_get_write_avg(dt_write_count);
#endif

    BdevTask *task = reinterpret_cast<DeviceTask *>(cb_arg);
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);
    bdev->ioPoolMgr->putIoWriteBuf(task->buff);
    bdev->ioBufsInUse--;

#ifndef TEST_RAW_IOPS
    spdk_bdev_free_io(bdev_io);
#endif

    if (bdev->stats.outstanding_io_cnt)
        bdev->stats.outstanding_io_cnt--;

    bdev->stats.write_compl_cnt++;
    (void)bdev->stateMachine();

    task->result = success;
    if (bdev->statsEnabled == true && success == true)
        bdev->stats.printWritePer(std::cout, bdev->spBdevCtx.bdev_addr);

    if (task->result == true) {
        if (task->clb)
            task->clb(StatusCode::OK, nullptr, task->size);
    } else {
        if (task->clb)
            task->clb(StatusCode::UNKNOWN_ERROR, nullptr, 0);
    }

    if (task->rqst->inPlace == false)
        IoRqst::writePool.put(task->rqst);

#ifdef _IO_FINALIZER_
    bdev->io2->enqueue(task);
#endif
}

/*
 * Callback function for a read IO completion.
 */
void SpdkBdev::readComplete(struct spdk_bdev_io *bdev_io, bool success,
                            void *cb_arg) {
#ifdef AVG_IO_LAT
    clock_gettime(JANS_CLOCK_MODE, &end_t);
    diff_t = io_time_diff(start_t, end_t);

    io_diff_read_t.tv_sec += diff_t.tv_sec;
    io_diff_read_t.tv_nsec += diff_t.tv_nsec;

    io_update_read_avg(dt_read_count);
    io_get_read_avg(dt_read_count);
#endif

    BdevTask *task = reinterpret_cast<DeviceTask *>(cb_arg);
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);

#ifndef TEST_RAW_IOPS
    spdk_bdev_free_io(bdev_io);
#endif

    if (bdev->stats.outstanding_io_cnt)
        bdev->stats.outstanding_io_cnt--;

    bdev->stats.read_compl_cnt++;
    (void)bdev->stateMachine();

    task->result = success;
    if (bdev->statsEnabled == true && success == true)
        bdev->stats.printReadPer(std::cout, bdev->spBdevCtx.bdev_addr);

    if (task->result == true) {
        if (task->clb)
            task->clb(StatusCode::OK,
                      task->buff->getSpdkDmaBuf(), task->size);
    } else {
        if (task->clb)
            task->clb(StatusCode::UNKNOWN_ERROR, nullptr, 0);
    }

    bdev->ioPoolMgr->putIoReadBuf(task->buff);
    bdev->ioBufsInUse--;
    if (task->rqst->inPlace == false)
        IoRqst::readPool.put(task->rqst);

#ifdef _IO_FINALIZER_
    bdev->io2->enqueue(task);
#endif
}

/*
 * Callback function that SPDK framework will call when an io buffer becomes
 * available
 */
void SpdkBdev::readQueueIoWait(void *cb_arg) {
    BdevTask *task = reinterpret_cast<DeviceTask *>(cb_arg);
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);

    uint64_t numBlks = !(task->size%task->blockSize) ? task->size/task->blockSize : task->size/task->blockSize + 1;
    int r_rc = spdk_bdev_read_blocks(
        bdev->spBdevCtx.bdev_desc, bdev->spBdevCtx.io_channel,
        task->buff->getSpdkDmaBuf(), task->lba,
        numBlks, SpdkBdev::readComplete, task);

    /* If a read IO still fails due to shortage of io buffers, queue it up for
     * later execution */
    if (r_rc) {
        r_rc = spdk_bdev_queue_io_wait(bdev->spBdevCtx.bdev,
                                       bdev->spBdevCtx.io_channel,
                                       &task->bdev_io_wait);
        if (r_rc) {
            IOP_CRITICAL("Spdk queue_io_wait error [" + std::to_string(r_rc) +
                         "] for spdk bdev");
            bdev->deinit();
            spdk_app_stop(-1);
        }
    } else {
        bdev->stats.read_err_cnt--;
    }
}

/*
 * Callback function that SPDK framework will call when an io buffer becomes
 * available
 */
void SpdkBdev::writeQueueIoWait(void *cb_arg) {
    BdevTask *task = reinterpret_cast<DeviceTask *>(cb_arg);
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);

    uint64_t numBlks = !(task->size%task->blockSize) ? task->size/task->blockSize : task->size/task->blockSize + 1;
    int w_rc = spdk_bdev_write_blocks(
        bdev->spBdevCtx.bdev_desc, bdev->spBdevCtx.io_channel,
        task->buff->getSpdkDmaBuf(), task->lba,
        numBlks, SpdkBdev::writeComplete, task);

    /* If a write IO still fails due to shortage of io buffers, queue it up for
     * later execution */
    if (w_rc) {
        w_rc = spdk_bdev_queue_io_wait(bdev->spBdevCtx.bdev,
                                       bdev->spBdevCtx.io_channel,
                                       &task->bdev_io_wait);
        if (w_rc) {
            IOP_CRITICAL("Spdk queue_io_wait error [" + std::to_string(w_rc) +
                         "] for spdk bdev");
            bdev->deinit();
            spdk_app_stop(-1);
        }
    } else {
        bdev->stats.write_err_cnt--;
    }
}

bool SpdkBdev::read(DeviceTask *task) {
    if (ioEngine && task->routing == true)
        return ioEngine->enqueue(task);
    return doRead(task);
}

bool SpdkBdev::doRead(DeviceTask *task) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);
    if (stateMachine() == true) {
        if (task->buff)
            ioPoolMgr->putIoReadBuf(task->buff);
        bdev->ioBufsInUse--;
        return false;
    }

    uint64_t numBlks = !(task->size%task->blockSize) ? task->size/task->blockSize : task->size/task->blockSize + 1;
    bdev->ioBufsInUse++;
    task->buff = ioPoolMgr->getIoReadBuf(numBlks*task->blockSize, bdev->spBdevCtx.buf_align);

#ifdef TEST_RAW_IOPS
    int r_rc = 0;
    SpdkBdev::readComplete(reinterpret_cast<struct spdk_bdev_io *>(task->buff),
                           true, task);
#else
    //std::cout << "Read dataSize " << task->size <<
        //" blockSize " << task->blockSize << " lba " << task->lba << " nblks " << numBlks <<
        // " nvme: " << bdev->spBdevCtx.bdev_addr << std::endl;
#ifdef AVG_IO_LAT
    clock_gettime(JANS_CLOCK_MODE, &start_t);
#endif

    int r_rc = spdk_bdev_read_blocks(
        bdev->spBdevCtx.bdev_desc, bdev->spBdevCtx.io_channel,
        task->buff->getSpdkDmaBuf(), task->lba,
        numBlks, SpdkBdev::readComplete, task);
#endif
    bdev->stats.outstanding_io_cnt++;

    if (r_rc) {
        stats.read_err_cnt++;
        /*
         * -ENOMEM condition indicates a shortage of IO buffers.
         *  Schedule this IO for later execution by providing a callback
         * function, when sufficient IO buffers are available
         */
        if (r_rc == -ENOMEM) {
            r_rc = SpdkBdev::reschedule(task);
            if (r_rc) {
                IOP_CRITICAL("Spdk queue_io_wait error [" +
                             std::to_string(r_rc) + "] for spdk bdev");
                if (task->buff)
                    ioPoolMgr->putIoReadBuf(task->buff);
                bdev->deinit();
                spdk_app_stop(-1);
            }
        } else {
            IOP_CRITICAL("Spdk read error [" + std::to_string(r_rc) +
                         "] for spdk bdev");
            if (task->buff)
                ioPoolMgr->putIoReadBuf(task->buff);
            bdev->deinit();
            spdk_app_stop(-1);
        }
    }

    return !r_rc ? true : false;
}

bool SpdkBdev::write(DeviceTask *task) {
    if (ioEngine && task->routing == true)
        return ioEngine->enqueue(task);
    return doWrite(task);
}

bool SpdkBdev::doWrite(DeviceTask *task) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);
    if (stateMachine() == true) {
        if (task->buff)
            ioPoolMgr->putIoWriteBuf(task->buff);
        bdev->ioBufsInUse--;
        return false;
    }

    uint64_t numBlks = !(task->size%task->blockSize) ? task->size/task->blockSize : task->size/task->blockSize + 1;
    auto dataSizeAlign = bdev->getAlignedSize(numBlks*task->blockSize);
    bdev->ioBufsInUse++;
    task->buff = ioPoolMgr->getIoWriteBuf(dataSizeAlign, bdev->spBdevCtx.buf_align);

    memcpy(task->buff->getSpdkDmaBuf(), task->rqst->data, task->size);

#ifdef TEST_RAW_IOPS
    int w_rc = 0;
    SpdkBdev::writeComplete(reinterpret_cast<struct spdk_bdev_io *>(task->buff),
                            true, task);
#else
    //std::cout << "Write dataSize " << task->size <<
        //" blockSize " << task->blockSize << " lba " << task->lba << " nblks " << numBlks <<
        // " nvme: " << bdev->spBdevCtx.bdev_addr << std::endl;
#ifdef AVG_IO_LAT
    clock_gettime(JANS_CLOCK_MODE, &start_t);
#endif

    int w_rc = spdk_bdev_write_blocks(
        bdev->spBdevCtx.bdev_desc, bdev->spBdevCtx.io_channel,
        task->buff->getSpdkDmaBuf(), task->lba,
        numBlks, SpdkBdev::writeComplete, task);
#endif
    bdev->stats.outstanding_io_cnt++;

    if (w_rc) {
        stats.write_err_cnt++;
        /*
         * -ENOMEM condition indicates a shortage of IO buffers.
         *  Schedule this IO for later execution by providing a callback
         * function, when sufficient IO buffers are available
         */
        if (w_rc == -ENOMEM) {
            w_rc = SpdkBdev::reschedule(task);
            if (w_rc) {
                IOP_CRITICAL("Spdk queue_io_wait error [" +
                             std::to_string(w_rc) + "] for spdk bdev");
                if (task->buff)
                    ioPoolMgr->putIoWriteBuf(task->buff);
                bdev->deinit();
                spdk_app_stop(-1);
            }
        } else {
            IOP_CRITICAL("Spdk write error [" + std::to_string(w_rc) +
                         "] for spdk bdev");
            if (task->buff)
                ioPoolMgr->putIoWriteBuf(task->buff);
            bdev->deinit();
            spdk_app_stop(-1);
        }
    }

    return !w_rc ? true : false;
}

int SpdkBdev::reschedule(DeviceTask *task) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(task->bdev);

    task->bdev_io_wait.bdev = spBdevCtx.bdev;
    task->bdev_io_wait.cb_fn = SpdkBdev::writeQueueIoWait;
    task->bdev_io_wait.cb_arg = task;

    return spdk_bdev_queue_io_wait(
        bdev->spBdevCtx.bdev, bdev->spBdevCtx.io_channel, &task->bdev_io_wait);
}

void SpdkBdev::deinit() {
    isRunning = 4;
    while (isRunning == 4) {
    }
    spdk_put_io_channel(spBdevCtx.io_channel);
    spdk_bdev_close(spBdevCtx.bdev_desc);
}

struct spdk_bdev *SpdkBdev::prevBdev = 0;

bool SpdkBdev::bdevInit() {
    if (confBdevNum == -1) { // single Bdev
        spBdevCtx.bdev = spdk_bdev_first();
    } else { // JBOD or RAID0
        if (!confBdevNum) {
            spBdevCtx.bdev = spdk_bdev_first_leaf();
            SpdkBdev::prevBdev = spBdevCtx.bdev;
        } else if (confBdevNum > 0) {
            spBdevCtx.bdev = spdk_bdev_next_leaf(SpdkBdev::prevBdev);
            SpdkBdev::prevBdev = spBdevCtx.bdev;
        } else {
            IOP_CRITICAL("Get leaf BDEV failed");
            spBdevCtx.state = SPDK_BDEV_NOT_FOUND;
            return false;
        }
    }

    if (!spBdevCtx.bdev) {
        IOP_CRITICAL(std::string("No NVMe devices detected for name[") +
                     spBdevCtx.bdev_name + "]");
        spBdevCtx.state = SPDK_BDEV_NOT_FOUND;
        return false;
    }
    IOP_DEBUG("NVMe devices detected for name[" + spBdevCtx->bdev_name + "]");

    spdk_bdev_opts bdev_opts;
    int rc = spdk_bdev_open(spBdevCtx.bdev, true, 0, 0, &spBdevCtx.bdev_desc);
    if (rc) {
        IOP_CRITICAL("Open BDEV failed with error code[" + std::to_string(rc) + "]");
        spBdevCtx.state = SPDK_BDEV_ERROR;
        return false;
    }

    spdk_bdev_get_opts(&bdev_opts);
    IOP_DEBUG("bdev.bdev_io_pool_size[" + bdev_opts.bdev_io_pool_size + "]" +
              " bdev.bdev.io_cache_size[" + bdev_opts.bdev_io_cache_size + "]");

    spBdevCtx.io_pool_size = bdev_opts.bdev_io_pool_size;
    maxIoBufs = spBdevCtx.io_pool_size;

    spBdevCtx.io_cache_size = bdev_opts.bdev_io_cache_size;
    maxCacheIoBufs = spBdevCtx.io_cache_size;

    spBdevCtx.io_channel = spdk_bdev_get_io_channel(spBdevCtx.bdev_desc);
    if (!spBdevCtx.io_channel) {
        IOP_CRITICAL(std::string("Get io_channel failed bdev[") +
                     spBdevCtx.bdev_name + "]");
        spBdevCtx.state = SPDK_BDEV_ERROR;
        return false;
    }

    spBdevCtx.blk_size = spdk_bdev_get_block_size(spBdevCtx.bdev);
    IOP_DEBUG("BDEV block size[" + std::to_string(spBdevCtx.blk_size) + "]");

    spBdevCtx.data_blk_size = spdk_bdev_get_data_block_size(spBdevCtx.bdev);
    IOP_DEBUG("BDEV data block size[" + spBdevCtx.data_blk_size + "]");

    spBdevCtx.buf_align = spdk_bdev_get_buf_align(spBdevCtx.bdev);
    IOP_DEBUG("BDEV align[" + std::to_string(spBdevCtx.buf_align) + "]");

    spBdevCtx.blk_num = spdk_bdev_get_num_blocks(spBdevCtx.bdev);
    IOP_DEBUG("BDEV number of blocks[" + std::to_string(spBdevCtx.blk_num) +
              "]");

    ioEngineInitDone = 1;
    return true;
}

bool SpdkBdev::init(const SpdkConf &conf) {
    confBdevNum = conf.getBdevNum();
    strcpy(spBdevCtx.bdev_name, conf.getBdevNvmeName().c_str());
    strcpy(spBdevCtx.bdev_addr, conf.getBdevNvmeAddr().c_str());
    spBdevCtx.bdev = 0;
    spBdevCtx.bdev_desc = 0;
    spBdevCtx.pci_addr = conf.getBdevSpdkPciAddr();

    setRunning(3);

    /*
     * Set up finalizer
     */
#ifdef _IO_FINALIZER_
    io2 = new Io2Poller();
    io2Thread = new std::thread(&SpdkBdev::finilizerThreadMain, this);
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpuCoreFin, &cpuset);

    int set_result = pthread_setaffinity_np(io2Thread->native_handle(),
                                            sizeof(cpu_set_t), &cpuset);
    if (!set_result) {
        IOP_DEBUG("SpdkCore thread affinity set on CPU core [" +
                  std::to_string(cpuCoreFin) + "]");
    } else {
        IOP_DEBUG("Cannot set affinity on CPU core [" +
                  std::to_string(cpuCoreFin) + "] for Io2r");
    }
#endif
    /*
     * Set up ioEngine
     */
    ioEngine = new SpdkIoEngine();
    ioEngineThread = new std::thread(&SpdkBdev::ioEngineThreadMain, this);
    cpu_set_t cpuset1;
    CPU_ZERO(&cpuset1);
    CPU_SET(cpuCoreIoEng, &cpuset1);

    int set_result = pthread_setaffinity_np(ioEngineThread->native_handle(),
                                        sizeof(cpu_set_t), &cpuset1);
    if (!set_result) {
        IOP_DEBUG("SpdkCore thread affinity set on CPU core [" +
                  std::to_string(cpuCoreIoEng) + "]");
    } else {
        IOP_DEBUG("Cannot set affinity on CPU core [" +
                  std::to_string(cpuCoreIoEng) + "] for IoEngine");
    }

    while (!ioEngineInitDone) {
    }
    setRunning(1);

    return true;
}

void SpdkBdev::finilizerThreadMain() {
    std::string finThreadName = std::string(spBdevCtx.bdev_name) + "_finalizer";
    pthread_setname_np(pthread_self(), finThreadName.c_str());
    while (isRunning == 3) {
    }
    while (isRunning) {
        io2->dequeue();
        io2->process();
    }
}

int SpdkBdev::ioEngineIoFunction(void *arg) {
    SpdkBdev *bdev = reinterpret_cast<SpdkBdev *>(arg);
    if (bdev->isRunning) {
        uint32_t can_queue_cnt = bdev->canQueue();
        if (can_queue_cnt) {
            bdev->ioEngine->dequeue(can_queue_cnt);
            bdev->ioEngine->process();
        }
    }

    return 0;
}

void SpdkBdev::ioEngineThreadMain() {
    std::string finThreadName = std::string(spBdevCtx.bdev_name) + "_io";
    pthread_setname_np(pthread_self(), finThreadName.c_str());

    struct spdk_thread *spdk_th = spdk_thread_create(spBdevCtx.bdev_name, 0);
    if (!spdk_th) {
        IOP_CRITICAL(
            "Spdk spdk_thread_create() can't create context on pthread");
        deinit();
        spdk_app_stop(-1);
        return;
    }
    spdk_set_thread(spdk_th);

    bool ret = bdevInit();
    if (ret == false) {
        IOP_CRITICAL("Bdev init failed");
        return;
    }

    /*
     * Wait for ready on
     */
    while (isRunning == 3) {
    }

    struct spdk_poller *spdk_io_poller =
        spdk_poller_register(SpdkBdev::ioEngineIoFunction, this, 0);
    if (!spdk_io_poller) {
        IOP_CRITICAL("Spdk poller can't be created");
        spdk_thread_exit(spdk_th);
        deinit();
        spdk_app_stop(-1);
        return;
    }

    /*
     * Ensure only IOs and events for this SPDK thread are processed
     * by this Bdev
     */
    for (;;) {
        if (isRunning == 4)
            break;
        int ret = spdk_thread_poll(spdk_th, 0, 0);
        if (ret < 0)
            break;
    }
    isRunning = 5;

    spdk_poller_unregister(&spdk_io_poller);
}

void SpdkBdev::getBdevGeom(BdevGeom &geom) {
    geom.dev_num = 1;
    geom.type = SpdkDeviceClass::BDEV;
    geom.blk_num[0] = spBdevCtx.blk_num;
}

void SpdkBdev::setMaxQueued(uint32_t io_cache_size, uint32_t blk_size) {}

void SpdkBdev::enableStats(bool en) { statsEnabled = en; }

} // namespace BdevCpp
