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

#ifndef NETFS_H
#define NETFS_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <talloc.h>

#include "extension/extension.h"
#include "tracee/tracee.h"

/*
 * The netfs extension turns a remote or image-backed resource into a
 * directory tree that guest programs can read and write as if it were
 * a local folder.  It is built around two independent plug-in layers:
 *
 *   NetfsBackend   transports: ftp(s)/sftp, smb, nfs (directory
 *                  transports) and img/raw, qcow2, nbd, iscsi (block
 *                  transports).
 *
 *   NetfsFsDriver  filesystem drivers that can be stacked on top of a
 *                  block backend so that it, too, exposes a directory
 *                  tree (ext2/3/4 implemented; fat, ntfs reserved).
 *
 * A backend is either:
 *
 *   NETFS_KIND_DIR    it already exposes a directory tree (ftp, smb,
 *                     nfs), so it implements NetfsDirOps directly;
 *
 *   NETFS_KIND_BLOCK  it exposes a byte-addressable device (iscsi,
 *                     nbd, img, qcow2); a NetfsFsDriver is then needed
 *                     to interpret the filesystem stored on it.
 *
 * Everything above the backend layer (caching, lazy materialization,
 * write-back, bind-mount glue) is backend agnostic.
 */

/* One entry of a remote directory listing.  */
typedef struct NetfsDirent {
    char *name;			/* basename, no slash */
    char *target;		/* symlink target, when known */
    bool is_dir;
    bool is_link;
    off_t size;
    time_t mtime;
    mode_t mode;
    uint64_t ino;		/* filesystem inode, 0 when unknown */
} NetfsDirent;

struct NetfsMount;
typedef struct NetfsMount NetfsMount;

/* Directory-level operations.  All paths are relative to the mount
 * root, use '/' as separator and have no leading nor trailing slash
 * ("" denotes the root itself).  All functions return 0 on success or
 * -errno on error.  */
typedef struct NetfsDirOps {
    int (*list)(NetfsMount * mount, const char *rel,
		NetfsDirent ** entries, size_t * count, TALLOC_CTX * ctx);
    int (*stat)(NetfsMount * mount, const char *rel, struct stat *st);
    int (*get)(NetfsMount * mount, const char *rel, const char *local);
    int (*put)(NetfsMount * mount, const char *rel, const char *local);
    int (*mkdir)(NetfsMount * mount, const char *rel);
    int (*rmdir)(NetfsMount * mount, const char *rel);
    int (*unlink)(NetfsMount * mount, const char *rel);
    int (*rename)(NetfsMount * mount, const char *from, const char *to);
    /* Symlinks and hard links, only meaningful for filesystems that can
     * represent them (block backends); NULL elsewhere.  */
    int (*symlink)(NetfsMount * mount, const char *rel, const char *target);
    int (*link)(NetfsMount * mount, const char *from, const char *to);
    /* Metadata changes, applied inside the filesystem itself.  */
    int (*chmod)(NetfsMount * mount, const char *rel, mode_t mode);
    int (*chown)(NetfsMount * mount, const char *rel, uid_t uid, gid_t gid);
    /* Capacity of the filesystem behind the mount, for statfs(2).  NULL
     * when the backend cannot tell; netfs then reports a synthetic
     * non-zero capacity so df(1) still shows the mount.  */
    int (*statfs)(NetfsMount * mount, const char *rel, struct statfs *st);
} NetfsDirOps;

/* Block-level operations.  Used by image/network block transports and
 * consumed by a NetfsFsDriver.  */
typedef struct NetfsBlockOps {
    int (*open)(NetfsMount * mount);
    void (*close)(NetfsMount * mount);
    int (*size)(NetfsMount * mount, uint64_t * bytes);
    ssize_t(*pread)(NetfsMount * mount, void *buf, size_t count,
		    uint64_t offset);
    ssize_t(*pwrite)(NetfsMount * mount, const void *buf, size_t count,
		     uint64_t offset);
    int (*flush)(NetfsMount * mount);
} NetfsBlockOps;

typedef enum {
    NETFS_KIND_DIR = 0,
    NETFS_KIND_BLOCK,
} NetfsKind;

typedef struct NetfsBackend {
    const char *name;
    const char *const *schemes;	/* NULL terminated */
    NetfsKind kind;

    /* false when the backend is only reserved for future work.  */
    bool implemented;
    const char *reserved_note;

    NetfsDirOps dir;		/* meaningful when kind == NETFS_KIND_DIR */
    NetfsBlockOps block;	/* meaningful when kind == NETFS_KIND_BLOCK */

    /* Optional constructor/destructor.  The URL is passed verbatim,
     * credentials included.  */
    int (*init)(NetfsMount * mount, const char *url);
    void (*fini)(NetfsMount * mount);
} NetfsBackend;

/*
 * A filesystem driver interprets the content of a block backend so it
 * can be presented as a directory tree.  Reserved for iscsi/nbd/img/
 * qcow2 support.
 */
typedef struct NetfsFsDriver {
    const char *name;
    bool implemented;
    const char *reserved_note;

    /* Probe/prepare the backend for this filesystem; returns 0 on
     * success.  @close releases it.  */
    int (*open)(NetfsMount * mount);
    void (*close)(NetfsMount * mount);

    NetfsDirOps dir;
} NetfsFsDriver;

/* A node of the local mirror of the remote tree.  */
typedef struct NetfsNode {
    struct NetfsNode *parent;
    struct NetfsNode *child;	/* first child */
    struct NetfsNode *sibling;	/* next sibling */
    struct NetfsMount *mount;	/* NULL for the mount root when detached */
    char *name;

    char *target;		/* symlink target, when known */
    bool is_dir;
    bool is_link;
    bool materialized;		/* present in the local cache */
    bool listed;		/* directory listing already fetched */
    bool seen;			/* present in the last listing */
    bool fetched;		/* file content is up to date locally */
    bool dirty;			/* local content must be pushed back */
    bool deleted;		/* unlinked remotely, kept alive for open fds */

    off_t size;
    time_t mtime;
    mode_t mode;
    uint64_t ino;		/* inode inside the remote filesystem */
    time_t listed_at;
} NetfsNode;

/* An open file descriptor bound to a cached file.  */
typedef struct NetfsFd {
    struct NetfsFd *next;
    int fd;
    NetfsNode *node;
    bool dirty;
} NetfsFd;

/* Per-tracee state.  */
typedef struct NetfsTracee {
    struct NetfsTracee *next;
    pid_t pid;
    NetfsFd *fds;

    /* Paths captured at syscall enter for use at syscall exit.  */
    char pend_a[PATH_MAX];
    char pend_b[PATH_MAX];
    NetfsNode *pend_node;
    NetfsNode *pend_node2;
    int pend_fd;		/* descriptor captured for fstatfs */
    bool pend_valid;
} NetfsTracee;

struct NetfsMount {
    NetfsMount *next;

    char *url;			/* full URL, credentials included */
    char *display;		/* URL without credentials, for logs */
    char *scheme;
    char *guest;		/* guest mount point, absolute */
    char *cache;		/* host cache directory */
    bool read_only;

    const NetfsBackend *backend;
    void *backend_data;

    /* Optional filesystem driver stacked on a NETFS_KIND_BLOCK
     * backend.  Reserved.  */
    const NetfsFsDriver *fs;
    void *fs_data;

    /* Ownership given to entries the container creates; written into
     * the filesystem inside the disk.  */
    uid_t create_uid;
    gid_t create_gid;
    bool create_identity;

    /* True when the block layer is not a plain local file, so the
     * generic NetfsBlockOps io_manager must be used.  */
    bool custom_io;

    /*
     * Coalescing window used by the generic io_manager.  libext2fs
     * asks for one block at a time; a network backend would pay a full
     * round trip for each 4 KiB, which makes iSCSI and NBD about two
     * orders of magnitude slower than they should be.  Requests are
     * served from this window instead, so the backend only sees big
     * transfers.
     */
    struct {
	uint64_t base;		/* aligned offset, ~0 when free */
	size_t length;
	bool valid;
	bool dirty;
	unsigned long stamp;	/* for the LRU order */
	char *data;
    } io_slot[8];
    unsigned long io_stamp;

    unsigned long dbg_read_calls, dbg_write_calls;
    unsigned long long dbg_read_bytes, dbg_write_bytes;

    /* When another process owns the image, all directory operations are
     * forwarded to it through this table.  */
    void *share_data;
    const struct NetfsDirOps *share_ops;
    void *share_server;		/* owner side */

    /* Filesystem driver and its serialization.  The driver behind a
     * block image is not reentrant, and several containers of this
     * process may use it at once, so every call goes through @ops which
     * takes @lock.  When the image is already open elsewhere in this
     * process, @redirect points at the mount that owns it and @ops
     * forwards there instead.  */
    const struct NetfsDirOps *base_ops;
    const struct NetfsDirOps *ops;
    struct NetfsMount *redirect;
    pthread_mutex_t lock;
    bool lock_ready;
    bool registered;

    /* Backing storage of the image, and the file that publishes this
     * mount's mirror so that another process can tell an image living
     * inside it.  */
    dev_t image_dev;
    ino_t image_ino;
    char *image_path;
    struct tracee *tracee;	/* owner of this mount */
    char *cache_marker;

    NetfsNode root;
};

/* netfs.c */
extern int netfs_callback(Extension * extension, ExtensionEvent event,
			  intptr_t d1, intptr_t d2);
extern int netfs_add_mount(Tracee * tracee, const char *spec,
			   const char *forced_scheme);
/*
 * Return the guest mount point whose local cache is @host, or NULL when
 * @host does not belong to a netfs mount.  Used by other extensions
 * (vperm) to tell a remote root apart from a regular local one.
 */
extern const char *netfs_mount_guest_for_host(Tracee * tracee,
					      const char *host);
/*
 * Enumerate the netfs mounts of @tracee.  Returns false when @index is
 * past the last mount, otherwise fills @guest and @cache (both optional).
 */
extern bool netfs_mount_at(Tracee * tracee, unsigned int index,
			   const char **guest, const char **cache);
/*
 * True when @host is the cache of a mount backed by a block device.  Such
 * a root carries its own filesystem metadata, so the vperm database must
 * keep out of the way.
 */
extern bool netfs_is_block_root(Tracee * tracee, const char *host);
/*
 * Apply a metadata change inside the filesystem of a block-backed mount
 * whose cache is @host.  Returns a negative errno, or -ENODEV when @host
 * does not belong to a block mount.
 */
extern int netfs_block_chmod(Tracee * tracee, const char *host, mode_t mode);
extern int netfs_block_chown(Tracee * tracee, const char *host, uid_t uid,
			     gid_t gid);
/* Metadata as stored by the filesystem inside the disk.  */
extern int netfs_block_stat(Tracee * tracee, const char *host,
			    struct stat *st);
/* Ownership to give to entries the container is about to create.  */
extern void netfs_block_set_identity(Tracee * tracee, uid_t uid, gid_t gid);

/*
 * Take an advisory lock on a local image so that two containers cannot
 * write to the same file at once (their in-memory block/inode bitmaps
 * would otherwise be written back on top of each other and corrupt the
 * filesystem).  Writable images get an exclusive lock, read-only ones a
 * shared one.  Returns 0 when the lock was taken, -EBUSY when another
 * process already holds it and -errno on a real error.
 */
extern int netfs_lock_image(int fd, bool exclusive, const char *path);

/*
 * The directory operations of @mount: the shared service when the image
 * is owned by another process, the local filesystem driver otherwise.
 */
extern const NetfsDirOps *netfs_dir_ops_of(NetfsMount * mount);

/* Called once all the command line options are known: decides whether
 * this process owns the image or is a client of its owner.  */
extern void netfs_finalize(Tracee * tracee);

/* Refuse every change on the netfs mounts, the virtual root included.  */
extern int netfs_set_read_only(Tracee * tracee);

/*
 * Set by the signal handler when the user interrupts uvroot (^C).  A long
 * transfer checks it and aborts with EINTR, otherwise the signal would
 * only be forwarded once the syscall returns -- which can take minutes
 * for a big file.
 */
extern volatile sig_atomic_t netfs_interrupt;
extern void netfs_clear_interrupt(void);

/*
 * True when the guest root is a block image and it has no @rel entry.
 * Used to inject the host's resolver configuration when the image
 * carries none (uvroot shares the host network stack, but not its
 * /etc/resolv.conf).
 */
extern bool netfs_block_root_lacks(Tracee * tracee, const char *rel);

/*
 * Open @mount's image in this process: used when the process that owned
 * it went away and a user of the image has to take over.
 */
extern int netfs_open_image(struct tracee *tracee, NetfsMount * mount);

/* backend registry (netfs.c) */
extern const NetfsBackend *netfs_find_backend(const char *scheme);
extern const NetfsFsDriver *netfs_find_fs_driver(const char *name);
extern const NetfsBackend *const netfs_backends[];
extern const NetfsFsDriver *const netfs_fs_drivers[];

/* backend_img.c */
extern const NetfsBackend netfs_backend_img;
/*
 * Strip the "img://", "raw://" or "file://" prefix from @url and return
 * the underlying image path.
 */
extern const char *netfs_img_path(const char *url);

/* fs_ext2.c */
extern const NetfsFsDriver netfs_fs_driver_ext2;

/* backend_curl.c */
extern const NetfsBackend netfs_backend_ftp;

/* backend_smb.c */
extern const NetfsBackend netfs_backend_smb;

/* backend_reserved.c */
extern const NetfsBackend netfs_backend_nfs;
extern const NetfsBackend netfs_backend_iscsi;
extern const NetfsBackend netfs_backend_nbd;
extern const NetfsBackend netfs_backend_img;
extern const NetfsBackend netfs_backend_qcow2;

#endif				/* NETFS_H */
