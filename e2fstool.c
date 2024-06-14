#define _GNU_SOURCE

#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <stdbool.h>

#include "e2fstool.h"

static ext2_filsys fs = NULL;
static struct ext2fs_numeric_progress_struct progress;

const char *prog_name = "e2fstool";
char *in_file = NULL;
char *out_dir = NULL;
char *conf_dir = NULL;
char *mountpoint;
FILE *contexts, *filesystem;
bool android_configure = false, android_configure_only = false;
bool system_as_root = false;
image_type_t image_type = UNKNOWN;
bool quiet = false;
bool verbose = false;

static void usage(int ret)
{
    fprintf(stderr, "%s [-ehqsvV] [-c config_dir] [-m mountpoint]\n"
                    "\t [-b blocksize] filename [directory]\n",
            prog_name);
    exit(ret);
}

static image_type_t get_image_type(const char *filename)
{
    image_type_t type = UNKNOWN;
    FILE *fp = NULL;
    uint32_t sparse_magic;
    uint16_t ext4_magic;
    int ret;

    fp = fopen(filename, "rb");
    if (!fp)
    {
        E2FSTOOL_ERROR("while opening file");
        return UNKNOWN;
    }

    ret = fread(&sparse_magic, sizeof(sparse_magic), 1, fp);
    if (ret != 1)
    {
        E2FSTOOL_ERROR("while reading sparse_magic number");
        goto end;
    }

    ret = fseek(fp, sparse_magic == SPARSE_HEADER_MAGIC ? 0x460 : 0x438, SEEK_SET);
    if (ret)
    {
        E2FSTOOL_ERROR("while seeking to EXT4 magic offset");
        goto end;
    }

    ret = fread(&ext4_magic, sizeof(ext4_magic), 1, fp);
    if (ret != 1)
    {
        E2FSTOOL_ERROR("while reading ext4_magic number");
        goto end;
    }

    if (sparse_magic == SPARSE_HEADER_MAGIC && ext4_magic == EXT2_SUPER_MAGIC)
    {
        type = SPARSE;
    }
    else if (ext4_magic == EXT2_SUPER_MAGIC)
    {
        type = RAW;
    }

end:
    fclose(fp);
    return type;
}

static char *escape_regex_meta_chars(const char *filepath)
{
    size_t len = strlen(filepath) + 1;
    char *escaped = malloc(len);
    const char *p = filepath;
    char *e = escaped;

    while (*p)
    {
        if (strchr(".^$*+?()[]{}|\\<>", *p))
        {
            char *new_escaped = realloc(escaped, ++len);
            if (new_escaped == NULL)
            {
                free(escaped);
                E2FSTOOL_ERROR("Failed to allocate memory for escaped string %s", filepath);
                exit(EXIT_FAILURE);
            }
            e = new_escaped + (e - escaped);
            escaped = new_escaped;
            *e++ = '\\';
        }
        *e++ = *p++;
    }
    *e = '\0';
    return escaped;
}

errcode_t ino_get_xattr(ext2_filsys fs, ext2_ino_t ino, const char *key, void **val, size_t *val_len)
{
    errcode_t retval, close_retval;
    struct ext2_xattr_handle *xhandle;

    retval = ext2fs_xattrs_open(fs, ino, &xhandle);
    if (retval)
    {
        com_err(__func__, retval, "while opening inode %u", ino);
        return retval;
    }

    retval = ext2fs_xattrs_read(xhandle);
    if (retval)
    {
        com_err(__func__, retval,
                "while reading xattrs of inode %u", ino);
        goto xattrs_close;
    }

    retval = ext2fs_xattr_get(xhandle, key, val, val_len);
    if (retval && retval != EXT2_ET_EA_KEY_NOT_FOUND)
    {
        com_err(__func__, retval,
                "while reading xattrs of inode %u", ino);
        goto xattrs_close;
    }

xattrs_close:
    close_retval = ext2fs_xattrs_close(&xhandle);
    if (close_retval)
    {
        com_err(__func__, close_retval,
                "while closing xattrs of inode %u", ino);
    }
    return retval;
}

static inline errcode_t ino_get_selinux_xattr(ext2_filsys fs, ext2_ino_t ino,
                                              void **val, size_t *val_len)
{
    errcode_t retval = ino_get_xattr(fs, ino, "security." XATTR_SELINUX_SUFFIX, val, val_len);

    return retval == EXT2_ET_EA_KEY_NOT_FOUND ? 0 : retval;
}

static inline errcode_t ino_get_capabilities_xattr(ext2_filsys fs, ext2_ino_t ino,
                                                   uint64_t *cap)
{
    errcode_t retval;
    struct vfs_cap_data *cap_data = NULL;
    size_t len = 0;

    *cap = 0;

    retval = ino_get_xattr(fs, ino, "security." XATTR_CAPS_SUFFIX, (void **)&cap_data, &len);
    if (retval)
    {
        goto end;
    }

    if (cap_data &&
        cap_data->magic_etc & VFS_CAP_REVISION &&
        len == XATTR_CAPS_SZ)
    {
        *cap = cap_data->data[1].permitted;
        *cap <<= 32;
        *cap |= cap_data->data[0].permitted;
    }
    else if (cap_data)
    {
        fprintf(stderr, "%s: Unknown capabilities revision 0x%x\n", __func__, cap_data->magic_etc & VFS_CAP_REVISION_MASK);
    }

end:
    return retval == EXT2_ET_EA_KEY_NOT_FOUND ? 0 : retval;
}

errcode_t ino_get_config(ext2_ino_t ino, struct ext2_inode inode, const char *path)
{
    char *ctx = NULL;
    size_t ctx_len;
    uint64_t cap;
    errcode_t retval = 0;

    retval = ino_get_selinux_xattr(fs, ino, (void **)&ctx, &ctx_len);
    if (retval)
    {
        return retval;
    }

    retval = ino_get_capabilities_xattr(fs, ino, &cap);
    if (retval)
    {
        return retval;
    }

    fprintf(filesystem, "%s %u %u %o", ino == EXT2_ROOT_INO ? "/" : path, inode.i_uid, inode.i_gid, inode.i_mode & FILE_MODE_MASK);

    if (cap)
    {
        fprintf(filesystem, " capabilities=%llu", cap);
    }
    fputc('\n', filesystem);

    if (ctx)
    {
        char *context = NULL, *start = NULL, *escaped = NULL;
        size_t len = ctx_len + 2;

        if (ino == EXT2_ROOT_INO)
        {
            len += 6;
        }

        if (ino != EXT2_ROOT_INO || !system_as_root)
        {
            if (system_as_root)
                path++;
            escaped = escape_regex_meta_chars(path);
            len += strlen(escaped) + 1;
        }

        context = malloc(len);
        start = context;
        if (!context)
        {
            E2FSTOOL_ERROR("while allocating memory");
            exit(EXIT_FAILURE);
        }

        if (ino != EXT2_ROOT_INO || !system_as_root)
        {
            context += snprintf(context, strlen(escaped) + 2, "/%s", escaped);
            free(escaped);
        }

        if (ino == EXT2_ROOT_INO)
        {
            context += snprintf(context, 7, "(/.*)?");
        }

        context += snprintf(context, ctx_len + 2, " %s\n", ctx);
        fwrite(start, 1, len - 1, contexts);
        free(start);
    }
    return retval;
}

errcode_t ino_extract_symlink(ext2_filsys fs, ext2_ino_t ino, struct ext2_inode *inode,
                              const char *path)
{
    ext2_file_t e2_file;
    char *link_target = NULL;
    __u32 i_size = inode->i_size;
    errcode_t retval = 0;

    link_target = malloc(i_size + 1);
    if (!link_target)
    {
        E2FSTOOL_ERROR("while allocating memory");
        return EXT2_ET_NO_MEMORY;
    }

    if (i_size < SYMLINK_I_BLOCK_MAX_SIZE)
    {
        strncpy(link_target, (char *)inode->i_block, i_size + 1);
    }
    else
    {
        unsigned bytes = i_size;
        char *p = link_target;
        retval = ext2fs_file_open(fs, ino, 0, &e2_file);
        if (retval)
        {
            com_err(__func__, retval, "while opening ex2fs symlink");
            goto end;
        }
        for (;;)
        {
            unsigned int got;
            retval = ext2fs_file_read(e2_file, p, bytes, &got);
            if (retval)
            {
                com_err(__func__, retval, "while reading ex2fs symlink");
                goto end;
            }
            bytes -= got;
            p += got;
            if (got == 0 || bytes == 0)
                break;
        }
        link_target[i_size] = '\0';
        retval = ext2fs_file_close(e2_file);
        if (retval)
        {
            com_err(__func__, retval, "while closing symlink");
            goto end;
        }
    }

    retval = symlink(link_target, path);
    if (retval == -1)
    {
        E2FSTOOL_ERROR("while creating symlink");
    }

end:
    free(link_target);
    return retval;
}

errcode_t ino_extract_regular(ext2_filsys fs, ext2_ino_t ino, const char *path)
{
    ext2_file_t e2_file;
    struct ext2_inode inode;
    char *buf = NULL;
    int fd, nbytes;
    unsigned int written = 0, got;
    errcode_t retval = 0, close_retval = 0;

    retval = ext2fs_read_inode(fs, ino, &inode);
    if (retval)
    {
        com_err(__func__, retval, "while reading file inode %u", ino);
        return retval;
    }

    fd = open(path, O_WRONLY | O_TRUNC | O_BINARY | O_CREAT, 0644);
    if (fd < 0)
    {
        E2FSTOOL_ERROR("while creating file");
        return -1;
    }

    retval = ext2fs_file_open(fs, ino, 0, &e2_file);
    if (retval)
    {
        com_err(__func__, retval, "while opening ext2 file");
        goto end;
    }

    retval = ext2fs_get_mem(FILE_READ_BUFLEN, &buf);
    if (retval)
    {
        com_err(__func__, retval, "while allocating memory");
        goto end;
    }

    do
    {
        retval = ext2fs_file_read(e2_file, buf, FILE_READ_BUFLEN, &got);
        if (retval)
        {
            com_err(__func__, retval, "while reading ext2 file");
            goto quit;
        }

        nbytes = write(fd, buf, got);
        if (nbytes < 0)
        {
            if (errno & (EINTR | EAGAIN))
            {
                continue;
            }
            E2FSTOOL_ERROR("while writing file");
            retval = -1;
            goto close;
        }

        got -= nbytes;
        written += nbytes;
    } while (got);

    if (inode.i_size != written)
    {
        E2FSTOOL_ERROR("while writing file (%u of %u)", written, inode.i_size);
        retval = -1;
    }

close:
    close_retval = ext2fs_file_close(e2_file);
    if (close_retval)
        com_err(__func__, close_retval, "while closing ext2 file\n");
quit:
    ext2fs_free_mem(&buf);
end:
    close(fd);
    return retval ?: close_retval;
}

int walk_dir(ext2_ino_t dir,
             int flags EXT2FS_ATTR((unused)),
             struct ext2_dir_entry *de,
             int offset EXT2FS_ATTR((unused)),
             int blocksize EXT2FS_ATTR((unused)),
             char *buf EXT2FS_ATTR((unused)), void *priv_data)
{
    __u16 name_len;
    char *output_file;
    struct ext2_inode inode;
    struct inode_params *params = (struct inode_params *)priv_data;
    errcode_t retval = 0;

    name_len = de->name_len & 0xff;
    if (!strncmp(de->name, ".", name_len) ||
        !strncmp(de->name, "..", name_len))
        return 0;

    if (asprintf(&params->filename, "%s/%.*s", params->path, name_len, de->name) < 0)
    {
        E2FSTOOL_ERROR("while allocating memory");
        return EXT2_ET_NO_MEMORY;
    }

    if (!android_configure_only)
    {
        if (asprintf(&output_file, "%s%s", out_dir,
                     params->filename) < 0)
        {
            E2FSTOOL_ERROR("while allocating memory");
            retval = EXT2_ET_NO_MEMORY;
            goto end;
        }
    }

    retval = ext2fs_read_inode(fs, de->inode, &inode);
    if (retval)
    {
        com_err(__func__, retval, "while reading inode %u", de->inode);
        goto err;
    }

    if (android_configure)
    {
        char *config_path;
        retval = asprintf(&config_path, "%s%s", mountpoint, params->filename);
        if (retval < 0 || !config_path)
        {
            E2FSTOOL_ERROR("while allocating memory");
            retval = EXT2_ET_NO_MEMORY;
            goto err;
        }

        retval = ino_get_config(de->inode, inode, config_path);
        free(config_path);

        if (retval)
            goto err;
    }

    if (!quiet)
    {
        ext2fs_numeric_progress_update(fs, &progress, de->inode - RESERVED_INODES_COUNT);
    }

    if (dir == EXT2_ROOT_INO &&
        !strncmp(de->name, "lost+found", name_len))
    {
        goto err;
    }

    if (!quiet && verbose)
    {
        fprintf(stdout, "Extracting %s\n", params->filename + 1);
    }

    if (android_configure_only &&
        (inode.i_mode & LINUX_S_IFMT) != LINUX_S_IFDIR)
    {
        goto err;
    }

    switch (inode.i_mode & LINUX_S_IFMT)
    {
    case LINUX_S_IFCHR:
    case LINUX_S_IFBLK:
    case LINUX_S_IFIFO:
#if !defined(_WIN32) || defined(SVB_WIN32)
#if defined(S_IFSOCK) && !defined(SVB_WIN32)
    case LINUX_S_IFSOCK:
#endif
    case LINUX_S_IFLNK:
        retval = ino_extract_symlink(fs, de->inode, &inode, output_file);
        if (retval)
        {
            goto err;
        }
        break;
#endif
    case LINUX_S_IFREG:
        retval = ino_extract_regular(fs, de->inode, output_file);
        if (retval)
        {
            goto err;
        }
        break;
    case LINUX_S_IFDIR:;
        char *cur_path = params->path;
        char *cur_filename = params->filename;
        params->path = params->filename;

        if (!android_configure_only)
        {
            retval = mkdir(output_file, inode.i_mode & FILE_MODE_MASK);
            if (retval == -1 && errno != EEXIST)
            {
                E2FSTOOL_ERROR("while creating %s", output_file);
                goto err;
            }
        }
        retval = ext2fs_dir_iterate2(fs, de->inode, 0, NULL,
                                     walk_dir, params);
        if (retval)
        {
            goto err;
        }
        params->path = cur_path;
        params->filename = cur_filename;
        break;
    default:
        E2FSTOOL_ERROR("warning: unknown entry \"%s\" (%x)", params->filename, inode.i_mode & LINUX_S_IFMT);
    }

#ifdef SVB_MINGW
    if (!android_configure_only)
    {
        retval = set_path_timestamp(output_file, inode.i_atime, inode.i_mtime, inode.i_ctime);
        if (retval)
        {
            E2FSTOOL_ERROR("while configuring timestamps for %s", output_file);
        }
    }
#endif

err:
    if (!android_configure_only)
    {
        free(output_file);
    }
end:
    free(params->filename);
    return retval;
}

static errcode_t walk_fs(ext2_filsys fs)
{
    struct ext2_inode inode;
    struct inode_params params = {
        .path = "",
    };
    char *se_path, *fs_path;
    errcode_t retval = 0;

    retval = ext2fs_read_inode(fs, EXT2_ROOT_INO, &inode);
    if (retval)
    {
        com_err(__func__, retval, "while reading root inode");
        return retval;
    }

    if (!android_configure_only)
    {
        retval = mkdir(out_dir, inode.i_mode);
        if (retval == -1 && errno != EEXIST)
        {
            E2FSTOOL_ERROR("while creating %s", out_dir);
            return retval;
        }
    }
    if (android_configure)
    {
        if (mountpoint)
            ;
        else if (fs->super->s_last_mounted[0])
            mountpoint = (char *)fs->super->s_last_mounted;
        else if (fs->super->s_volume_name[0])
            mountpoint = (char *)fs->super->s_volume_name;
        else
            mountpoint = out_dir;

        ++mountpoint;
        if (!mountpoint[0])
            system_as_root = true;

        retval = mkdir(conf_dir, S_IRWXU | S_IRWXG | S_IRWXO);
        if (retval == -1 && errno != EEXIST)
        {
            E2FSTOOL_ERROR("while creating %s", conf_dir);
            return retval;
        }

        asprintf(&se_path, "%s/selinux_contexts.fs", conf_dir);
        contexts = fopen(se_path, "w");
        if (!contexts)
        {
            retval = -1;
            goto ctx_end;
        }

        asprintf(&fs_path, "%s/filesystem_config.fs", conf_dir);
        filesystem = fopen(fs_path, "w");
        if (!filesystem)
        {
            retval = -1;
            goto fs_end;
        }

        retval = ino_get_config(EXT2_ROOT_INO, inode, (char *)mountpoint);
        if (retval)
            goto end;
    }

    if (!quiet && !verbose)
        ext2fs_numeric_progress_init(fs, &progress,
                                     "Extracting filesystem inodes: ",
                                     fs->super->s_inodes_count - fs->super->s_free_inodes_count - RESERVED_INODES_COUNT);

    retval = ext2fs_dir_iterate2(fs, EXT2_ROOT_INO, 0, NULL, walk_dir,
                                 &params);
    if (retval)
    {
        goto end;
    }

    if (!quiet && !verbose)
        ext2fs_numeric_progress_close(fs, &progress, "done\n");
end:
    if (android_configure)
    {
        free(fs_path);
        fclose(filesystem);
    }
fs_end:
    if (android_configure)
    {
        free(se_path);
        fclose(contexts);
    }
ctx_end:
    return retval;
}

int main(int argc, char *argv[])
{
    int c, show_version_only = 0;
    io_manager io_mgr = unix_io_manager;
    errcode_t retval = 0, close_retval = 0;
    unsigned int b, blocksize = 0;

    add_error_table(&et_ext2_error_table);

    while ((c = getopt(argc, argv, "b:c:ehm:oqsvV")) != EOF)
    {
        switch (c)
        {
        case 'b':
            blocksize = parse_num_blocks2(optarg, -1);
            b = (blocksize > 0) ? blocksize : -blocksize;
            if (b < EXT2_MIN_BLOCK_SIZE ||
                b > EXT2_MAX_BLOCK_SIZE)
            {
                com_err(prog_name, 0,
                        "invalid block size - %s", optarg);
                exit(EXIT_FAILURE);
            }
            if (blocksize > 4096)
                fprintf(stderr, "Warning: blocksize %d not "
                                "usable on most systems.\n",
                        blocksize);
            break;
        case 'c':
            conf_dir = strdup(optarg);
            ++android_configure;
            break;
        case 'e':
            image_type = RAW;
            break;
        case 's':
            image_type = SPARSE;
            break;
        case 'o':
            android_configure_only++;
            break;
        case 'm':
            if (*optarg != '/')
            {
                fprintf(stderr, "Invalid mountpoint %s", optarg);
                exit(EXIT_FAILURE);
            }
            mountpoint = strdup(optarg);
            break;
        case 'h':
            usage(EXIT_SUCCESS);
        case 'q':
            ++quiet;
            break;
        case 'v':
            ++verbose;
            break;
        case 'V':
            ++show_version_only;
            break;
        default:
            usage(EXIT_FAILURE);
        }
    }

    if (!show_version_only)
    {
        if (optind >= argc)
        {
            fprintf(stderr, "Expected filename after options\n");
            usage(EXIT_FAILURE);
        }

        in_file = strdup(argv[optind++]);

        if (!android_configure_only)
        {
            if (optind >= argc)
            {
                fprintf(stderr, "Expected directory after options\n");
                usage(EXIT_FAILURE);
            }

            out_dir = strdup(argv[optind++]);
        }

        if (android_configure_only &&
            !android_configure)
        {
            fprintf(stderr, "Cant use option: -o without -c\n");
            usage(EXIT_FAILURE);
        }

        if (optind < argc)
        {
            fprintf(stderr, "Unexpected argument: %s\n", argv[optind]);
            usage(EXIT_FAILURE);
        }

        if (android_configure_only &&
            !android_configure)
        {
            fprintf(stderr, "Cannot use option: -o without -c\n");
            usage(EXIT_FAILURE);
        }
    }

    if (!quiet || show_version_only)
        printf("e2fstool %s (%s)\n\n", E2FSTOOL_VERSION,
               E2FSTOOL_DATE);

    if (show_version_only)
    {
        printf("Using %s\n",
               error_message(EXT2_ET_BASE));
        exit(EXIT_SUCCESS);
    }

    if (image_type == UNKNOWN)
    {
        image_type = get_image_type(in_file);
        if (image_type == UNKNOWN)
        {
            fprintf(stderr, "Unknown image type\n");
            usage(EXIT_FAILURE);
        }
    }

    if (!quiet)
    {
        printf("Opening %s image file", image_type == SPARSE ? "SPARSE" : "RAW");
        if (blocksize)
            printf(" with blocksize of %u", blocksize);
        printf(": ");
    }

    if (image_type == SPARSE)
    {
        char *new_in_file = NULL;
        io_mgr = sparse_io_manager;
        if (asprintf(&new_in_file, "(%s)", in_file) == -1)
        {
            E2FSTOOL_ERROR("while allocating file name");
            exit(EXIT_FAILURE);
        }
        free(in_file);
        in_file = new_in_file;
    }

    retval = ext2fs_open(in_file, EXT2_FLAG_64BITS | EXT2_FLAG_EXCLUSIVE | EXT2_FLAG_THREADS | EXT2_FLAG_PRINT_PROGRESS, 0, blocksize, io_mgr, &fs);
    if (retval)
    {
        puts("\n");
        com_err(prog_name, retval, "while opening file %s", in_file);
        exit(EXIT_FAILURE);
    }

    if (!quiet)
    {
        puts("done");
    }

    retval = walk_fs(fs);
    if (retval)
        goto end;

    if (!quiet && !android_configure_only)
    {
        fprintf(stdout, "\nWritten %u inodes (%u blocks) to \"%s\"\n",
                fs->super->s_inodes_count - fs->super->s_free_inodes_count,
                fs->super->s_blocks_count - fs->super->s_free_blocks_count -
                    RESERVED_INODES_COUNT,
                out_dir);
    }
end:
    close_retval = ext2fs_close_free(&fs);
    if (close_retval)
    {
        com_err(prog_name, close_retval, "%s",
                "while closing filesystem");
    }
    if (retval)
    {
        com_err(prog_name, retval, "%s",
                "while walking filesystem");
    }

    free(in_file);
    free(out_dir);
    free(conf_dir);
    if (mountpoint)
        free(android_configure ? --mountpoint : mountpoint);
    remove_error_table(&et_ext2_error_table);
    return (close_retval || retval) ? EXIT_FAILURE : EXIT_SUCCESS;
}
