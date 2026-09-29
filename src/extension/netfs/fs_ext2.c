/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of uvroot.
 *
 * Copyright (C) 2026 uvroot Developers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

/*
 * ext2/3/4 filesystem driver, built on the e2fsprogs user-space library
 * (libext2fs).  It turns a block backend into a directory tree without
 * any privilege, loop device or /dev/fuse access.
 *
 * Ownership and permissions come from the on-disk inodes, so they are
 * stored inside the image itself and are completely independent from
 * the .uvroot-vperm database used by non-block mounts.
 *
 * The library is resolved with dlopen() so uvroot keeps no dependency on
 * it; when it is missing the driver simply reports that it is
 * unavailable.
 *
 * This first stage implements the read path (list, stat, get).  The
 * write path (put/mkdir/rmdir/unlink/rename and in-image chmod/chown)
 * is the next step and is reported as read-only for now.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <talloc.h>

#include "build.h"
#include "cli/note.h"
#include "extension/netfs/netfs.h"

#ifdef HAVE_LIBEXT2FS

#include <ext2fs/ext2fs.h>
#include <dlfcn.h>

typedef struct Ext2Api {
    void *handle;
    errcode_t(*open) (const char *name, int flags, int superblock,
		      unsigned int block_size, io_manager manager,
		      ext2_filsys * ret_fs);
    errcode_t(*close) (ext2_filsys fs);
    errcode_t(*read_inode) (ext2_filsys fs, ext2_ino_t ino,
			    struct ext2_inode *inode);
    errcode_t(*dir_iterate) (ext2_filsys fs, ext2_ino_t dir, int flags,
			     char *block_buf,
			     int (*func) (struct ext2_dir_entry * dirent,
					  int offset, int blocksize,
					  char *buf, void *priv_data),
			     void *priv_data);
    errcode_t(*lookup) (ext2_filsys fs, ext2_ino_t dir, const char *name,
			int namelen, char *buf, ext2_ino_t *inode);
    errcode_t(*file_open) (ext2_filsys fs, ext2_ino_t ino, int flags,
			   ext2_file_t *ret);
    errcode_t(*file_read) (ext2_file_t file, void *buf, unsigned int wanted,
			   unsigned int *got);
    errcode_t(*file_close) (ext2_file_t file);
    const char *(*error_message) (errcode_t code);
    errcode_t(*read_bitmaps) (ext2_filsys fs);
    errcode_t(*flush) (ext2_filsys fs);
    errcode_t(*write_inode) (ext2_filsys fs, ext2_ino_t ino,
			     struct ext2_inode *inode);
    errcode_t(*write_new_inode) (ext2_filsys fs, ext2_ino_t ino,
				 struct ext2_inode *inode);
    errcode_t(*new_inode) (ext2_filsys fs, ext2_ino_t dir, int mode,
			   ext2fs_inode_bitmap map, ext2_ino_t *ret);
    errcode_t(*link) (ext2_filsys fs, ext2_ino_t dir, const char *name,
		      ext2_ino_t ino, int flags);
    errcode_t(*unlink) (ext2_filsys fs, ext2_ino_t dir, const char *name,
			ext2_ino_t ino, int flags);
    errcode_t(*mkdir) (ext2_filsys fs, ext2_ino_t parent, ext2_ino_t inum,
		       const char *name);
    errcode_t(*symlink) (ext2_filsys fs, ext2_ino_t parent, ext2_ino_t ino,
			 const char *name, const char *target);
    int (*is_fast_symlink) (struct ext2_inode *inode);
    errcode_t(*write_inode_bitmap) (ext2_filsys fs);
    errcode_t(*punch) (ext2_filsys fs, ext2_ino_t ino,
		       struct ext2_inode * inode, char *block_buf,
		       blk64_t start, blk64_t end);
    void (*inode_alloc_stats2) (ext2_filsys fs, ext2_ino_t ino, int inuse,
				int isdir);
    errcode_t(*file_write) (ext2_file_t file, const void *buf,
			    unsigned int nbytes, unsigned int *written);
    errcode_t(*file_set_size2) (ext2_file_t file, ext2_off64_t size);
    io_manager manager;		/* unix_io_manager, copied by value */
} Ext2Api;

static Ext2Api ext2_api;
static bool ext2_api_ready = false;

/*
 * io_manager serving NetfsBlockOps directly.  It lets a filesystem be
 * read from anything that is not a plain file (an NBD or iSCSI export,
 * a QCOW2 image, ...).  Local raw images keep using the library's own
 * unix_io_manager, which is better tested; a backend opts into this one
 * with mount->custom_io, and UVROOT_NETFS_FORCE_CUSTOM_IO forces it for
 * testing that path with a local image.
 *
 * The mount pointer is passed through the channel's private_data, never
 * app_data: e2fsprogs overwrites app_data with its own ext2_filsys.
 */
static NetfsMount *ext2_io_mount = NULL;
static struct struct_io_manager netfs_io_manager_decl;

static errcode_t netfs_io_read_error(io_channel channel UNUSED,
				     unsigned long block UNUSED,
				     int count UNUSED, void *data UNUSED,
				     size_t size UNUSED, int actual UNUSED,
				     errcode_t error)
{
    return error;
}

static errcode_t netfs_io_write_error(io_channel channel UNUSED,
				      unsigned long block UNUSED,
				      int count UNUSED,
				      const void *data UNUSED,
				      size_t size UNUSED, int actual UNUSED,
				      errcode_t error)
{
    return error;
}

static errcode_t netfs_io_open(const char *name UNUSED, int flags UNUSED,
			       io_channel *channel)
{
    io_channel result;

    if (ext2_io_mount == NULL)
	return EXT2_ET_BAD_DEVICE_NAME;

    result = calloc(1, sizeof(*result));
    if (result == NULL)
	return ENOMEM;

    result->magic = EXT2_ET_MAGIC_IO_CHANNEL;
    result->manager = (io_manager) &netfs_io_manager_decl;
    result->name = strdup(name != NULL ? name : "netfs");
    result->block_size = 1024;
    result->read_error = netfs_io_read_error;
    result->write_error = netfs_io_write_error;
    /*
     * The mount is kept in private_data, *not* in app_data: e2fsprogs
     * overwrites app_data with its own ext2_filsys right after open()
     * returns (see ext2fs_open2()), which used to make every block
     * transfer read a bogus mount pointer and fail with
     * EXT2_ET_BAD_DEVICE_NAME ("Unknown code ext2 58").
     */
    result->private_data = ext2_io_mount;
    *channel = result;
    return 0;
}

static errcode_t netfs_io_flush_slots(NetfsMount *mount);

static errcode_t netfs_io_close(io_channel channel)
{
    {
	NetfsMount *mount = channel->private_data;

	if (mount != NULL)
	    (void) netfs_io_flush_slots(mount);
    }

    if (channel == NULL)
	return 0;
    free(channel->name);
    free(channel);
    return 0;
}

static errcode_t netfs_io_set_blksize(io_channel channel, int blksize)
{
    channel->block_size = blksize;
    return 0;
}

/* Serve a byte range, coping with short reads/writes.  */
#define NETFS_IO_WINDOW (1024u * 1024u)

/*
 * ^C while a transfer is running: uvroot ignores the signal until the
 * syscall it is handling returns, so a big push could not be stopped
 * for minutes.  The signal handler raises this flag and the transfer
 * gives up with EINTR instead.
 */
static bool netfs_io_interrupted(void)
{
    if (netfs_interrupt == 0)
	return false;

    netfs_interrupt = 0;
    return true;
}
#define NETFS_IO_SLOTS (sizeof(((NetfsMount *) 0)->io_slot) \
			/ sizeof(((NetfsMount *) 0)->io_slot[0]))

static errcode_t netfs_io_flush_slot(NetfsMount *mount, int index)
{
    ssize_t done;

    if (!mount->io_slot[index].valid || !mount->io_slot[index].dirty)
	return 0;

    if (mount->backend->block.pwrite == NULL)
	return EXT2_ET_OP_NOT_SUPPORTED;

    done = mount->backend->block.pwrite(mount, mount->io_slot[index].data,
					mount->io_slot[index].length,
					mount->io_slot[index].base);
    if (done < 0)
	return (errcode_t) (-done);
    if ((size_t) done != mount->io_slot[index].length)
	return EXT2_ET_SHORT_WRITE;

    mount->io_slot[index].dirty = false;
    mount->dbg_write_calls++;
    mount->dbg_write_bytes += mount->io_slot[index].length;
    return 0;
}

static errcode_t netfs_io_flush_slots(NetfsMount *mount)
{
    size_t i;

    for (i = 0; i < NETFS_IO_SLOTS; i++) {
	errcode_t status;

	if (netfs_io_interrupted())
	    return EINTR;

	status = netfs_io_flush_slot(mount, (int) i);
	if (status != 0)
	    return status;
    }
    return 0;
}

static void netfs_io_drop_slots(NetfsMount *mount)
{
    size_t i;

    for (i = 0; i < NETFS_IO_SLOTS; i++) {
	mount->io_slot[i].valid = false;
	mount->io_slot[i].dirty = false;
	mount->io_slot[i].base = ~(uint64_t) 0;
    }
}

/*
 * Return the slot covering @offset, loading it if needed.  Written data
 * stays in the slot until it is evicted or flushed, so libext2fs's
 * block-at-a-time updates travel in whole windows instead of one
 * network round trip per block.
 */
static errcode_t netfs_io_slot(NetfsMount *mount, uint64_t offset,
			       bool writing, bool full_window, int *index)
{
    uint64_t base = offset & ~((uint64_t) NETFS_IO_WINDOW - 1);
    size_t i, victim = 0;
    unsigned long oldest = ~0UL;
    errcode_t status;
    ssize_t done;

    for (i = 0; i < NETFS_IO_SLOTS; i++) {
	if (mount->io_slot[i].valid && mount->io_slot[i].base == base) {
	    mount->io_slot[i].stamp = ++mount->io_stamp;
	    *index = (int) i;
	    return 0;
	}
	if (!mount->io_slot[i].valid) {
	    victim = i;
	    oldest = 0;
	    break;
	}
	if (mount->io_slot[i].stamp < oldest) {
	    oldest = mount->io_slot[i].stamp;
	    victim = i;
	}
    }

    status = netfs_io_flush_slot(mount, (int) victim);
    if (status != 0)
	return status;

    if (mount->io_slot[victim].data == NULL) {
	mount->io_slot[victim].data = talloc_size(mount, NETFS_IO_WINDOW);
	if (mount->io_slot[victim].data == NULL)
	    return ENOMEM;
    }

    mount->io_slot[victim].base = base;
    mount->io_slot[victim].valid = true;
    mount->io_slot[victim].dirty = false;
    mount->io_slot[victim].length = NETFS_IO_WINDOW;
    mount->io_slot[victim].stamp = ++mount->io_stamp;

    /*
     * Only a write that really covers the whole window may skip the
     * read.  A smaller one has to be merged with the existing content,
     * otherwise the untouched part of the window would be written back
     * as garbage -- which corrupts the filesystem.
     */
    if (writing && full_window && offset == base) {
	*index = (int) victim;
	return 0;
    }

    if (mount->backend->block.pread == NULL)
	return EXT2_ET_OP_NOT_SUPPORTED;

    done = mount->backend->block.pread(mount, mount->io_slot[victim].data,
				       NETFS_IO_WINDOW, base);
    if (done < 0) {
	mount->io_slot[victim].valid = false;
	return (errcode_t) (-done);
    }
    if (done == 0) {
	mount->io_slot[victim].valid = false;
	return EXT2_ET_SHORT_READ;
    }

    mount->io_slot[victim].length = (size_t) done;
    mount->dbg_read_calls++;
    mount->dbg_read_bytes += (size_t) done;
    *index = (int) victim;
    return 0;
}

static errcode_t netfs_io_transfer(io_channel channel, uint64_t offset,
				   size_t length, void *data, bool writing)
{
    NetfsMount *mount = channel->private_data;
    char *cursor = data;

    if (mount == NULL || mount->backend == NULL)
	return EXT2_ET_BAD_DEVICE_NAME;

    /*
     * libext2fs asks for one block at a time: without this, a network
     * backend pays a round trip for each 4 KiB.  Small requests are
     * served from the slot cache; big ones go straight through.
     */
    while (length > 0 && length <= NETFS_IO_WINDOW) {
	int index;
	size_t in_window, chunk;
	errcode_t status;

	if (netfs_io_interrupted())
	    return EINTR;

	status = netfs_io_slot(mount, offset, writing,
			       length >= NETFS_IO_WINDOW, &index);
	if (status == EXT2_ET_SHORT_READ)
	    break;
	if (status != 0)
	    return status;

	in_window = (size_t) (offset - mount->io_slot[index].base);
	if (in_window >= mount->io_slot[index].length)
	    break;

	chunk = mount->io_slot[index].length - in_window;
	if (chunk > length)
	    chunk = length;

	if (writing) {
	    memcpy(mount->io_slot[index].data + in_window, cursor, chunk);
	    mount->io_slot[index].dirty = true;
	} else {
	    memcpy(cursor, mount->io_slot[index].data + in_window, chunk);
	}

	cursor += chunk;
	offset += chunk;
	length -= chunk;
    }
    if (length == 0)
	return 0;

    /* Anything left over goes directly to the backend.  */
    {
	errcode_t status = netfs_io_flush_slots(mount);

	if (status != 0)
	    return status;
    }

    while (length > 0) {
	size_t chunk = length > 65536 ? 65536 : length;
	ssize_t done;

	if (netfs_io_interrupted())
	    return EINTR;

	if (writing) {
	    if (mount->backend->block.pwrite == NULL)
		return EXT2_ET_OP_NOT_SUPPORTED;
	    done = mount->backend->block.pwrite(mount, cursor, chunk, offset);
	} else {
	    if (mount->backend->block.pread == NULL)
		return EXT2_ET_OP_NOT_SUPPORTED;
	    done = mount->backend->block.pread(mount, cursor, chunk, offset);
	}

	if (done < 0)
	    return (errcode_t) (-done);
	if (done == 0)
	    return EXT2_ET_SHORT_READ;

	cursor += done;
	offset += (uint64_t) done;
	length -= (size_t) done;
    }

    return 0;
}

static errcode_t netfs_io_read_blk64(io_channel channel,
				     unsigned long long block, int count,
				     void *data)
{
    /* A negative count means "this many bytes", not blocks.  */
    if (count < 0)
	return netfs_io_transfer(channel,
				 block * (uint64_t) channel->block_size,
				 (size_t) (-count), data, false);

    return netfs_io_transfer(channel, block * (uint64_t) channel->block_size,
			     (size_t) count * channel->block_size, data,
			     false);
}

static errcode_t netfs_io_write_blk64(io_channel channel,
				      unsigned long long block, int count,
				      const void *data)
{
    if (count < 0)
	return netfs_io_transfer(channel,
				 block * (uint64_t) channel->block_size,
				 (size_t) (-count), (void *) data, true);

    return netfs_io_transfer(channel, block * (uint64_t) channel->block_size,
			     (size_t) count * channel->block_size,
			     (void *) data, true);
}

static errcode_t netfs_io_read_blk(io_channel channel, unsigned long block,
				   int count, void *data)
{
    return netfs_io_read_blk64(channel, block, count, data);
}

static errcode_t netfs_io_write_blk(io_channel channel, unsigned long block,
				    int count, const void *data)
{
    return netfs_io_write_blk64(channel, block, count, data);
}

static errcode_t netfs_io_flush(io_channel channel)
{
    NetfsMount *mount = channel->private_data;
    errcode_t status;

    if (mount == NULL || mount->backend == NULL)
	return 0;

    status = netfs_io_flush_slots(mount);
    if (status != 0)
	return status;

    if (mount->backend->block.flush == NULL)
	return 0;

    {
	int status = mount->backend->block.flush(mount);

	return status < 0 ? (errcode_t) (-status) : 0;
    }
}

static errcode_t netfs_io_set_option(io_channel channel UNUSED,
				     const char *option UNUSED,
				     const char *argument UNUSED)
{
    return 0;
}

static struct struct_io_manager netfs_io_manager_decl = {
    .magic = EXT2_ET_MAGIC_IO_MANAGER,
    .name = "netfs",
    .open = netfs_io_open,
    .close = netfs_io_close,
    .set_blksize = netfs_io_set_blksize,
    .read_blk = netfs_io_read_blk,
    .write_blk = netfs_io_write_blk,
    .flush = netfs_io_flush,
    .write_byte = NULL,
    .set_option = netfs_io_set_option,
    .get_stats = NULL,
    .read_blk64 = netfs_io_read_blk64,
    .write_blk64 = netfs_io_write_blk64,
};

typedef struct Ext2Data {
    ext2_filsys fs;
    bool read_only;
} Ext2Data;

static bool load_ext2_api(void)
{
    static const char *candidates[] = {
	"libext2fs.so.2", "libext2fs.so", NULL
    };
    void *handle = NULL;
    size_t i;

    if (ext2_api_ready)
	return true;

    for (i = 0; candidates[i] != NULL && handle == NULL; i++)
	handle = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
	VERBOSE(NULL, 1, "netfs: libext2fs not found: %s", dlerror());
	return false;
    }

    memset(&ext2_api, 0, sizeof(ext2_api));
    ext2_api.handle = handle;

#define RESOLVE(field, symbol)						\
    do {								\
	*(void **) (&ext2_api.field) = dlsym(handle, symbol);		\
	if (ext2_api.field == NULL) {					\
	    VERBOSE(NULL, 1, "netfs: %s not found in libext2fs", symbol); \
	    dlclose(handle);						\
	    ext2_api.handle = NULL;					\
	    return false;						\
	}								\
    } while (0)

    RESOLVE(open, "ext2fs_open");
    RESOLVE(close, "ext2fs_close");
    RESOLVE(read_inode, "ext2fs_read_inode");
    RESOLVE(dir_iterate, "ext2fs_dir_iterate");
    RESOLVE(lookup, "ext2fs_lookup");
    RESOLVE(file_open, "ext2fs_file_open");
    RESOLVE(file_read, "ext2fs_file_read");
    RESOLVE(file_close, "ext2fs_file_close");
    RESOLVE(error_message, "error_message");
    RESOLVE(read_bitmaps, "ext2fs_read_bitmaps");
    RESOLVE(flush, "ext2fs_flush");
    RESOLVE(write_inode, "ext2fs_write_inode");
    RESOLVE(write_new_inode, "ext2fs_write_new_inode");
    RESOLVE(new_inode, "ext2fs_new_inode");
    RESOLVE(link, "ext2fs_link");
    RESOLVE(unlink, "ext2fs_unlink");
    RESOLVE(mkdir, "ext2fs_mkdir");
    RESOLVE(symlink, "ext2fs_symlink");
    RESOLVE(is_fast_symlink, "ext2fs_is_fast_symlink");
    RESOLVE(inode_alloc_stats2, "ext2fs_inode_alloc_stats2");
    RESOLVE(punch, "ext2fs_punch");
    RESOLVE(write_inode_bitmap, "ext2fs_write_inode_bitmap");
    RESOLVE(file_write, "ext2fs_file_write");
    RESOLVE(file_set_size2, "ext2fs_file_set_size2");
#undef RESOLVE

    {
	io_manager *manager = dlsym(handle, "unix_io_manager");

	if (manager == NULL) {
	    VERBOSE(NULL, 1, "netfs: unix_io_manager not found");
	    dlclose(handle);
	    ext2_api.handle = NULL;
	    return false;
	}
	ext2_api.manager = *manager;
    }

    ext2_api_ready = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* Filesystem driver                                                   */
/* ------------------------------------------------------------------ */

static void ext2_fini(NetfsMount *mount)
{
    Ext2Data *data = mount->fs_data;

    /*
     * The slot cache is a write-back one: whatever is still dirty has
     * to reach the backend before the filesystem is closed, otherwise
     * the last changes are lost.
     */
    (void) netfs_io_flush_slots(mount);
    netfs_io_drop_slots(mount);

    if (data != NULL) {
	if (data->fs != NULL)
	    VERBOSE(NULL, 3, "netfs: block I/O: %lu reads / %llu bytes, "
	    "%lu writes / %llu bytes",
	    mount->dbg_read_calls, mount->dbg_read_bytes,
	    mount->dbg_write_calls, mount->dbg_write_bytes);
    ext2_api.close(data->fs);
	TALLOC_FREE(data);
    }
    mount->fs_data = NULL;
}

static int ext2_init(NetfsMount *mount)
{
    const char *path = netfs_img_path(mount->url);
    Ext2Data *data;
    errcode_t code;

    if (!load_ext2_api())
	return -ENOSYS;

    if (path == NULL || path[0] == '\0')
	return -EINVAL;

    data = talloc_zero(mount, Ext2Data);
    if (data == NULL)
	return -ENOMEM;

    ext2_io_mount = mount;
    if (getenv("UVROOT_NETFS_FORCE_CUSTOM_IO") != NULL)
	mount->custom_io = true;
    {
	io_manager manager = mount->custom_io
	    ? (io_manager) &netfs_io_manager_decl : ext2_api.manager;

	code = ext2_api.open(path, EXT2_FLAG_64BITS | EXT2_FLAG_RW, 0, 0,
			     manager, &data->fs);
    }
    if (code == 0) {
	/* Bitmaps must be loaded for block/inode allocation to work.  */
	if (ext2_api.read_bitmaps(data->fs) != 0) {
	    VERBOSE(NULL, 3, "netfs: block I/O: %lu reads / %llu bytes, "
	    "%lu writes / %llu bytes",
	    mount->dbg_read_calls, mount->dbg_read_bytes,
	    mount->dbg_write_calls, mount->dbg_write_bytes);
    ext2_api.close(data->fs);
	    data->fs = NULL;
	    code = -1;
	}
    } else {
	/* Fall back to a read-only image.  */
	io_manager manager = mount->custom_io
	    ? (io_manager) &netfs_io_manager_decl : ext2_api.manager;

	code = ext2_api.open(path, EXT2_FLAG_64BITS, 0, 0, manager,
			     &data->fs);
	if (code == 0)
	    data->read_only = true;
    }

    if (code != 0 || data->fs == NULL) {
	VERBOSE(NULL, 1, "netfs: %s is not an ext2/3/4 image: %s", path,
		ext2_api.error_message((errcode_t) code));
	TALLOC_FREE(data);
	return -ENODEV;
    }

    mount->fs_data = data;
    return 0;
}

static void ext2_to_stat(const struct ext2_inode *inode, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = inode->i_mode;
    st->st_uid = inode->i_uid;
    st->st_gid = inode->i_gid;
    st->st_nlink = inode->i_links_count;
    st->st_size = (off_t) inode->i_size
	| ((off_t) inode->i_size_high << 32);
    st->st_mtime = inode->i_mtime;
    st->st_ctime = inode->i_ctime;
    st->st_atime = inode->i_atime;
}

/* Walk "a/b/c" from the root, returning the inode number.  */static int ext2_resolve(NetfsMount *mount, const char *rel, ext2_ino_t *ino)
{
    Ext2Data *data = mount->fs_data;
    const char *cursor = rel;
    ext2_ino_t current = EXT2_ROOT_INO;

    while (*cursor != '\0') {
	const char *slash = strchr(cursor, '/');
	size_t length = slash != NULL ? (size_t) (slash - cursor) : strlen(cursor);
	char name[NAME_MAX + 1];
	ext2_ino_t next;
	errcode_t code;

	if (length == 0) {
	    cursor++;
	    continue;
	}
	if (length > NAME_MAX)
	    return -ENAMETOOLONG;

	memcpy(name, cursor, length);
	name[length] = '\0';

	code = ext2_api.lookup(data->fs, current, name, (int) length, NULL,
			       &next);
	if (code != 0)
	    return -ENOENT;

	current = next;
	cursor = slash != NULL ? slash + 1 : cursor + length;
    }

    *ino = current;
    return 0;
}

/*
 * Read the target of the symbolic link @ino into @buffer.  A fast
 * symlink keeps it in the inode itself, a slow one in its first block.
 */
static int ext2_readlink(NetfsMount *mount, ext2_ino_t ino,
			 struct ext2_inode *inode, char *buffer, size_t size)
{
    Ext2Data *data = mount->fs_data;
    size_t length = inode->i_size;

    if (length >= size)
	return -ENAMETOOLONG;
    if (length == 0) {
	buffer[0] = '\0';
	return 0;
    }

    if (ext2_api.is_fast_symlink(inode)) {
	memcpy(buffer, &inode->i_block, length);
	buffer[length] = '\0';
	return 0;
    }

    {
	ext2_file_t file;
	unsigned int got = 0;

	if (ext2_api.file_open(data->fs, ino, 0, &file) != 0)
	    return -EIO;

	if (ext2_api.file_read(file, buffer, (unsigned int) length, &got) != 0
	    || got != length) {
	    ext2_api.file_close(file);
	    return -EIO;
	}
	ext2_api.file_close(file);
	buffer[length] = '\0';
    }

    return 0;
}

typedef struct Ext2List {
    TALLOC_CTX *context;
    NetfsDirent *entries;
    size_t count;
    size_t capacity;
    int error;
} Ext2List;

static int ext2_list_cb(struct ext2_dir_entry *dirent, int offset UNUSED,
			int blocksize UNUSED, char *buffer UNUSED,
			void *priv)
{
    Ext2List *list = priv;
    NetfsDirent *entry;
    size_t name_length;
    char name[NAME_MAX + 1];

    if (dirent->inode == 0)
	return 0;

    /*
     * In this e2fsprogs version struct ext2_dir_entry.name_len is a
     * 16-bit field whose high byte carries the file type (the historical
     * dir_entry2 format), so only the low byte is the name length.
     * Using the raw field used to append the following directory entry
     * to any name whose length is a multiple of four -- "rel_link", for
     * instance -- which then could not be resolved again.
     */
    name_length = dirent->name_len & 0xFF;
    if (name_length > NAME_MAX)
	name_length = NAME_MAX;
    memcpy(name, dirent->name, name_length);
    name[name_length] = '\0';

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
	return 0;

    if (list->count == list->capacity) {
	size_t capacity = list->capacity == 0 ? 16 : list->capacity * 2;
	NetfsDirent *grown =
	    talloc_realloc(list->context, list->entries, NetfsDirent,
			   capacity);

	if (grown == NULL) {
	    list->error = -ENOMEM;
	    return DIRENT_ABORT;
	}
	list->entries = grown;
	list->capacity = capacity;
    }

    entry = &list->entries[list->count];
    memset(entry, 0, sizeof(*entry));
    entry->name = talloc_strdup(list->context, name);
    if (entry->name == NULL) {
	list->error = -ENOMEM;
	return DIRENT_ABORT;
    }

    /* The type is taken from the inode in the second pass, which also
     * works for the old directory entry format.  */
    list->count++;
    return 0;
}

static int ext2_list(NetfsMount *mount, const char *rel,
		     NetfsDirent **entries, size_t *count, TALLOC_CTX *context)
{
    Ext2Data *data = mount->fs_data;
    Ext2List list;
    ext2_ino_t dir;
    errcode_t code;
    int status;
    size_t i;

    status = ext2_resolve(mount, rel, &dir);
    if (status < 0)
	return status;

    memset(&list, 0, sizeof(list));
    list.context = context;

    code = ext2_api.dir_iterate(data->fs, dir, 0, NULL, ext2_list_cb, &list);
    if (code != 0 && list.error == 0)
	return -EIO;
    if (list.error != 0)
	return list.error;

    /* Metadata is read here rather than in the callback, where the
     * directory buffer is being iterated.  */
    for (i = 0; i < list.count; i++) {
	char child[PATH_MAX];
	struct ext2_inode inode;
	ext2_ino_t ino;

	if (rel[0] == '\0')
	    snprintf(child, sizeof(child), "%s", list.entries[i].name);
	else
	    snprintf(child, sizeof(child), "%s/%s", rel,
		     list.entries[i].name);

	if (ext2_resolve(mount, child, &ino) < 0)
	    continue;
	if (ext2_api.read_inode(data->fs, ino, &inode) != 0)
	    continue;

	{
	    struct stat st;

	    ext2_to_stat(&inode, &st);
	    list.entries[i].mode = st.st_mode;
	    list.entries[i].size = st.st_size;
	    list.entries[i].mtime = st.st_mtime;
	    list.entries[i].is_dir = S_ISDIR(st.st_mode);
	    list.entries[i].is_link = S_ISLNK(st.st_mode);
	    list.entries[i].ino = ino;
	}

	if (list.entries[i].is_link) {
	    char target[PATH_MAX];

	    if (ext2_readlink(mount, ino, &inode, target,
			      sizeof(target)) == 0)
		list.entries[i].target = talloc_strdup(context, target);
	}
    }

    *entries = list.entries;
    *count = list.count;
    return 0;
}

static int ext2_stat(NetfsMount *mount, const char *rel, struct stat *st)
{
    Ext2Data *data = mount->fs_data;
    struct ext2_inode inode;
    ext2_ino_t ino;
    int status;

    status = ext2_resolve(mount, rel, &ino);
    if (status < 0)
	return status;

    if (ext2_api.read_inode(data->fs, ino, &inode) != 0)
	return -EIO;

    ext2_to_stat(&inode, st);
    st->st_ino = (ino_t) ino;
    return 0;
}

/*
 * Capacity of the filesystem inside the image.  The superblock keeps
 * the totals up to date (every allocation goes through
 * block_alloc_stats2/inode_alloc_stats2), so df(1) can show the real
 * size of the disk instead of the host filesystem that holds it.
 */
static int ext2_statfs(NetfsMount *mount, const char *rel UNUSED,
		       struct statfs *st)
{
    Ext2Data *data = mount->fs_data;
    struct ext2_super_block *super = data->fs->super;
    uint64_t blocks;
    uint64_t free_blocks;

    if (super == NULL)
	return -EIO;

    /* The 64-bit fields are only meaningful when the feature is on;
     * ext2fs_blocks_count() lives in libext2fs, which is dlopen()ed.  */
    blocks = super->s_blocks_count;
    free_blocks = super->s_free_blocks_count;
    if ((super->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0) {
	blocks |= (uint64_t) super->s_blocks_count_hi << 32;
	free_blocks |= (uint64_t) super->s_free_blocks_hi << 32;
    }

    memset(st, 0, sizeof(*st));
    st->f_type = EXT2_SUPER_MAGIC;
    st->f_bsize = (long) data->fs->blocksize;
    st->f_frsize = (long) data->fs->blocksize;
    st->f_blocks = (fsblkcnt_t) blocks;
    st->f_bfree = (fsblkcnt_t) free_blocks;
    st->f_bavail = st->f_bfree;
    st->f_files = (fsfilcnt_t) super->s_inodes_count;
    st->f_ffree = (fsfilcnt_t) super->s_free_inodes_count;
    st->f_namelen = EXT2_NAME_LEN;
    return 0;
}

static int ext2_get(NetfsMount *mount, const char *rel, const char *local)
{
    Ext2Data *data = mount->fs_data;
    ext2_file_t file;
    ext2_ino_t ino;
    FILE *output;
    char buffer[65536];
    int status;

    status = ext2_resolve(mount, rel, &ino);
    if (status < 0)
	return status;

    if (ext2_api.file_open(data->fs, ino, 0, &file) != 0)
	return -EIO;

    output = fopen(local, "wb");
    if (output == NULL) {
	int saved = errno;

	ext2_api.file_close(file);
	return -saved;
    }

    for (;;) {
	unsigned int got = 0;

	if (ext2_api.file_read(file, buffer, sizeof(buffer), &got) != 0) {
	    fclose(output);
	    ext2_api.file_close(file);
	    return -EIO;
	}
	if (got == 0)
	    break;
	if (fwrite(buffer, 1, got, output) != got) {
	    int saved = errno;

	    fclose(output);
	    ext2_api.file_close(file);
	    return -saved;
	}
    }

    fclose(output);
    ext2_api.file_close(file);
    return 0;
}

/* Split "a/b/c" into the parent inode and the basename.  */
static int ext2_parent(NetfsMount *mount, const char *rel, ext2_ino_t *parent,
		       char name[NAME_MAX + 1])
{
    char directory[PATH_MAX];
    const char *slash = strrchr(rel, '/');
    size_t length;

    if (slash == NULL) {
	directory[0] = '\0';
	length = strlen(rel);
	if (length == 0 || length > NAME_MAX)
	    return -ENAMETOOLONG;
	memcpy(name, rel, length + 1);
    } else {
	length = (size_t) (slash - rel);
	if (length >= sizeof(directory))
	    return -ENAMETOOLONG;
	memcpy(directory, rel, length);
	directory[length] = '\0';
	length = strlen(slash + 1);
	if (length == 0 || length > NAME_MAX)
	    return -ENAMETOOLONG;
	memcpy(name, slash + 1, length + 1);
    }

    return ext2_resolve(mount, directory, parent);
}

static void ext2_touch_parent(NetfsMount *mount, const char *rel)
{
    Ext2Data *data = mount->fs_data;
    ext2_ino_t parent;
    char name[NAME_MAX + 1];
    struct ext2_inode inode;

    if (ext2_parent(mount, rel, &parent, name) < 0)
	return;
    if (ext2_api.read_inode(data->fs, parent, &inode) != 0)
	return;

    inode.i_mtime = (__u32) time(NULL);
    inode.i_ctime = inode.i_mtime;
    (void) ext2_api.write_inode(data->fs, parent, &inode);
}

static int ext2_put(NetfsMount *mount, const char *rel, const char *local)
{
    Ext2Data *data = mount->fs_data;
    ext2_ino_t parent;
    ext2_ino_t ino = 0;
    struct ext2_inode inode;
    ext2_file_t file = NULL;
    char name[NAME_MAX + 1];
    FILE *input = NULL;
    char buffer[65536];
    struct stat local_stat;
    bool have_local_stat = false;
    mode_t mode = S_IFREG | 0644;
    off_t total = 0;
    bool created = false;
    int status = 0;

    if (data->read_only)
	return -EROFS;

    status = ext2_parent(mount, rel, &parent, name);
    if (status < 0)
	return status;

    /*
     * The local mirror carries the mode the guest asked for, either
     * through open(2)'s mode argument or through a chmod(2)/fchmod(2)
     * that was applied to the cache before the file was pushed.  A
     * block image must record it: a hard-coded 0644 would make every
     * extracted program non-executable.
     */
    if (stat(local, &local_stat) == 0) {
	have_local_stat = true;
	mode = S_IFREG | (local_stat.st_mode & 07777);
    }

    if (ext2_api.lookup(data->fs, parent, name, (int) strlen(name), NULL,
			&ino) != 0) {
	errcode_t code;

	/*
	 * A file that cannot fit would otherwise be written until the
	 * image is full, which takes a long time for a big one (and the
	 * user only learns about it at the end).  Refuse it right away.
	 */
	if (have_local_stat && S_ISREG(local_stat.st_mode)
	    && local_stat.st_size > 0) {
	    uint64_t needed = ((uint64_t) local_stat.st_size
			       + data->fs->blocksize - 1)
			      / data->fs->blocksize;
	    uint64_t free_blocks =
		(uint64_t) data->fs->super->s_free_blocks_count;

	    if (needed > free_blocks)
		return -ENOSPC;
	}

	code = ext2_api.new_inode(data->fs, parent, (int) mode, 0, &ino);

	if (code != 0)
	    return -ENOSPC;

	created = true;
	memset(&inode, 0, sizeof(inode));
	inode.i_mode = (__u16) mode;
	inode.i_uid = mount->create_identity ? (__u16) mount->create_uid : 0;
	inode.i_gid = mount->create_identity ? (__u16) mount->create_gid : 0;
	inode.i_links_count = 1;
	inode.i_atime = inode.i_ctime = inode.i_mtime = (__u32) time(NULL);

	if (ext2_api.write_new_inode(data->fs, ino, &inode) != 0) {
	    status = -EIO;
	    goto fail;
	}

	ext2_api.inode_alloc_stats2(data->fs, ino, 1, 0);

	if (ext2_api.link(data->fs, parent, name, ino,
			  EXT2_FT_REG_FILE | EXT2FS_LINK_EXPAND) != 0) {
	    status = -ENOSPC;
	    goto fail;
	}
    }

    if (ext2_api.file_open(data->fs, ino, EXT2_FILE_WRITE, &file) != 0) {
	status = -EIO;
	goto fail;
    }

    if (ext2_api.file_set_size2(file, 0) != 0) {
	status = -EIO;
	goto fail;
    }

    input = fopen(local, "rb");
    if (input == NULL) {
	status = -errno;
	goto fail;
    }

    for (;;) {
	size_t got = fread(buffer, 1, sizeof(buffer), input);
	unsigned int written = 0;

	if (got == 0)
	    break;
	if (ext2_api.file_write(file, buffer, (unsigned int) got,
				&written) != 0 || written != got) {
	    /* The image is full.  */
	    status = -ENOSPC;
	    goto fail;
	}
	total += (off_t) got;
    }

    fclose(input);
    input = NULL;

    (void) ext2_api.file_set_size2(file, (ext2_off64_t) total);
    ext2_api.file_close(file);
    file = NULL;

    if (ext2_api.read_inode(data->fs, ino, &inode) == 0) {
	inode.i_mtime = inode.i_ctime = (__u32) time(NULL);
	(void) ext2_api.write_inode(data->fs, ino, &inode);
    }

    ext2_touch_parent(mount, rel);
    (void) ext2_api.flush(data->fs);
    return 0;

  fail:
    if (input != NULL)
	fclose(input);
    if (file != NULL)
	ext2_api.file_close(file);

    /*
     * A file that could not be written must not stay behind: an entry
     * pointing at an inode whose allocation was never flushed is
     * exactly what makes e2fsck report corruption.  Roll it back and
     * flush, so the on-disk state stays consistent (and the caller
     * learns about the failure instead of believing the write worked).
     */
    /* Give back whatever blocks the partial write managed to allocate,
     * otherwise a full image stays permanently full.  The extent tree
     * was changed by the writes, so reload the inode first.  */
    if (ext2_api.read_inode(data->fs, ino, &inode) == 0
	&& ext2_api.punch != NULL)
	(void) ext2_api.punch(data->fs, ino, &inode, NULL, 0, ~(blk64_t) 0);

    if (created) {
	if (ext2_api.read_inode(data->fs, ino, &inode) == 0) {
	    inode.i_size = 0;
	    inode.i_blocks = 0;
	    inode.i_links_count = 0;
	    inode.i_dtime = (__u32) time(NULL);
	    (void) ext2_api.write_inode(data->fs, ino, &inode);
	}
	(void) ext2_api.unlink(data->fs, parent, name, ino, 0);
	ext2_api.inode_alloc_stats2(data->fs, ino, -1, 0);
    } else if (ext2_api.read_inode(data->fs, ino, &inode) == 0) {
	/* An existing file keeps its entry, now empty.  */
	inode.i_size = 0;
	inode.i_blocks = 0;
	(void) ext2_api.write_inode(data->fs, ino, &inode);
    }

    ext2_touch_parent(mount, rel);
    (void) ext2_api.flush(data->fs);
    return status;
}

static int ext2_mkdir(NetfsMount *mount, const char *rel)
{
    Ext2Data *data = mount->fs_data;
    ext2_ino_t parent;
    ext2_ino_t ino;
    struct ext2_inode inode;
    char name[NAME_MAX + 1];
    int status;

    if (data->read_only)
	return -EROFS;

    status = ext2_parent(mount, rel, &parent, name);
    if (status < 0)
	return status;

    if (ext2_api.mkdir(data->fs, parent, 0, name) != 0)
	return -ENOSPC;

    if (ext2_api.lookup(data->fs, parent, name, (int) strlen(name), NULL,
			&ino) != 0) {
	(void) ext2_api.flush(data->fs);
	return -EIO;
    }

    if (ext2_api.read_inode(data->fs, ino, &inode) == 0) {
	inode.i_mode = S_IFDIR | 0755;
	inode.i_uid = mount->create_identity ? (__u16) mount->create_uid : 0;
	inode.i_gid = mount->create_identity ? (__u16) mount->create_gid : 0;
	(void) ext2_api.write_inode(data->fs, ino, &inode);
    }

    ext2_touch_parent(mount, rel);
    (void) ext2_api.flush(data->fs);
    return 0;
}

static int ext2_unlink(NetfsMount *mount, const char *rel)
{
    Ext2Data *data = mount->fs_data;
    ext2_ino_t parent;
    ext2_ino_t ino;
    struct ext2_inode inode;
    char name[NAME_MAX + 1];
    int status;

    if (data->read_only)
	return -EROFS;

    status = ext2_parent(mount, rel, &parent, name);
    if (status < 0)
	return status;

    if (ext2_api.lookup(data->fs, parent, name, (int) strlen(name), NULL,
			&ino) != 0)
	return -ENOENT;

    if (ext2_api.read_inode(data->fs, ino, &inode) != 0)
	return -EIO;

    if (S_ISDIR(inode.i_mode))
	return -EISDIR;

    if (ext2_api.unlink(data->fs, parent, name, ino, 0) != 0)
	return -EIO;

    /* Release the data blocks, then the inode itself.  */
    {
	ext2_file_t file;

	if (ext2_api.file_open(data->fs, ino, EXT2_FILE_WRITE, &file) == 0) {
	    (void) ext2_api.file_set_size2(file, 0);
	    ext2_api.file_close(file);
	}
    }

    if (inode.i_links_count > 0)
	inode.i_links_count--;
    inode.i_dtime = (__u32) time(NULL);
    (void) ext2_api.write_inode(data->fs, ino, &inode);

    /* -1 releases the inode: it clears the bitmap *and* updates the
     * group/superblock free-inode counters.  0 would only clear the
     * bitmap and leave every counter stale, which e2fsck reports as an
     * invalid free-inode count.  */
    ext2_api.inode_alloc_stats2(data->fs, ino, -1, 0);
    if (ext2_api.write_inode_bitmap != NULL)
	(void) ext2_api.write_inode_bitmap(data->fs);

    ext2_touch_parent(mount, rel);
    (void) ext2_api.flush(data->fs);
    return 0;
}

static int ext2_rmdir(NetfsMount *mount, const char *rel)
{
    Ext2Data *data = mount->fs_data;
    ext2_ino_t parent;
    ext2_ino_t ino;
    struct ext2_inode inode;
    char name[NAME_MAX + 1];
    int status;

    if (data->read_only)
	return -EROFS;

    status = ext2_parent(mount, rel, &parent, name);
    if (status < 0)
	return status;

    if (ext2_api.lookup(data->fs, parent, name, (int) strlen(name), NULL,
			&ino) != 0)
	return -ENOENT;

    if (ext2_api.read_inode(data->fs, ino, &inode) != 0)
	return -EIO;
    if (!S_ISDIR(inode.i_mode))
	return -ENOTDIR;

    /* Release the directory's data blocks (extent aware).  */
    if (ext2_api.punch != NULL)
	(void) ext2_api.punch(data->fs, ino, &inode, NULL, 0, ~(blk64_t) 0);

    if (ext2_api.unlink(data->fs, parent, name, ino, 0) != 0)
	return -EIO;

    /* Removing a directory also removes its ".." entry, so its parent
     * loses one link; ext2fs_unlink() does not do that by itself.  */
    {
	struct ext2_inode parent_inode;

	if (ext2_api.read_inode(data->fs, parent, &parent_inode) == 0) {
	    if (parent_inode.i_links_count > 0)
		parent_inode.i_links_count--;
	    (void) ext2_api.write_inode(data->fs, parent, &parent_inode);
	}
    }

    /*
     * A released directory must end up with no link at all ("debugfs -w
     * -R rmdir" leaves Links: 0) and a deletion time, and its inode has
     * to be released in the bitmap *and* in the group counters: that is
     * what inode_alloc_stats2(fs, ino, -1, 1) does.  Passing 0 there --
     * as this used to -- clears the bitmap but leaves every counter
     * stale, which e2fsck reports as corruption.
     */
    inode.i_size = 0;
    inode.i_blocks = 0;
    inode.i_links_count = 0;
    inode.i_dtime = (__u32) time(NULL);
    (void) ext2_api.write_inode(data->fs, ino, &inode);
    ext2_api.inode_alloc_stats2(data->fs, ino, -1, 1);

    ext2_touch_parent(mount, rel);
    (void) ext2_api.flush(data->fs);
    return 0;
}

static int ext2_rename(NetfsMount *mount, const char *from, const char *to)
{
    Ext2Data *data = mount->fs_data;
    ext2_ino_t old_parent;
    ext2_ino_t new_parent;
    ext2_ino_t ino;
    struct ext2_inode inode;
    char old_name[NAME_MAX + 1];
    char new_name[NAME_MAX + 1];
    int status;

    if (data->read_only)
	return -EROFS;

    status = ext2_parent(mount, from, &old_parent, old_name);
    if (status < 0)
	return status;
    status = ext2_parent(mount, to, &new_parent, new_name);
    if (status < 0)
	return status;

    if (ext2_api.lookup(data->fs, old_parent, old_name,
			(int) strlen(old_name), NULL, &ino) != 0)
	return -ENOENT;
    if (ext2_api.read_inode(data->fs, ino, &inode) != 0)
	return -EIO;

    if (S_ISDIR(inode.i_mode)) {
	/* Moving a directory also repoints its ".." entry.  */
	if (ext2_api.link(data->fs, new_parent, new_name, ino,
			  EXT2_FT_DIR | EXT2FS_LINK_EXPAND) != 0)
	    return -ENOSPC;

	if (ext2_api.unlink(data->fs, old_parent, old_name, ino, 0) != 0)
	    return -EIO;

	if (old_parent != new_parent) {
	    (void) ext2_api.unlink(data->fs, ino, "..", old_parent, 0);
	    if (ext2_api.link(data->fs, ino, "..", new_parent,
			      EXT2_FT_DIR) != 0)
		return -EIO;
	}
    } else {
	if (ext2_api.link(data->fs, new_parent, new_name, ino,
			  (S_ISLNK(inode.i_mode) ? EXT2_FT_SYMLINK
						 : EXT2_FT_REG_FILE)
			  | EXT2FS_LINK_EXPAND) != 0)
	    return -ENOSPC;

	if (ext2_api.unlink(data->fs, old_parent, old_name, ino, 0) != 0)
	    return -EIO;
    }

    ext2_touch_parent(mount, from);
    ext2_touch_parent(mount, to);
    (void) ext2_api.flush(data->fs);
    return 0;
}

static int ext2_symlink(NetfsMount *mount, const char *rel, const char *target)
{
    Ext2Data *data = mount->fs_data;
    ext2_ino_t parent;
    ext2_ino_t ino;
    struct ext2_inode inode;
    char name[NAME_MAX + 1];
    int status;

    if (data->read_only)
	return -EROFS;
    if (target == NULL || target[0] == '\0')
	return -EINVAL;

    status = ext2_parent(mount, rel, &parent, name);
    if (status < 0)
	return status;

    if (ext2_api.symlink(data->fs, parent, 0, name, target) != 0)
	return -ENOSPC;

    /*
     * ext2fs_symlink() gives the new inode ownership 0/0; the container
     * identity that created it goes in instead, like for files and
     * directories.
     */
    if (ext2_api.lookup(data->fs, parent, name, (int) strlen(name), NULL,
			&ino) == 0
	&& ext2_api.read_inode(data->fs, ino, &inode) == 0) {
	inode.i_uid = mount->create_identity ? (__u16) mount->create_uid : 0;
	inode.i_gid = mount->create_identity ? (__u16) mount->create_gid : 0;
	(void) ext2_api.write_inode(data->fs, ino, &inode);
    }

    ext2_touch_parent(mount, rel);
    (void) ext2_api.flush(data->fs);
    return 0;
}

static int ext2_link(NetfsMount *mount, const char *from, const char *to)
{
    Ext2Data *data = mount->fs_data;
    ext2_ino_t parent;
    ext2_ino_t ino;
    struct ext2_inode inode;
    char name[NAME_MAX + 1];
    int status;

    if (data->read_only)
	return -EROFS;

    status = ext2_parent(mount, to, &parent, name);
    if (status < 0)
	return status;

    if (ext2_resolve(mount, from, &ino) < 0)
	return -ENOENT;
    if (ext2_api.read_inode(data->fs, ino, &inode) != 0)
	return -EIO;
    if (S_ISDIR(inode.i_mode))
	return -EPERM;

    if (ext2_api.link(data->fs, parent, name, ino,
		      (S_ISLNK(inode.i_mode) ? EXT2_FT_SYMLINK
					     : EXT2_FT_REG_FILE)
		      | EXT2FS_LINK_EXPAND) != 0)
	return -ENOSPC;

    inode.i_links_count++;
    inode.i_ctime = (__u32) time(NULL);
    (void) ext2_api.write_inode(data->fs, ino, &inode);

    ext2_touch_parent(mount, to);
    (void) ext2_api.flush(data->fs);
    return 0;
}

static int ext2_chmod(NetfsMount *mount, const char *rel, mode_t mode)
{
    Ext2Data *data = mount->fs_data;
    struct ext2_inode inode;
    ext2_ino_t ino;

    if (data->read_only)
	return -EROFS;
    if (ext2_resolve(mount, rel, &ino) < 0)
	return -ENOENT;
    if (ext2_api.read_inode(data->fs, ino, &inode) != 0)
	return -EIO;

    inode.i_mode = (inode.i_mode & S_IFMT) | (mode & 07777);
    inode.i_ctime = (__u32) time(NULL);
    if (ext2_api.write_inode(data->fs, ino, &inode) != 0)
	return -EIO;

    (void) ext2_api.flush(data->fs);
    return 0;
}

static int ext2_chown(NetfsMount *mount, const char *rel, uid_t uid, gid_t gid)
{
    Ext2Data *data = mount->fs_data;
    struct ext2_inode inode;
    ext2_ino_t ino;

    if (data->read_only)
	return -EROFS;
    if (ext2_resolve(mount, rel, &ino) < 0)
	return -ENOENT;
    if (ext2_api.read_inode(data->fs, ino, &inode) != 0)
	return -EIO;

    if ((uid_t) -1 != uid)
	inode.i_uid = (__u16) uid;
    if ((gid_t) -1 != gid)
	inode.i_gid = (__u16) gid;
    inode.i_ctime = (__u32) time(NULL);
    if (ext2_api.write_inode(data->fs, ino, &inode) != 0)
	return -EIO;

    (void) ext2_api.flush(data->fs);
    return 0;
}

const NetfsFsDriver netfs_fs_driver_ext2 = {
    .name = "ext2",
    .implemented = true,
    .open = ext2_init,
    .close = ext2_fini,
    .dir = {
	    .list = ext2_list,
	    .stat = ext2_stat,
	    .get = ext2_get,
	    .put = ext2_put,
	    .mkdir = ext2_mkdir,
	    .rmdir = ext2_rmdir,
	    .unlink = ext2_unlink,
	    .rename = ext2_rename,
	    .symlink = ext2_symlink,
	    .link = ext2_link,
	    .chmod = ext2_chmod,
	    .chown = ext2_chown,
	    .statfs = ext2_statfs,
	    },
};

#else				/* !HAVE_LIBEXT2FS */

const NetfsFsDriver netfs_fs_driver_ext2 = {
    .name = "ext2",
    .implemented = false,
    .reserved_note = "this build was made without the libext2fs headers",
};

#endif				/* HAVE_LIBEXT2FS */
