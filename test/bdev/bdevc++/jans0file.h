/*
 * jans0file.h
 *
 *  Created on: Jun 10, 2020
 *      Author: jan.lisowiec@intel.com
 */

#ifndef jans0file_h
#define jans0file_h

namespace BdevCpp {
class FutureBase;
};

#define CONTEXT_MAX_FUTS 256

struct AsyncIoEntry {
    BdevCpp::FutureBase   *fut;
    int                    opcode;
    char                  *data;
    size_t                 data_size;
    void                  *user_data;
};

struct AsyncIoContext {
    AsyncIoEntry   futs[CONTEXT_MAX_FUTS];
    long           tail;
    long           head;
    long           freeFuts;
    bool           full;
    bool           empty;
    std::mutex     opMutex;
    std::condition_variable     opCv;
};

int ModInit();
void ModDeinit();

bool jans_is_filename_sync(const char *name);
bool jans_is_filefd_sync(int fd);
int jans_async_open(const char *name, int flags, mode_t mode = S_IRUSR | S_IWUSR);
int jans_async_close(int fd);
uint64_t jans_async_lseek(int fd, off_t pos, int whence);
int jans_io_setup(int maxevents, io_context_t *ctxp);
int jans_io_destroy(io_context_t ctx);
int jans_io_submit(io_context_t ctx, long nr, struct iocb *ios[]);
int jans_io_cancel(io_context_t ctx, struct iocb *iocb, struct io_event *evt);
int jans_io_getevents(io_context_t ctx_id, long min_nr, long nr, struct io_event *events, struct timespec *timeout);

int jans_sync_open(const char *name, int flags, mode_t mode = S_IRUSR | S_IWUSR);
int jans_sync_close(int fd);
uint64_t jans_sync_lseek(int fd, off_t pos, int whence);
ssize_t jans_pread(int fd, void *buf, size_t count, off_t offset);
ssize_t jans_pwrite(int fd, const void *buf, size_t count, off_t offset);
int jans_stat(const char *path, struct stat *buf);
int jans_async_unlink(const char *pathname);
int jans_sync_unlink(const char *pathname);
int jans_sync_rename(const char *oldpath, const char *newpath);
int jans_async_rename(const char *oldpath, const char *newpath);
void jans_io_prep_pread(struct iocb *iocb, int fd, void *buf, size_t count, long long offset);
void jans_io_prep_pwrite(struct iocb *iocb, int fd, void *buf, size_t count, long long offset);
int jans_fallocate(int fd, int mode, off_t offset, off_t len);

#endif /* jans0file_h */
