/*
 * main.c
 * POC for named eventfd
 *
 * Created on 03/06/2026
 */

// NOTE: Run single threaded (-s)

// fuse 3.14.0 is installed on my system so just using that
// don't uninstall fuse through apt, it will break your system

#include <stdint.h>
#define FUSE_USE_VERSION 31

#include <asm-generic/errno-base.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

#define PATH_MAX 256
#define MAX_FILES 16
#define FILE_TABLE_SZ 64

static struct fuse* afs_fuse = NULL;

// TODO: fuse.h comment on declaration of poll funcn says that sending a single notification
// is sufficient to clear all pending polls. But it is unclear whether sending a single
// notification per file is sufficient, or you need to send a single notification per
// file description. Probably the former, hence a single fuse_pollhandle per file
// should work, but need to confirm this by looking at fuse code (probably around
// FUSE_NOTIFY_POLL in kernel fuse code).
typedef struct afs_file_info {
    bool valid;
    char path[PATH_MAX];
    int eventfd;
    int refcnt;
    struct fuse_pollhandle* pollhandle;
} afs_file_info_t;

static afs_file_info_t files[MAX_FILES];

typedef struct file_description {
    bool valid;
    int fuse_fh;
    afs_file_info_t* afi;
} file_description_t;

static file_description_t open_file_table[FILE_TABLE_SZ];

static int afs_getattr(const char* path, struct stat* stbuf, struct fuse_file_info* fi)
{
    memset(stbuf, 0, sizeof(struct stat));

    if (!strcmp(path, "/")) {
        stbuf->st_mode = S_IFDIR | 777;
        stbuf->st_nlink = 2;
        return 0;
    }

    int idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].valid && !strcmp(files[i].path, path)) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        return -ENOENT;
    }

    int ret = fstat(files[idx].eventfd, stbuf);
    if (ret)
        return -errno;

    return 0;
}

static int afs_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info* fi,
    enum fuse_readdir_flags flags)
{
    if (strcmp(path, "/"))
        return -ENOENT;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].valid)
            filler(buf, files[i].path, NULL, 0, 0);
    }

    return 0;
}

static int afs_open(const char* path, struct fuse_file_info* fi)
{
    int file_table_idx = -1;
    for (int i = 0; i < FILE_TABLE_SZ; i++) {
        if (!open_file_table[i].valid) {
            file_table_idx = i;
            break;
        }
    }
    if (file_table_idx == -1)
        return -EMFILE;

    int files_idx = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (files[i].valid && !strcmp(path, files[i].path)) {
            files_idx = i;
            break;
        }
    }

    // Create a new file if it doesn't exist
    if (files_idx == -1) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (!files[i].valid) {
                files_idx = i;
                break;
            }
        }
        if (files_idx == -1)
            return -ENOSPC;

        strcpy(files[files_idx].path, path);
        files[files_idx].eventfd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK);
        if (files[files_idx].eventfd == -1)
            return -errno;

        files[files_idx].refcnt = 1;
        files[files_idx].pollhandle = NULL;
        files[files_idx].valid = true;
    } else {
        files[files_idx].refcnt++;
    }

    fi->direct_io = 1;
    fi->nonseekable = 1;

    static int fuse_fh_counter = 0;
    fi->fh = fuse_fh_counter++;

    open_file_table[file_table_idx].valid = true;
    open_file_table[file_table_idx].fuse_fh = fi->fh;
    open_file_table[file_table_idx].afi = &files[files_idx];

    return 0;
}

static int afs_release(const char* path, struct fuse_file_info* fi)
{
    int fuse_fh = fi->fh;
    int idx = -1;
    for (int i = 0; i < FILE_TABLE_SZ; i++) {
        if (open_file_table[i].valid && open_file_table[i].fuse_fh == fuse_fh) {
            idx = i;
        }
    }
    assert(idx != -1);

    open_file_table[idx].valid = false;
    afs_file_info_t* afi = open_file_table[idx].afi;

    afi->refcnt--;
    if (!afi->refcnt) {
        afi->valid = false;
        close(afi->eventfd);
    }
}

// HACK: All this code is not thread safe, hence you need to run fuse with a single worker thread. But
// then a blocking read() will block the full filesystem, and since the wake-up event is a write(),
// the system deadlocks. The solution to this is to use the low-level api, for now I'm just
// forcefully making read non-blocking (O_NONBLOCK types). Even with multiple threads you don't really
// wanna block a worker thread because of a blocking call, so ig the only proper solution is to use
// the low-level api eventually.
static int afs_read(const char* path, char* buf, size_t size, off_t offset,
    struct fuse_file_info* fi)
{
    // this is almost certainly the case, doesn't make sense if fi can be NULL but libfuse docs are
    // practically non-existent sigh
    assert(fi != NULL);

    if (size != sizeof(uint64_t) || offset != 0)
        return -EINVAL;

    file_description_t* desc = NULL;
    int fuse_fh = fi->fh;
    for (int i = 0; i < FILE_TABLE_SZ; i++) {
        if (open_file_table[i].valid && open_file_table[i].fuse_fh == fuse_fh) {
            desc = &open_file_table[i];
            break;
        }
    }
    assert(desc != NULL);

    ssize_t ret = read(desc->afi->eventfd, buf, sizeof(uint64_t));
    if (ret != sizeof(uint64_t)) {
        return -errno; // Possibly EAGAIN
    }
    return sizeof(uint64_t);
}

static int afs_write(const char* path, const char* buf, size_t size,
    off_t offset, struct fuse_file_info* fi)
{
    assert(fi != NULL);

    if (size != sizeof(uint64_t) || offset != 0)
        return -EINVAL;

    file_description_t* desc = NULL;
    int fuse_fh = fi->fh;
    for (int i = 0; i < FILE_TABLE_SZ; i++) {
        if (open_file_table[i].valid && open_file_table[i].fuse_fh == fuse_fh) {
            desc = &open_file_table[i];
            break;
        }
    }
    assert(desc != NULL);

    ssize_t ret = write(desc->afi->eventfd, buf, sizeof(uint64_t));
    if (ret != sizeof(uint64_t)) {
        return -errno;
    }
    return sizeof(uint64_t);
}

static int afs_poll(const char* path, struct fuse_file_info* fi,
    struct fuse_pollhandle* ph, unsigned* reventsp)
{
    if (!afs_fuse) {
        struct fuse_context* ctx = fuse_get_context();
        if (ctx)
            afs_fuse = ctx->fuse;
    }

    file_description_t* desc = NULL;
    for (int i = 0; i < FILE_TABLE_SZ; i++) {
        if (open_file_table[i].valid && open_file_table[i].fuse_fh == fi->fh) {
            desc = &open_file_table[i];
            break;
        }
    }
    assert(desc != NULL);

    if (ph)
}

static const struct fuse_operations afs_ops
    = {};

int main(int argc, char** argv)
{
    for (int i = 0; i < MAX_FILES; i++) {
        files[i].valid = false;
    }

    for (int i = 0; i < FILE_TABLE_SZ; i++) {
        open_file_table[i].valid = false;
    }

    int ret = fuse_main(argc, argv, &afs_ops, NULL);

    return ret;
}
