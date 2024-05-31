#ifndef E2FSTOOL_H_INC
#define E2FSTOOL_H_INC

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sparse/sparse.h>

#include <assert.h>
#include <e2p/e2p.h>
#include <ext2fs/ext2fs.h>
#include <ext2fs/ext2fsP.h>

#include <private/android_filesystem_capability.h>

#define E2FSTOOL_VERSION "1.0.0"
#define E2FSTOOL_DATE "31-May-2024"

#define E2FSTOOL_ERROR(pfx, ...) printf("%s: %s" pfx "\n", __func__, strerror(errno) __VA_OPT__(,) __VA_ARGS__)

#ifdef SVB_MINGW
#ifdef HAVE_LIB_NT_H
#include "libnt.h"
#endif

#define mkdir(p, m) mkdir(p)
#endif

#ifndef XATTR_SELINUX_SUFFIX
#define XATTR_SELINUX_SUFFIX "selinux"
#endif
#ifndef XATTR_CAPS_SUFFIX
#define XATTR_CAPS_SUFFIX "capability"
#endif

#define FILE_MODE_MASK 0x0FFF
#define FILE_READ_BUFLEN (1 << 26)
#define RESERVED_INODES_COUNT 0xA /* Excluding EXT2_ROOT_INO */
#define SYMLINK_I_BLOCK_MAX_SIZE 0x3D

#define SPARSE_HEADER_MAGIC 0xed26ff3a
#define EXT4_SUPERBLOCK_MAGIC 0xef53

typedef enum image_type {
    SPARSE,
    RAW,
    UNKNOWN
} image_type_t;

struct inode_params {
    char *path;
    char *filename;
    size_t len;
};
#endif /* E2FSTOOL_H_INC */
