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
 * netfs: user-space virtual mounts for uvroot.
 *
 * A remote (or image backed) resource is mirrored into a private local
 * cache directory which is then bound into the guest.  Whenever uvroot
 * canonicalizes a path that falls inside one of these caches, the
 * corresponding remote entry is materialized on demand (directories are
 * created and listed, files become sparse placeholders carrying the
 * remote size and timestamp).  File content is fetched lazily when a
 * file is opened for reading and pushed back when the last descriptor
 * referring to it is closed.  Namespace operations (mkdir, rmdir,
 * unlink, rename) are mirrored to the remote side as well.
 */

#include <dirent.h>	/* opendir(3), */
#include <linux/fs.h>	/* FS_IOC_FIEMAP, */
#include <linux/fiemap.h>	/* struct fiemap, */
#include <sys/ioctl.h>	/* ioctl(2), */
#include <sys/queue.h>	/* CIRCLEQ_*, */
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <talloc.h>

#include "build.h"
#include "cli/note.h"
#include "extension/extension.h"
#include "extension/netfs/netfs.h"
#include "extension/netfs/share.h"
#include "extension/vperm/vperm.h"
#include "path/binding.h"
#include "path/path.h"
#include "syscall/seccomp.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "tracee/mem.h"
#include "tracee/abi.h"
#include "tracee/reg.h"
#include "arch.h"

/* How long a directory listing is considered fresh, in seconds.  */
#define NETFS_DIR_TTL		2

typedef struct NetfsConfig {
    NetfsMount *mounts;
    NetfsTracee *tracees;
    char *tmp_root;
    bool tmp_root_is_temporary;
    unsigned int next_mount_id;

    /* --read-only: every mount of this container refuses any change,
     * whatever the virtual identity -- id0 included.  */
    bool read_only;
} NetfsConfig;

/* ------------------------------------------------------------------ */
/* Backend registry                                                    */
/* ------------------------------------------------------------------ */

const NetfsBackend *const netfs_backends[] = {
    &netfs_backend_ftp,
    &netfs_backend_smb,
    &netfs_backend_nfs,
    &netfs_backend_img,
    &netfs_backend_qcow2,
    &netfs_backend_nbd,
    &netfs_backend_iscsi,

    NULL,
};

const NetfsBackend *netfs_find_backend(const char *scheme)
{
    size_t i;

    if (scheme == NULL)
	return NULL;

    for (i = 0; netfs_backends[i] != NULL; i++) {
	const NetfsBackend *backend = netfs_backends[i];
	size_t j;

	for (j = 0; backend->schemes != NULL && backend->schemes[j] != NULL;
	     j++) {
	    if (strcasecmp(backend->schemes[j], scheme) == 0)
		return backend;
	}
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static NetfsConfig *config_of(Extension *extension)
{
    return talloc_get_type_abort(extension->config, NetfsConfig);
}

/*
 * Build the path of @node relative to its mount root.  The walk is
 * iterative so that deeply nested trees cannot exhaust the stack.
 */
static void node_relpath(const NetfsNode *node, char *buffer, size_t size)
{
    const NetfsNode *stack[PATH_MAX / 2 + 2];
    size_t depth = 0;
    size_t used = 0;
    size_t i;

    if (size == 0)
	return;
    buffer[0] = '\0';

    while (node != NULL && node->parent != NULL
	   && depth < sizeof(stack) / sizeof(stack[0])) {
	stack[depth++] = node;
	node = node->parent;
    }

    for (i = depth; i > 0; i--) {
	const char *name = stack[i - 1]->name;
	size_t length = strlen(name);

	if (used > 0) {
	    if (used + 1 >= size)
		break;
	    buffer[used++] = '/';
	}
	if (used + length >= size)
	    length = size - used - 1;
	memcpy(buffer + used, name, length);
	used += length;
	buffer[used] = '\0';
    }
}

static void node_local_path(const NetfsNode *node, char *buffer, size_t size)
{
    char rel[PATH_MAX];
    size_t prefix;

    if (node->parent == NULL) {
	snprintf(buffer, size, "%s", node->mount->cache);
	return;
    }

    node_relpath(node, rel, sizeof(rel));

    prefix = strlen(node->mount->cache);
    if (prefix + 1 >= size) {
	snprintf(buffer, size, "%s", node->mount->cache);
	return;
    }

    {
	size_t room = size - prefix - 1;
	size_t length = strlen(rel);

	if (length > room)
	    length = room;

	memcpy(buffer, node->mount->cache, prefix);
	buffer[prefix] = '/';
	memcpy(buffer + prefix + 1, rel, length);
	buffer[prefix + 1 + length] = '\0';
    }
}

static NetfsNode *find_child(NetfsNode *dir, const char *name)
{
    NetfsNode *child;

    for (child = dir->child; child != NULL; child = child->sibling) {
	if (strcmp(child->name, name) == 0)
	    return child;
    }

    return NULL;
}

static NetfsNode *new_child_node(NetfsNode *dir, const char *name)
{
    NetfsNode *node;

    /*
     * Nodes are allocated on the mount, never on a parent node: the
     * root node is embedded in NetfsMount and therefore is not a valid
     * talloc context.
     */
    node = talloc_zero(dir->mount, NetfsNode);
    if (node == NULL)
	return NULL;

    node->parent = dir;
    node->mount = dir->mount;
    node->name = talloc_strdup(node, name);
    node->sibling = dir->child;
    dir->child = node;

    return node;
}

static void forget_child(NetfsNode *node)
{
    NetfsNode **link;

    if (node->parent == NULL)
	return;

    for (link = &node->parent->child; *link != NULL;
	 link = &(*link)->sibling) {
	if (*link == node) {
	    *link = node->sibling;
	    node->sibling = NULL;
	    node->parent = NULL;
	    return;
	}
    }
}

static const NetfsDirOps *dir_ops_of(NetfsMount *mount)
{
    if (mount->ops != NULL)
	return mount->ops;

    if (mount->share_ops != NULL)
	return mount->share_ops;

    if (mount->backend == NULL)
	return NULL;

    if (mount->backend->kind == NETFS_KIND_DIR)
	return &mount->backend->dir;

    if (mount->fs != NULL)
	return &mount->fs->dir;

    return NULL;
}

const NetfsDirOps *netfs_dir_ops_of(NetfsMount *mount)
{
    return dir_ops_of(mount);
}


/* ------------------------------------------------------------------ */
/* Serialization of a filesystem driver shared by several containers    */
/* ------------------------------------------------------------------ */

/*
 * A block image is driven by a single, non-reentrant user-space
 * filesystem.  When several containers of this process use the same
 * image they share that driver, so every operation is taken under the
 * mount lock.  A container whose image is already open elsewhere in the
 * process redirects to the owner's mount (and thus to the same lock and
 * the same mirror), which is how data written by one container is
 * immediately visible to the others.
 */

#define LOCKED_LIST(NAME)						\
static int NAME(NetfsMount *mount, const char *rel,			\
		NetfsDirent **entries, size_t *count, TALLOC_CTX *ctx)	\
{									\
	int status;							\
	pthread_mutex_lock(&mount->lock);				\
	status = mount->base_ops->list(mount, rel, entries, count, ctx);\
	pthread_mutex_unlock(&mount->lock);				\
	return status;							\
}

#define LOCKED_ONE(FUNC, FIELD, TYPE)					\
static int FUNC(NetfsMount *mount, const char *rel, TYPE arg)		\
{									\
	int status;							\
	pthread_mutex_lock(&mount->lock);				\
	status = mount->base_ops->FIELD(mount, rel, arg);		\
	pthread_mutex_unlock(&mount->lock);				\
	return status;							\
}

#define LOCKED_SIMPLE(FUNC, FIELD)					\
static int FUNC(NetfsMount *mount, const char *rel)			\
{									\
	int status;							\
	pthread_mutex_lock(&mount->lock);				\
	status = mount->base_ops->FIELD(mount, rel);			\
	pthread_mutex_unlock(&mount->lock);				\
	return status;							\
}

#define LOCKED_TWO(FUNC, FIELD)						\
static int FUNC(NetfsMount *mount, const char *from, const char *to)	\
{									\
	int status;							\
	pthread_mutex_lock(&mount->lock);				\
	status = mount->base_ops->FIELD(mount, from, to);		\
	pthread_mutex_unlock(&mount->lock);				\
	return status;							\
}

LOCKED_LIST(locked_list)
LOCKED_ONE(locked_stat, stat, struct stat *)
LOCKED_ONE(locked_get, get, const char *)
LOCKED_ONE(locked_put, put, const char *)
LOCKED_SIMPLE(locked_mkdir, mkdir)
LOCKED_SIMPLE(locked_rmdir, rmdir)
LOCKED_SIMPLE(locked_unlink, unlink)
LOCKED_TWO(locked_rename, rename)
LOCKED_TWO(locked_symlink, symlink)
LOCKED_TWO(locked_link, link)
LOCKED_ONE(locked_chmod, chmod, mode_t)

static int locked_chown_full(NetfsMount *mount, const char *rel, uid_t uid,
			     gid_t gid)
{
    int status;

    pthread_mutex_lock(&mount->lock);
    status = mount->base_ops->chown(mount, rel, uid, gid);
    pthread_mutex_unlock(&mount->lock);
    return status;
}

static int locked_statfs(NetfsMount *mount, const char *rel, struct statfs *st)
{
    int status;

    if (mount->base_ops->statfs == NULL)
	return -ENOSYS;

    pthread_mutex_lock(&mount->lock);
    status = mount->base_ops->statfs(mount, rel, st);
    pthread_mutex_unlock(&mount->lock);
    return status;
}

#undef LOCKED_LIST
#undef LOCKED_ONE
#undef LOCKED_SIMPLE
#undef LOCKED_TWO

static const NetfsDirOps locking_ops = {
    .list = locked_list,
    .stat = locked_stat,
    .get = locked_get,
    .put = locked_put,
    .mkdir = locked_mkdir,
    .rmdir = locked_rmdir,
    .unlink = locked_unlink,
    .rename = locked_rename,
    .symlink = locked_symlink,
    .link = locked_link,
    .chmod = locked_chmod,
    .chown = locked_chown_full,
    .statfs = locked_statfs,
};

#define REDIRECT_LIST(NAME, FIELD)					\
static int NAME(NetfsMount *mount, const char *rel,			\
		NetfsDirent **entries, size_t *count, TALLOC_CTX *ctx)	\
{									\
	NetfsMount *owner = mount->redirect;				\
	return owner->ops->FIELD(owner, rel, entries, count, ctx);	\
}

#define REDIRECT_ONE(NAME, FIELD, TYPE)					\
static int NAME(NetfsMount *mount, const char *rel, TYPE arg)		\
{									\
	NetfsMount *owner = mount->redirect;				\
	return owner->ops->FIELD(owner, rel, arg);			\
}

#define REDIRECT_SIMPLE(FUNC, FIELD)					\
static int FUNC(NetfsMount *mount, const char *rel)			\
{									\
	NetfsMount *owner = mount->redirect;				\
	return owner->ops->FIELD(owner, rel);				\
}

#define REDIRECT_TWO(NAME, FIELD)					\
static int NAME(NetfsMount *mount, const char *from, const char *to)	\
{									\
	NetfsMount *owner = mount->redirect;				\
	return owner->ops->FIELD(owner, from, to);			\
}

REDIRECT_LIST(redirect_list, list)
REDIRECT_ONE(redirect_stat, stat, struct stat *)
REDIRECT_ONE(redirect_get, get, const char *)
REDIRECT_SIMPLE(redirect_rmdir, rmdir)
REDIRECT_SIMPLE(redirect_unlink, unlink)
REDIRECT_TWO(redirect_rename, rename)
REDIRECT_ONE(redirect_chmod, chmod, mode_t)

static int redirect_chown(NetfsMount *mount, const char *rel, uid_t uid,
			  gid_t gid)
{
    NetfsMount *owner = mount->redirect;

    return owner->ops->chown(owner, rel, uid, gid);
}

static int redirect_statfs(NetfsMount *mount, const char *rel,
			   struct statfs *st)
{
    NetfsMount *owner = mount->redirect;

    if (owner->ops->statfs == NULL)
	return -ENOSYS;
    return owner->ops->statfs(owner, rel, st);
}

#undef REDIRECT_LIST
#undef REDIRECT_ONE
#undef REDIRECT_SIMPLE
#undef REDIRECT_TWO

/*
 * The virtual identity of the container that creates an entry is written
 * into the image, so a container borrowing another one's mount must hand
 * its own identity to the owner before the creating operation -- and
 * atomically with it, hence directly under the owner's lock.
 */
static void redirect_take_identity(NetfsMount *owner, NetfsMount *borrower)
{
    owner->create_uid = borrower->create_uid;
    owner->create_gid = borrower->create_gid;
    owner->create_identity = borrower->create_identity;
}

static int redirect_put(NetfsMount *mount, const char *rel, const char *local)
{
    NetfsMount *owner = mount->redirect;
    int status;

    pthread_mutex_lock(&owner->lock);
    redirect_take_identity(owner, mount);
    status = owner->base_ops->put(owner, rel, local);
    pthread_mutex_unlock(&owner->lock);
    return status;
}

static int redirect_mkdir(NetfsMount *mount, const char *rel)
{
    NetfsMount *owner = mount->redirect;
    int status;

    pthread_mutex_lock(&owner->lock);
    redirect_take_identity(owner, mount);
    status = owner->base_ops->mkdir(owner, rel);
    pthread_mutex_unlock(&owner->lock);
    return status;
}

static int redirect_symlink(NetfsMount *mount, const char *rel,
			    const char *target)
{
    NetfsMount *owner = mount->redirect;
    int status;

    pthread_mutex_lock(&owner->lock);
    redirect_take_identity(owner, mount);
    status = owner->base_ops->symlink(owner, rel, target);
    pthread_mutex_unlock(&owner->lock);
    return status;
}

static int redirect_link(NetfsMount *mount, const char *from, const char *to)
{
    NetfsMount *owner = mount->redirect;
    int status;

    pthread_mutex_lock(&owner->lock);
    redirect_take_identity(owner, mount);
    status = owner->base_ops->link(owner, from, to);
    pthread_mutex_unlock(&owner->lock);
    return status;
}

static const NetfsDirOps redirect_ops = {
    .list = redirect_list,
    .stat = redirect_stat,
    .get = redirect_get,
    .put = redirect_put,
    .mkdir = redirect_mkdir,
    .rmdir = redirect_rmdir,
    .unlink = redirect_unlink,
    .rename = redirect_rename,
    .symlink = redirect_symlink,
    .link = redirect_link,
    .chmod = redirect_chmod,
    .chown = redirect_chown,
    .statfs = redirect_statfs,
};

/* ------------------------------------------------------------------ */
/* Process-wide image registry                                          */
/* ------------------------------------------------------------------ */

/*
 * Every image this process has opened, indexed by its backing storage
 * (st_dev + st_ino) so that two names for the same image -- a symlink,
 * a hard link, another mount point -- map to the same driver.
 */
typedef struct NetfsImage {
    struct NetfsImage *next;
    dev_t dev;
    ino_t ino;
    NetfsMount *mount;
    unsigned refs;
} NetfsImage;

static NetfsImage *netfs_images;
static pthread_mutex_t netfs_images_lock = PTHREAD_MUTEX_INITIALIZER;

/* Root context holding the references that keep a borrowed mount alive.
 * Private, so that no container thread allocates from the shared global
 * null context.  */
static TALLOC_CTX *netfs_refs;

/* Held while a container looks for, or opens, an image, so that two
 * containers of this process cannot open the same image twice.  */
static pthread_mutex_t netfs_open_lock = PTHREAD_MUTEX_INITIALIZER;

/* Borrow the mount already driving @dev:@ino, or NULL.  */
static NetfsMount *netfs_image_acquire(dev_t dev, ino_t ino)
{
    NetfsImage *entry;
    NetfsMount *mount = NULL;

    pthread_mutex_lock(&netfs_images_lock);
    for (entry = netfs_images; entry != NULL; entry = entry->next) {
	if (entry->dev == dev && entry->ino == ino) {
	    /* Keep the owner's mount alive as long as a container uses
	     * it: the owner may well finish and be freed first.  */
	    if (netfs_refs == NULL)
		netfs_refs = talloc_new(NULL);
	    if (netfs_refs == NULL
		|| talloc_reference(netfs_refs, entry->mount) == NULL) {
		mount = NULL;
		break;
	    }
	    entry->refs++;
	    mount = entry->mount;
	    break;
	}
    }
    pthread_mutex_unlock(&netfs_images_lock);
    return mount;
}

static void netfs_image_register(dev_t dev, ino_t ino, NetfsMount *mount)
{
    /* Not talloc: see netfs_image_acquire().  */
    NetfsImage *entry = calloc(1, sizeof(NetfsImage));

    if (entry == NULL)
	return;

    entry->dev = dev;
    entry->ino = ino;
    entry->mount = mount;
    entry->refs = 1;

    pthread_mutex_lock(&netfs_images_lock);
    entry->next = netfs_images;
    netfs_images = entry;
    pthread_mutex_unlock(&netfs_images_lock);
}

static void netfs_image_release(NetfsMount *mount)
{
    NetfsImage *entry;
    NetfsImage **link;

    pthread_mutex_lock(&netfs_images_lock);
    for (link = &netfs_images; (entry = *link) != NULL; link = &entry->next) {
	if (entry->mount == mount) {
	    if (--entry->refs == 0) {
		*link = entry->next;
		free(entry);
	    }
	    break;
	}
    }
    pthread_mutex_unlock(&netfs_images_lock);
}

/*
 * Drop the registry entry of @mount (talloc destructor) and release the
 * image it borrowed, if any.
 */
static void netfs_cache_publish(NetfsMount *mount);
static void netfs_cache_unpublish(NetfsMount *mount);

static int release_mount(NetfsMount *mount)
{
    if (mount->registered) {
	netfs_cache_unpublish(mount);
	netfs_image_release(mount);
	pthread_mutex_destroy(&mount->lock);
	mount->registered = false;
    }
    if (mount->redirect != NULL) {
	netfs_image_release(mount->redirect);
	/* Drop the reference taken by netfs_image_acquire().  */
	if (netfs_refs != NULL)
	    talloc_unlink(netfs_refs, mount->redirect);
	mount->redirect = NULL;
    }
    return 0;
}


/* ------------------------------------------------------------------ */
/* Nested images                                                        */
/* ------------------------------------------------------------------ */

/*
 * An image whose file lives inside the filesystem of another mounted
 * image must not be opened as a block device of its own: the outer
 * driver and the inner one would write the same bytes through two
 * different paths.  Every mounted image publishes its mirror in a
 * per-user directory, so any process -- not only the containers of this
 * one -- can tell that an image is nested, and refuse it.
 */

#define NETFS_CACHES "caches"

static void netfs_cache_publish(NetfsMount *mount)
{
    const char *base = netfs_share_user_dir();
    char directory[PATH_MAX];
    char path[PATH_MAX];
    FILE *file;

    if (base == NULL || mount->cache == NULL)
	return;

    snprintf(directory, sizeof(directory), "%s/%s", base, NETFS_CACHES);
    if (mkdir(directory, 0700) < 0 && errno != EEXIST)
	return;

    if (snprintf(path, sizeof(path), "%s/%d-%llu-%llu", directory,
		 (int) getpid(), (unsigned long long) mount->image_dev,
		 (unsigned long long) mount->image_ino)
	>= (int) sizeof(path))
	return;
    mount->cache_marker = talloc_strdup(mount, path);
    if (mount->cache_marker == NULL)
	return;

    file = fopen(path, "w");
    if (file == NULL)
	return;
    fprintf(file, "%s\n%s\n%llu\n%llu\n", mount->cache,
	    mount->image_path != NULL ? mount->image_path : "",
	    (unsigned long long) mount->image_dev,
	    (unsigned long long) mount->image_ino);
    fclose(file);
}

static void netfs_cache_unpublish(NetfsMount *mount)
{
    if (mount->cache_marker != NULL) {
	unlink(mount->cache_marker);
	mount->cache_marker = NULL;
    }
}

/* Is @path a file inside the mirror @cache?  */
static bool netfs_path_inside(const char *path, const char *cache)
{
    size_t length;

    if (path == NULL || cache == NULL || cache[0] == '\0')
	return false;

    length = strlen(cache);
    return strncmp(path, cache, length) == 0 && path[length] == '/';
}

/*
 * Two image *files* may share disk blocks: a reflink (btrfs, XFS) or a
 * snapshot gives two inodes whose extents point at the same physical
 * blocks.  Two drivers must never write those, so the physical extents
 * are compared as well as the paths.
 */
static struct fiemap *netfs_extents(const char *path)
{
    size_t size = sizeof(struct fiemap)
	+ 64 * sizeof(struct fiemap_extent);
    struct fiemap *map = calloc(1, size);
    int fd;

    if (map == NULL)
	return NULL;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
	free(map);
	return NULL;
    }

    map->fm_start = 0;
    map->fm_length = FIEMAP_MAX_OFFSET;
    map->fm_extent_count = 64;
    if (ioctl(fd, FS_IOC_FIEMAP, map) < 0) {
	free(map);
	map = NULL;
    }
    close(fd);
    return map;
}

static bool netfs_extents_overlap(const struct fiemap *a,
				  const struct fiemap *b)
{
    uint32_t i;
    uint32_t j;

    for (i = 0; i < a->fm_mapped_extents; i++) {
	uint64_t a_start = a->fm_extents[i].fe_physical;
	uint64_t a_end = a_start + a->fm_extents[i].fe_length;

	if ((a->fm_extents[i].fe_flags & FIEMAP_EXTENT_UNKNOWN) != 0)
	    continue;

	for (j = 0; j < b->fm_mapped_extents; j++) {
	    uint64_t b_start = b->fm_extents[j].fe_physical;
	    uint64_t b_end = b_start + b->fm_extents[j].fe_length;

	    if ((b->fm_extents[j].fe_flags & FIEMAP_EXTENT_UNKNOWN) != 0)
		continue;
	    if (a_start < b_end && b_start < a_end)
		return true;
	}
    }
    return false;
}

/* Does @path share physical blocks with the image @other?  */
static bool netfs_blocks_shared(const char *path, dev_t dev,
				const char *other, dev_t other_dev)
{
    struct fiemap *mine;
    struct fiemap *theirs;
    bool shared = false;

    /* Physical addresses are only comparable within one device.  */
    if (other == NULL || other[0] == '\0' || other_dev != dev)
	return false;

    mine = netfs_extents(path);
    if (mine == NULL)
	return false;
    theirs = netfs_extents(other);
    if (theirs != NULL) {
	shared = netfs_extents_overlap(mine, theirs);
	free(theirs);
    }
    free(mine);
    return shared;
}

/*
 * Return the mirror of a mounted image that contains @path, or NULL.
 * Both the images of this process and the ones published by the other
 * processes of the same user are considered.
 */
static char *netfs_containing_cache(TALLOC_CTX *ctx, const char *path);

/*
 * Return the image of a mounted one that shares physical blocks with
 * @path, or NULL.  Both this process and the other processes of the
 * same user are considered.
 */
static char *netfs_sharing_blocks(TALLOC_CTX *ctx, const char *path, dev_t dev,
				  ino_t ino)
{
    NetfsImage *entry;
    char directory[PATH_MAX];
    DIR *dir;
    struct dirent *item;
    char *owner = NULL;
    const char *base;

    pthread_mutex_lock(&netfs_images_lock);
    for (entry = netfs_images; entry != NULL; entry = entry->next) {
	/* The very same backing storage is shared, not conflicting.  */
	if (entry->mount->image_dev == dev && entry->mount->image_ino == ino)
	    continue;
	if (netfs_blocks_shared(path, dev, entry->mount->image_path,
				entry->mount->image_dev)) {
	    owner = talloc_strdup(ctx, entry->mount->image_path);
	    break;
	}
    }
    pthread_mutex_unlock(&netfs_images_lock);
    if (owner != NULL)
	return owner;

    base = netfs_share_user_dir();
    if (base == NULL)
	return NULL;
    snprintf(directory, sizeof(directory), "%s/%s", base, NETFS_CACHES);

    dir = opendir(directory);
    if (dir == NULL)
	return NULL;

    while ((item = readdir(dir)) != NULL) {
	char file_path[PATH_MAX + 256];
	char line[PATH_MAX];
	char other[PATH_MAX];
	char device[64];
	char inode[64];
	unsigned long long other_dev = 0;
	unsigned long long other_ino = 0;
	FILE *file;

	if (item->d_name[0] == '.')
	    continue;
	snprintf(file_path, sizeof(file_path), "%s/%s", directory,
		 item->d_name);
	file = fopen(file_path, "r");
	if (file == NULL)
	    continue;
	if (fgets(line, sizeof(line), file) == NULL
	    || fgets(other, sizeof(other), file) == NULL
	    || fgets(device, sizeof(device), file) == NULL
	    || fgets(inode, sizeof(inode), file) == NULL) {
	    fclose(file);
	    continue;
	}
	fclose(file);
	other[strcspn(other, "\n")] = '\0';
	other_dev = strtoull(device, NULL, 10);
	other_ino = strtoull(inode, NULL, 10);

	/* Same backing storage: this container may share it.  */
	if ((dev_t) other_dev == dev && (ino_t) other_ino == ino)
	    continue;

	if (netfs_blocks_shared(path, dev, other, (dev_t) other_dev)) {
	    owner = talloc_strdup(ctx, other);
	    break;
	}
    }
    closedir(dir);
    return owner;
}

static char *netfs_containing_cache(TALLOC_CTX *ctx, const char *path)
{
    NetfsImage *entry;
    char directory[PATH_MAX];
    DIR *dir;
    struct dirent *item;
    char *owner = NULL;
    const char *base;

    pthread_mutex_lock(&netfs_images_lock);
    for (entry = netfs_images; entry != NULL; entry = entry->next) {
	if (netfs_path_inside(path, entry->mount->cache)) {
	    owner = talloc_strdup(ctx, entry->mount->cache);
	    break;
	}
    }
    pthread_mutex_unlock(&netfs_images_lock);
    if (owner != NULL)
	return owner;

    base = netfs_share_user_dir();
    if (base == NULL)
	return NULL;
    snprintf(directory, sizeof(directory), "%s/%s", base, NETFS_CACHES);

    dir = opendir(directory);
    if (dir == NULL)
	return NULL;

    while ((item = readdir(dir)) != NULL) {
	char file_path[PATH_MAX + 256];
	char line[PATH_MAX];
	FILE *file;

	if (item->d_name[0] == '.')
	    continue;
	snprintf(file_path, sizeof(file_path), "%s/%s", directory,
		 item->d_name);
	file = fopen(file_path, "r");
	if (file == NULL)
	    continue;
	if (fgets(line, sizeof(line), file) == NULL) {
	    fclose(file);
	    continue;
	}
	fclose(file);
	line[strcspn(line, "\n")] = '\0';

	if (netfs_path_inside(path, line)) {
	    owner = talloc_strdup(ctx, line);
	    break;
	}
    }
    closedir(dir);
    return owner;
}

static NetfsMount *find_mount_by_host(NetfsConfig *config, const char *host,
				      const char **rel)
{
    NetfsMount *mount;

    for (mount = config->mounts; mount != NULL; mount = mount->next) {
	size_t length = strlen(mount->cache);

	if (strncmp(host, mount->cache, length) != 0)
	    continue;
	if (host[length] != '\0' && host[length] != '/')
	    continue;

	*rel = (host[length] == '/') ? host + length + 1 : host + length;
	return mount;
    }

    return NULL;
}

static NetfsNode *node_from_rel(NetfsMount *mount, const char *rel)
{
    NetfsNode *node = &mount->root;
    const char *cursor = rel;

    while (*cursor != '\0') {
	const char *slash = strchr(cursor, '/');
	size_t length = slash != NULL ? (size_t) (slash - cursor) : strlen(cursor);
	char name[NAME_MAX + 1];

	if (length == 0 || length > NAME_MAX)
	    return NULL;
	if (node->is_dir && !node->listed)
	    return NULL;

	memcpy(name, cursor, length);
	name[length] = '\0';

	node = find_child(node, name);
	if (node == NULL)
	    return NULL;

	cursor = slash != NULL ? slash + 1 : cursor + length;
    }

    return node;
}

/* ------------------------------------------------------------------ */
/* Local mirror materialization                                        */
/* ------------------------------------------------------------------ */

/*
 * Hard links share an inode, so every name of a file must share one
 * cache file too.  Two independent copies would show the guest nlink 1
 * and different inode numbers, let a write through one name leave the
 * other stale, and make a later push of that stale copy erase the
 * update.  Hard links may live in different directories, hence the walk.
 */
static NetfsNode *find_materialized_ino(NetfsNode *dir, uint64_t ino,
					NetfsNode *skip)
{
    NetfsNode *child;

    for (child = dir->child; child != NULL; child = child->sibling) {
	if (child == skip || child->deleted)
	    continue;
	if (child->ino == ino && child->materialized
	    && !child->is_dir && !child->is_link)
	    return child;
	if (child->is_dir) {
	    NetfsNode *found = find_materialized_ino(child, ino, skip);

	    if (found != NULL)
		return found;
	}
    }
    return NULL;
}

static int create_file_placeholder(NetfsNode *node)
{
    char local[PATH_MAX];
    struct timespec times[2];
    int fd;

    if (node->materialized)
	return 0;

    node_local_path(node, local, sizeof(local));

    if (node->ino != 0 && node->mount != NULL) {
	NetfsNode *twin =
	    find_materialized_ino(&node->mount->root, node->ino, node);

	if (twin != NULL) {
	    char twin_local[PATH_MAX];

	    node_local_path(twin, twin_local, sizeof(twin_local));

	    /* A stale placeholder may be in the way.  */
	    if (unlink(local) < 0 && errno != ENOENT)
		return -errno;
	    if (link(twin_local, local) == 0) {
		node->materialized = true;
		/* The two names now share their content, so it is as
		 * up to date locally as the twin's.  */
		node->fetched = twin->fetched || twin->dirty;
		return 0;
	    }
	    /* Fall back to an independent copy if link() failed.  */
	}
    }

    fd = open(local, O_CREAT | O_WRONLY | O_CLOEXEC,
	      node->mode != 0 ? (node->mode & 07777) : 0644);
    if (fd < 0)
	return -errno;

    if (node->size > 0 && ftruncate(fd, node->size) < 0) {
	int status = -errno;
	close(fd);
	return status;
    }
    close(fd);

    if (node->mtime > 0) {
	times[0].tv_sec = node->mtime;
	times[0].tv_nsec = 0;
	times[1] = times[0];
	(void) utimensat(AT_FDCWD, local, times, 0);
    }

    node->materialized = true;
    return 0;
}

static int create_link_placeholder(NetfsNode *node)
{
    char local[PATH_MAX];

    if (node->materialized)
	return 0;

    if (node->target == NULL)
	return create_file_placeholder(node);

    node_local_path(node, local, sizeof(local));

    /* A previous listing may have created a plain placeholder here.  */
    if (unlink(local) < 0 && errno != ENOENT)
	return -errno;

    if (symlink(node->target, local) < 0)
	return -errno;

    node->materialized = true;
    return 0;
}

static int create_dir_placeholder(NetfsNode *node)
{
    char local[PATH_MAX];

    if (node->materialized)
	return 0;

    node_local_path(node, local, sizeof(local));
    if (mkdir(local, node->mode != 0 ? (node->mode & 07777) : 0755) < 0
	&& errno != EEXIST)
	return -errno;

    node->materialized = true;
    return 0;
}

static int refresh_dir(Tracee *tracee, NetfsMount *mount, NetfsNode *dir)
{
    const NetfsDirOps *ops = dir_ops_of(mount);
    TALLOC_CTX *context;
    NetfsDirent *entries = NULL;
    NetfsNode *child;
    NetfsNode *next;
    size_t count = 0;
    size_t i;
    char rel[PATH_MAX];
    int status;

    if (ops == NULL || ops->list == NULL)
	return -ENOSYS;

    node_relpath(dir, rel, sizeof(rel));

    context = talloc_new(NULL);
    if (context == NULL)
	return -ENOMEM;

    status = ops->list(mount, rel, &entries, &count, context);
    if (status < 0) {
	VERBOSE(tracee, 1, "netfs: cannot list \"%s\": %s",
		rel[0] != '\0' ? rel : "/", strerror(-status));
	talloc_free(context);
	return status;
    }

    for (child = dir->child; child != NULL; child = child->sibling)
	child->seen = false;

    for (i = 0; i < count; i++) {
	NetfsDirent *entry = &entries[i];
	NetfsNode *child;

	if (entry->name == NULL || entry->name[0] == '\0'
	    || strcmp(entry->name, ".") == 0
	    || strcmp(entry->name, "..") == 0)
	    continue;

	child = find_child(dir, entry->name);
	if (child == NULL) {
	    child = new_child_node(dir, entry->name);
	    if (child == NULL) {
		talloc_free(context);
		return -ENOMEM;
	    }
	} else if (child->deleted) {
	    continue;
	}
	child->seen = true;

	child->is_dir = entry->is_dir;
	child->is_link = entry->is_link;
	child->size = entry->size;
	child->mtime = entry->mtime;
	child->mode = entry->mode;
	child->ino = entry->ino;

	if (entry->target != NULL && child->target == NULL)
	    child->target = talloc_strdup(child, entry->target);

	if (child->is_dir)
	    (void) create_dir_placeholder(child);
	else if (child->is_link && child->target != NULL)
	    (void) create_link_placeholder(child);
	else
	    (void) create_file_placeholder(child);
    }

    /*
     * Anything that is gone from the listing was removed elsewhere --
     * by another container sharing this image, or by another process in
     * the same container.  It must disappear from the local mirror too,
     * otherwise readdir(2) keeps returning it while stat(2) fails.
     */
    for (child = dir->child; child != NULL; child = next) {
	char path[PATH_MAX];

	next = child->sibling;
	if (child->seen || child->deleted)
	    continue;

	node_local_path(child, path, sizeof(path));
	if (child->is_dir)
	    (void) rmdir(path);
	else
	    (void) unlink(path);

	/* Kept alive like an unlinked-but-open entry.  */
	child->deleted = true;
	forget_child(child);
	VERBOSE(tracee, 2, "netfs: \"%s\" vanished from \"%s\"", path, rel);
    }

    dir->listed = true;
    dir->listed_at = time(NULL);

    talloc_free(context);
    return 0;
}

static int materialize_dir(Tracee *tracee, NetfsMount *mount, NetfsNode *node)
{
    int status;

    if (!node->materialized) {
	status = create_dir_placeholder(node);
	if (status < 0)
	    return status;
    }

    /* A mount shared with other containers is modified behind our
     * back, so its listings must not be cached at all.  */
    if (!node->listed
	|| mount->share_ops != NULL || mount->redirect != NULL
	|| (time(NULL) - node->listed_at) >= NETFS_DIR_TTL) {
	status = refresh_dir(tracee, mount, node);
	if (status < 0)
	    return status;
    }

    return 0;
}

/*
 * Walk @rel and make sure every component exists in the local cache.
 * On success @node_out points to the node for @rel, or NULL when the
 * resource does not exist remotely.
 */
static int materialize_path(Tracee *tracee, NetfsMount *mount, const char *rel,
			    NetfsNode **node_out)
{
    NetfsNode *node = &mount->root;
    const char *cursor = rel;
    int status;

    if (node_out != NULL)
	*node_out = NULL;

    status = materialize_dir(tracee, mount, &mount->root);
    if (status < 0)
	return status;

    while (*cursor != '\0') {
	const char *slash = strchr(cursor, '/');
	size_t length = slash != NULL ? (size_t) (slash - cursor) : strlen(cursor);
	char name[NAME_MAX + 1];
	NetfsNode *child;

	if (length == 0) {
	    cursor++;
	    continue;
	}
	if (length > NAME_MAX)
	    return -ENAMETOOLONG;

	memcpy(name, cursor, length);
	name[length] = '\0';

	child = find_child(node, name);
	if (child == NULL) {
	    /* The entry may have appeared remotely since the last
	     * listing; refresh once before giving up.  */
	    status = refresh_dir(tracee, mount, node);
	    if (status < 0)
		return status;
	    child = find_child(node, name);
	}

	if (child == NULL || child->deleted) {
	    if (node_out != NULL)
		*node_out = NULL;
	    return 0;
	}

	node = child;
	if (node->is_dir) {
	    status = materialize_dir(tracee, mount, node);
	    if (status < 0)
		return status;
	} else if (node->is_link && node->target != NULL) {
	    status = create_link_placeholder(node);
	    if (status < 0)
		return status;
	} else {
	    status = create_file_placeholder(node);
	    if (status < 0)
		return status;
	}

	cursor = slash != NULL ? slash + 1 : cursor + length;
    }

    if (node_out != NULL)
	*node_out = node;
    return 0;
}

/*
 * Create a node for a file or directory that was just created by the
 * guest through the cache.  Returns NULL if @host does not belong to
 * any netfs mount.
 */
static NetfsNode *node_create_from_host(NetfsConfig *config, const char *host,
					bool is_dir)
{
    const char *rel;
    NetfsMount *mount = find_mount_by_host(config, host, &rel);
    NetfsNode *parent;
    NetfsNode *node;
    char *copy;
    char *base;
    char *slash;

    if (mount == NULL)
	return NULL;

    copy = talloc_strdup(NULL, rel);
    if (copy == NULL)
	return NULL;

    slash = strrchr(copy, '/');
    if (slash != NULL) {
	*slash = '\0';
	base = slash + 1;
	parent = node_from_rel(mount, copy);
    } else {
	base = copy;
	parent = &mount->root;
    }

    if (parent == NULL || base[0] == '\0') {
	talloc_free(copy);
	return NULL;
    }

    node = find_child(parent, base);
    if (node == NULL) {
	node = new_child_node(parent, base);
	if (node != NULL) {
	    node->is_dir = is_dir;
	    node->materialized = true;
	    node->fetched = !is_dir;
	}
    }

    talloc_free(copy);
    return node;
}

/* ------------------------------------------------------------------ */
/* Content transfer                                                    */
/* ------------------------------------------------------------------ */

static int netfs_fetch(Tracee *tracee, NetfsMount *mount, NetfsNode *node)
{
    const NetfsDirOps *ops = dir_ops_of(mount);
    char local[PATH_MAX];
    char rel[PATH_MAX];
    int status;

    if (node->is_dir || node->is_link || node->fetched || node->dirty
	|| node->deleted)
	return 0;

    if (ops == NULL || ops->get == NULL)
	return -ENOSYS;

    if (!node->materialized) {
	status = create_file_placeholder(node);
	if (status < 0)
	    return status;
    }

    node_local_path(node, local, sizeof(local));
    node_relpath(node, rel, sizeof(rel));

    VERBOSE(tracee, 2, "netfs: fetching \"%s\"", rel);
    status = ops->get(mount, rel, local);
    if (status < 0) {
	note(tracee, WARNING, USER,
	     "netfs: cannot fetch \"%s\" from %s: %s",
	     rel, mount->display, strerror(-status));
	return status;
    }

    node->fetched = true;
    return 0;
}

static int netfs_push(Tracee *tracee, NetfsMount *mount, NetfsNode *node)
{
    const NetfsDirOps *ops = dir_ops_of(mount);
    char local[PATH_MAX];
    char rel[PATH_MAX];
    struct stat st;
    mode_t saved_mode = 0;
    bool mode_relaxed = false;
    int status;

    if (node == NULL || !node->dirty || node->is_dir || node->deleted)
	return 0;

    if (mount->read_only) {
	/* Do not pretend the data was stored.  */
	node->dirty = false;
	return -EROFS;
    }

    ops = dir_ops_of(mount);
    if (ops == NULL || ops->put == NULL)
	return -ENOSYS;

    node_local_path(node, local, sizeof(local));
    node_relpath(node, rel, sizeof(rel));

    /*
     * The cache file is uvroot's own private copy, but it carries the
     * mode the guest asked for.  A program may create a file that
     * denies the owner read access and still expect its content to be
     * stored: GNU tar does exactly that for the placeholder files it
     * writes in place of "unsafe" symlink targets (mode 0).  Reading
     * that file back to upload it would then fail with EACCES, and the
     * whole extraction aborts.  Grant the owner read access for the
     * duration of the push, then restore the guest-visible mode.
     */
    if (stat(local, &st) == 0 && S_ISREG(st.st_mode)
	&& (st.st_mode & S_IRUSR) == 0) {
	saved_mode = st.st_mode & 07777;
	if (chmod(local, saved_mode | S_IRUSR) == 0)
	    mode_relaxed = true;
    }

    VERBOSE(tracee, 2, "netfs: pushing \"%s\"", rel);
    status = ops->put(mount, rel, local);

    if (mode_relaxed) {
	(void) chmod(local, saved_mode);

	/*
	 * The backend saw the mode with the extra read bit, so the
	 * image may hold a more permissive mode than the guest asked
	 * for.  Put the real one back (a block image has its inode
	 * mode changed in place; the directory backends have no
	 * chmod here and simply ignore it).
	 */
	if (status == 0 && ops->chmod != NULL)
	    (void) ops->chmod(mount, rel, saved_mode);
    }

    if (status < 0) {
	note(tracee, WARNING, USER,
	     "netfs: cannot push \"%s\" to %s: %s",
	     rel, mount->display, strerror(-status));

	/*
	 * The data could not be stored (a full image, typically).  Do not
	 * keep a copy of it on the host: the cache must not grow past what
	 * the image can hold, and the mirror no longer matches the image.
	 */
	(void) truncate(local, 0);
	node->fetched = false;
	return status;
    }

    node->dirty = false;
    node->fetched = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-tracee state                                                    */
/* ------------------------------------------------------------------ */

static void warn_remote(Tracee *tracee, NetfsMount *mount, const char *action,
			const char *rel, int status)
{
    if (status >= 0)
	return;

    note(tracee, WARNING, USER, "netfs: cannot %s \"%s\" on %s: %s",
	 action, rel, mount->display, strerror(-status));
}

static NetfsTracee *find_netfs_tracee(NetfsConfig *config, pid_t pid)
{
    NetfsTracee *entry;

    for (entry = config->tracees; entry != NULL; entry = entry->next) {
	if (entry->pid == pid)
	    return entry;
    }

    return NULL;
}

static NetfsTracee *get_netfs_tracee(NetfsConfig *config, pid_t pid, bool create)
{
    NetfsTracee *entry = find_netfs_tracee(config, pid);

    if (entry != NULL || !create)
	return entry;

    entry = talloc_zero(config, NetfsTracee);
    if (entry == NULL)
	return NULL;

    entry->pid = pid;
    entry->next = config->tracees;
    config->tracees = entry;

    return entry;
}

static NetfsFd *fd_find(NetfsTracee *entry, int fd)
{
    NetfsFd *item;

    for (item = entry->fds; item != NULL; item = item->next) {
	if (item->fd == fd)
	    return item;
    }

    return NULL;
}

static void fd_add(NetfsTracee *entry, int fd, NetfsNode *node, bool dirty)
{
    NetfsFd *item;

    if (fd < 0 || node == NULL)
	return;

    item = fd_find(entry, fd);
    if (item == NULL) {
	item = talloc_zero(entry, NetfsFd);
	if (item == NULL)
	    return;
	item->fd = fd;
	item->next = entry->fds;
	entry->fds = item;
    }

    item->node = node;
    item->dirty = dirty;
}

static void fd_remove(NetfsTracee *entry, int fd)
{
    NetfsFd **link;

    for (link = &entry->fds; *link != NULL; link = &(*link)->next) {
	if ((*link)->fd == fd) {
	    NetfsFd *item = *link;
	    *link = item->next;
	    talloc_free(item);
	    return;
	}
    }
}

static bool fd_has_node(NetfsTracee *entry, NetfsNode *node)
{
    NetfsFd *item;

    for (item = entry->fds; item != NULL; item = item->next) {
	if (item->node == node)
	    return true;
    }

    return false;
}

/*
 * Drop the descriptor @fd from the table.  The content is pushed back
 * only when this was the last descriptor referring to the file: shells
 * commonly open a file, dup2() it over stdout and close the original
 * descriptor long before anything is written.
 */
/*
 * Drop @fd's entry.  Returns the error of the write-back, if any, so
 * that close(2) can report a full image instead of pretending the data
 * was stored.
 */
static int fd_release(Tracee *tracee, NetfsTracee *entry, int fd)
{
    NetfsFd *item = fd_find(entry, fd);
    NetfsNode *node;
    bool dirty;
    int status = 0;

    if (item == NULL)
	return 0;

    node = item->node;
    dirty = item->dirty;
    fd_remove(entry, fd);

    if (dirty && node != NULL && !fd_has_node(entry, node))
	status = netfs_push(tracee, node->mount, node);

    return status;
}

static void fd_duplicate(NetfsTracee *entry, int old_fd, int new_fd)
{
    NetfsFd *item;

    if (old_fd < 0 || new_fd < 0 || old_fd == new_fd)
	return;

    item = fd_find(entry, old_fd);
    if (item == NULL)
	return;

    fd_add(entry, new_fd, item->node, item->dirty);
}

static void inherit_tracee_state(NetfsConfig *config, pid_t parent,
				 pid_t child)
{
    NetfsTracee *parent_entry = find_netfs_tracee(config, parent);
    NetfsTracee *child_entry;
    NetfsFd *item;

    if (parent_entry == NULL)
	return;

    child_entry = get_netfs_tracee(config, child, true);
    if (child_entry == NULL)
	return;

    for (item = parent_entry->fds; item != NULL; item = item->next)
	fd_add(child_entry, item->fd, item->node, item->dirty);
}

static void flush_tracee(Tracee *tracee, NetfsConfig *config)
{
    NetfsTracee *entry = find_netfs_tracee(config, tracee->pid);
    NetfsFd *item;

    if (entry == NULL)
	return;

    for (item = entry->fds; item != NULL; item = item->next) {
	if (item->dirty && item->node != NULL)
	    (void) netfs_push(tracee, item->node->mount, item->node);
	item->dirty = false;
    }
}

/* ------------------------------------------------------------------ */
/* Syscall hooks                                                       */
/* ------------------------------------------------------------------ */

static char *netfs_cache_root(void);
static void capture_path(Tracee *tracee, Reg reg, char path[PATH_MAX]);

/*
 * Name the filesystem a mount should appear as in the synthesized mount
 * tables: a bind or the root is hostfs, a remote directory is netfs, a
 * local image is imgfs and a network block device is nblockfs.
 */
static const char *mount_fstype(const NetfsMount *mount)
{
    if (mount->backend != NULL && mount->backend->kind == NETFS_KIND_BLOCK) {
	if (mount->scheme != NULL
	    && (strncmp(mount->scheme, "iscsi", 5) == 0
		|| strncmp(mount->scheme, "nbd", 3) == 0))
	    return "nblockfs";
	return "imgfs";
    }
    return "netfs";
}

static bool guest_is_netfs_mount(NetfsConfig *config, const char *guest)
{
    NetfsMount *mount;

    for (mount = config->mounts; mount != NULL; mount = mount->next) {
	if (mount->guest != NULL && strcmp(mount->guest, guest) == 0)
	    return true;
    }
    return false;
}

/*
 * The four paths that describe the storage layout to user space.  They
 * are served from a generated file so that df(1), findmnt(1) and
 * /etc/mtab all agree with the virtual mounts.
 */
static bool is_mounts_path(const char *guest)
{
    const char *cursor;
    const char *slash;

    if (strcmp(guest, "/proc/mounts") == 0
	|| strcmp(guest, "/proc/self/mounts") == 0
	|| strcmp(guest, "/proc/self/mountinfo") == 0
	|| strcmp(guest, "/etc/mtab") == 0)
	return true;

    /*
     * The kernel exposes /proc/mounts as a symlink to self/mounts, so
     * canonicalization turns it into /proc/<pid>/mounts.  Accept those
     * too, otherwise opening it would just fall back to the (possibly
     * empty) file inside the image.
     */
    if (strncmp(guest, "/proc/", 6) != 0)
	return false;
    cursor = guest + 6;
    slash = strchr(cursor, '/');
    if (slash == NULL || slash == cursor)
	return false;
    for (; cursor < slash; cursor++) {
	if (*cursor < '0' || *cursor > '9')
	    return false;
    }
    return strcmp(slash + 1, "mounts") == 0
	|| strcmp(slash + 1, "mountinfo") == 0;
}

static void write_mount_line(FILE *file, const char *device, const char *guest,
			     const char *fstype, bool read_only)
{
    fprintf(file, "%s %s %s %s 0 0\n", device, guest, fstype,
	    read_only ? "ro" : "rw");
}

static void write_mountinfo_line(FILE *file, int id, const char *device,
				 const char *guest, const char *fstype)
{
    fprintf(file, "%d 0 0:1 / %s rw - %s %s rw\n",
	    id, guest, fstype, device);
}

/*
 * Render every netfs mount and every binding into a file under the
 * per-process cache root and return its path.  The table is rebuilt on
 * each access: bindings are installed after the mounts are created, and
 * a mount may be added later.
 */
static const char *netfs_mounts_file(Tracee *tracee, NetfsConfig *config,
				     bool mountinfo)
{
    static char storage[PATH_MAX];
    NetfsMount *mount;
    const char *root = netfs_cache_root();
    FILE *file;
    int id = 1;

    if (root == NULL || tracee->fs == NULL)
	return NULL;

    snprintf(storage, sizeof(storage), "%s/uvroot-%s", root,
	     mountinfo ? "mountinfo" : "mounts");

    file = fopen(storage, "w");
    if (file == NULL)
	return NULL;

    for (mount = config->mounts; mount != NULL; mount = mount->next) {
	const char *fstype = mount_fstype(mount);
	char device[PATH_MAX + 128];

	snprintf(device, sizeof(device), "%s:%s", fstype,
		 mount->display != NULL ? mount->display : mount->url);
	if (mountinfo)
	    write_mountinfo_line(file, id++, device, mount->guest, fstype);
	else
	    write_mount_line(file, device, mount->guest, fstype,
			     mount->read_only);
    }

    {
	Binding *binding;

	for (binding = CIRCLEQ_FIRST(tracee->fs->bindings.guest);
	     binding != (Binding *) tracee->fs->bindings.guest;
	     binding = CIRCLEQ_NEXT(binding, link.guest)) {
	    const char *guest = binding->guest.path;
	    const char *ignored;

	    /* uvroot plumbing (vperm shims, virtual passwd, injected
	     * resolver) is not storage: leave it out of df(1).  */
	    if (binding->internal)
		continue;

	    /* The mirror of a netfs mount is already listed above.  */
	    if (find_mount_by_host(config, binding->host.path,
				   &ignored) != NULL)
		continue;
	    if (guest_is_netfs_mount(config, guest))
		continue;

	    /* A -b binding is a plain host path mapping: always hostfs,
	     * whatever the file happens to contain.  */
	    if (mountinfo)
		write_mountinfo_line(file, id++, binding->host.path, guest,
				     "hostfs");
	    else
		write_mount_line(file, binding->host.path, guest, "hostfs",
				 false);
	}
    }

    fclose(file);
    return storage;
}

/*
 * Total apparent size of the file or directory a -b binding maps.  A
 * directory is walked recursively; symbolic links and special files are
 * not followed and contribute nothing, so a self-referencing tree
 * cannot loop.
 */
static uint64_t bound_object_bytes(const char *path)
{
    struct dirent *entry;
    struct stat st;
    uint64_t total = 0;
    DIR *dir;

    if (lstat(path, &st) != 0)
	return 0;
    if (S_ISREG(st.st_mode))
	return (uint64_t) st.st_size;
    if (!S_ISDIR(st.st_mode))
	return 0;

    dir = opendir(path);
    if (dir == NULL)
	return 0;

    while ((entry = readdir(dir)) != NULL) {
	char child[PATH_MAX];

	if (strcmp(entry->d_name, ".") == 0
	    || strcmp(entry->d_name, "..") == 0)
	    continue;
	if (snprintf(child, sizeof(child), "%s/%s", path,
		     entry->d_name) >= (int) sizeof(child))
	    continue;
	total += bound_object_bytes(child);
    }

    closedir(dir);
    return total;
}

/*
 * A -b binding is a host path mapping, so df(1) shows the host
 * filesystem's capacity -- and, in the "used" column, how much the
 * bound file or directory itself occupies.  The kernel already filled
 * @st with the host numbers; only f_bfree is rewritten.
 */
static bool is_user_binding(Tracee *tracee, const char *host)
{
    Binding *binding;

    if (host == NULL || host[0] != '/')
	return false;

    binding = get_binding(tracee, HOST, host);
    if (binding == NULL || binding->internal)
	return false;
    /* The guest root is the machine itself, not a bound object.  */
    if (binding->guest.path[0] == '/' && binding->guest.path[1] == '\0')
	return false;
    /* Walking /proc, /dev or /sys is meaningless (and can be endless).  */
    if (strcmp(binding->guest.path, "/proc") == 0
	|| strncmp(binding->guest.path, "/proc/", 6) == 0
	|| strcmp(binding->guest.path, "/dev") == 0
	|| strncmp(binding->guest.path, "/dev/", 5) == 0
	|| strcmp(binding->guest.path, "/sys") == 0
	|| strncmp(binding->guest.path, "/sys/", 5) == 0)
	return false;

    return true;
}

static void adjust_hostfs_statfs(const char *host, struct statfs *st)
{
    uint64_t bytes = bound_object_bytes(host);
    uint64_t unit = st->f_frsize != 0 ? (uint64_t) st->f_frsize
	: (uint64_t) st->f_bsize;
    uint64_t used_blocks;

    if (unit == 0)
	return;

    used_blocks = (bytes + unit - 1) / unit;
    if (used_blocks > (uint64_t) st->f_blocks)
	used_blocks = (uint64_t) st->f_blocks;
    st->f_bfree = (fsblkcnt_t) ((uint64_t) st->f_blocks - used_blocks);
}

/*
 * Capacity of the filesystem a statfs(2) call is about.  Returns 0 when
 * the mount was recognized (and @st filled), -ENODEV otherwise so the
 * kernel's own answer is kept for host paths.
 */
static int netfs_statfs_of(Tracee *tracee UNUSED, NetfsConfig *config,
			   NetfsTracee *entry, struct statfs *st)
{
    const NetfsDirOps *ops;
    NetfsMount *mount;
    char rel_storage[PATH_MAX];
    const char *rel = rel_storage;
    int status;

    if (!entry->pend_valid)
	return -ENODEV;

    if (entry->pend_fd >= 0) {
	NetfsFd *item = fd_find(entry, entry->pend_fd);

	if (item == NULL || item->node == NULL || item->node->mount == NULL)
	    return -ENODEV;
	mount = item->node->mount;
	node_relpath(item->node, rel_storage, sizeof(rel_storage));
    } else {
	mount = find_mount_by_host(config, entry->pend_a, &rel);
	if (mount == NULL)
	    return -ENODEV;
    }

    ops = dir_ops_of(mount);
    if (ops != NULL && ops->statfs != NULL) {
	status = ops->statfs(mount, rel, st);
	if (status == 0)
	    return 0;
    }

    /*
     * A directory transport does not know its capacity.  Report a
     * plausible non-zero one instead of the host filesystem that
     * happens to hold the cache, so df(1) shows the mount.
     */
    memset(st, 0, sizeof(*st));
    st->f_type = 0x696d6766;	/* "imgf", a private marker */
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    st->f_blocks = (256ULL << 30) / 4096;
    st->f_bfree = st->f_blocks / 2;
    st->f_bavail = st->f_bfree;
    st->f_files = 1 << 20;
    st->f_ffree = 1 << 19;
    st->f_namelen = 255;
    return 0;
}

static int handle_host_path(Tracee *tracee, NetfsConfig *config,
			    const char *host, bool is_final)
{
    const char *rel;
    NetfsMount *mount;
    NetfsNode *node = NULL;
    int status;

    mount = find_mount_by_host(config, host, &rel);
    if (mount == NULL)
	return 0;

    status = materialize_path(tracee, mount, rel, &node);
    if (status < 0)
	return status;

    /*
     * The kernel opens the executable itself, so no open() is traced
     * for it, and uvroot inspects the ELF header long before the syscall
     * is executed: the content has to be fetched right here.  This is
     * what makes a netfs mount usable as the guest rootfs.
     */
    if (is_final && node != NULL && !node->is_dir
	&& get_sysnum(tracee, ORIGINAL) == PR_execve)
	(void) netfs_fetch(tracee, mount, node);

    return 0;
}

static int handle_sysenter_start(Tracee *tracee, NetfsConfig *config)
{
    NetfsTracee *entry = get_netfs_tracee(config, tracee->pid, true);
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    NetfsFd *item;

    if (entry == NULL)
	return 0;

    entry->pend_valid = false;
    entry->pend_fd = -1;

    switch (sysnum) {
    case PR_close:{
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
	int status = fd_release(tracee, entry, fd);

	/* The image may be full: tell the caller instead of losing the
	 * error and letting it believe the file was written.  */
	if (status < 0)
	    return status;
	break;
    }

    case PR_dup2:
    case PR_dup3:{
	/* The kernel implicitly closes the target descriptor.  */
	int new_fd = (int) peek_reg(tracee, CURRENT, SYSARG_2);

	fd_release(tracee, entry, new_fd);
	break;
    }

    case PR_fsync:
    case PR_fdatasync:{
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);

	item = fd_find(entry, fd);
	if (item != NULL && item->dirty && item->node != NULL) {
	    int status = netfs_push(tracee, item->node->mount, item->node);

	    if (status < 0)
		return status;
	}
	break;
    }

    case PR_ftruncate:
    case PR_ftruncate64:{
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);

	item = fd_find(entry, fd);
	if (item != NULL && item->node != NULL) {
	    item->node->dirty = true;
	    item->node->fetched = true;
	    item->dirty = true;
	}
	break;
    }

    case PR_write:
    case PR_writev:
    case PR_pwrite64:
    case PR_pwritev:{
	int fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);

	item = fd_find(entry, fd);
	if (item != NULL && item->node != NULL) {
	    item->node->dirty = true;
	    item->node->fetched = true;
	    item->dirty = true;
	}
	break;
    }

    /*
     * The kernel-side copy helpers move data without a write() from the
     * process (cat(1) and cp(1) use copy_file_range(2), for instance),
     * so the destination descriptor is what has to be marked dirty.
     */
    case PR_copy_file_range:{
	int out_fd = (int) peek_reg(tracee, CURRENT, SYSARG_3);

	item = fd_find(entry, out_fd);
	if (item != NULL && item->node != NULL) {
	    item->node->dirty = true;
	    item->node->fetched = true;
	    item->dirty = true;
	}
	break;
    }

    case PR_sendfile:{
	int out_fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);

	item = fd_find(entry, out_fd);
	if (item != NULL && item->node != NULL) {
	    item->node->dirty = true;
	    item->node->fetched = true;
	    item->dirty = true;
	}
	break;
    }

    case PR_splice:{
	int out_fd = (int) peek_reg(tracee, CURRENT, SYSARG_3);

	item = fd_find(entry, out_fd);
	if (item != NULL && item->node != NULL) {
	    item->node->dirty = true;
	    item->node->fetched = true;
	    item->dirty = true;
	}
	break;
    }

    case PR_tee:{
	int out_fd = (int) peek_reg(tracee, CURRENT, SYSARG_2);

	item = fd_find(entry, out_fd);
	if (item != NULL && item->node != NULL) {
	    item->node->dirty = true;
	    item->node->fetched = true;
	    item->dirty = true;
	}
	break;
    }

    case PR_execve:
    case PR_exit_group:
	flush_tracee(tracee, config);
	break;

    /*
     * Remember the guest path before translation rewrites it: a few
     * storage-description paths have to be answered from a generated
     * file instead of the guest root.
     */
    case PR_open:
    case PR_creat:
	entry->pend_b[0] = '\0';
	capture_path(tracee, SYSARG_1, entry->pend_b);
	break;

    case PR_openat:
	entry->pend_b[0] = '\0';
	capture_path(tracee, SYSARG_2, entry->pend_b);
	break;

    default:
	break;
    }

    return 0;
}

static void capture_path(Tracee *tracee, Reg reg, char path[PATH_MAX])
{
    size_t length;

    if (get_sysarg_path(tracee, path, reg) < 0) {
	path[0] = '\0';
	return;
    }

    /*
     * Strip trailing slashes ("mkdir -p a/b" passes "a/" for the
     * intermediate component).  Keeping them would make the basename of
     * the relative path empty, so the entry would never be created on
     * the remote side -- a silent data loss.  The root keeps its slash.
     */
    length = strlen(path);
    while (length > 1 && path[length - 1] == '/')
	path[--length] = '\0';
}

/*
 * Read a string argument verbatim, without translating it as a guest
 * path.  Used for a symlink target, which must be stored in the remote
 * filesystem exactly as the container wrote it.
 */
static void capture_string(Tracee *tracee, Reg reg, char string[PATH_MAX])
{
    word_t address = peek_reg(tracee, ORIGINAL, reg);

    if (read_path(tracee, string, address) < 0)
	string[0] = '\0';
}

/*
 * A read-only mount refuses any change at the syscall boundary, so the
 * refusal does not depend on the virtual identity: id0 is refused too.
 */
static int deny_if_read_only(NetfsConfig *config, const char *host)
{
    const char *rel;
    NetfsMount *mount;

    if (host == NULL || host[0] == '\0')
	return 0;

    mount = find_mount_by_host(config, host, &rel);
    if (mount == NULL || !mount->read_only)
	return 0;

    /*
     * A read-only root must still let a shell write to /dev/null and
     * friends: those are device nodes and kernel filesystems, not the
     * data the flag is about.
     */
    if (mount->guest != NULL && strcmp(mount->guest, "/") == 0 && rel != NULL
	&& (strncmp(rel, "dev", 3) == 0 || strncmp(rel, "proc", 4) == 0
	    || strncmp(rel, "sys", 3) == 0 || strncmp(rel, "run", 3) == 0))
	return 0;

    return -EROFS;
}

static int handle_sysenter_end(Tracee *tracee, NetfsConfig *config,
			       intptr_t syscall_status)
{
    NetfsTracee *entry = get_netfs_tracee(config, tracee->pid, true);
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    int status = (int) syscall_status;

    if (entry == NULL)
	return 0;

    if (status < 0)
	return 0;

    switch (sysnum) {
    case PR_dup:
    case PR_dup2:
    case PR_dup3:
    case PR_fcntl:
    case PR_fcntl64:
	/* The new descriptor is only known at the exit stage.  */
	entry->pend_valid = true;
	entry->pend_node = NULL;
	entry->pend_node2 = NULL;
	break;

    case PR_execve:{
	/*
	 * The kernel opens the executable itself, so no open() is
	 * traced for it: its content has to be fetched here.  This is
	 * what makes a netfs mount usable as the guest rootfs.
	 */
	char host[PATH_MAX];
	const char *rel;
	NetfsMount *mount;
	NetfsNode *node = NULL;

	capture_path(tracee, SYSARG_1, host);
	if (host[0] == '\0')
	    break;

	mount = find_mount_by_host(config, host, &rel);
	if (mount == NULL)
	    break;

	if (materialize_path(tracee, mount, rel, &node) < 0)
	    break;
	if (node != NULL && !node->is_dir)
	    (void) netfs_fetch(tracee, mount, node);
	break;
    }

    case PR_open:
    case PR_openat:
    case PR_creat:{
	char host[PATH_MAX];
	word_t flags;
	bool new_file = false;

	if (sysnum == PR_open) {
	    flags = peek_reg(tracee, CURRENT, SYSARG_2);
	    capture_path(tracee, SYSARG_1, host);
	} else if (sysnum == PR_openat) {
	    flags = peek_reg(tracee, CURRENT, SYSARG_3);
	    capture_path(tracee, SYSARG_2, host);
	} else {
	    flags = O_CREAT | O_WRONLY | O_TRUNC;
	    capture_path(tracee, SYSARG_1, host);
	}

	if (host[0] == '\0')
	    return 0;

	/*
	 * The storage-description paths (/proc/mounts, /etc/mtab, ...)
	 * are answered from a generated table, using the guest path
	 * captured before translation.
	 */
	if (is_mounts_path(entry->pend_b)) {
	    const char *generated = netfs_mounts_file(tracee, config,
		strstr(entry->pend_b, "mountinfo") != NULL);
	    Reg path_reg = (sysnum == PR_openat) ? SYSARG_2 : SYSARG_1;

	    if (generated != NULL
		&& set_sysarg_path(tracee, generated, path_reg) == 0)
		return 0;
	}

	entry->pend_a[0] = '\0';
	strncpy(entry->pend_a, host, PATH_MAX - 1);
	entry->pend_a[PATH_MAX - 1] = '\0';
	entry->pend_valid = true;
	entry->pend_node = NULL;
	entry->pend_node2 = NULL;

	{
	    const char *rel;
	    NetfsMount *mount = find_mount_by_host(config, host, &rel);
	    NetfsNode *node = NULL;

	    if (mount == NULL)
		return 0;

	    if ((flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC
			  | O_APPEND)) != 0) {
		int read_only = deny_if_read_only(config, host);

		if (read_only < 0)
		    return read_only;
	    }

	    if (materialize_path(tracee, mount, rel, &node) < 0)
		return 0;

	    entry->pend_node = node;
	    if (node == NULL)
		new_file = true;

	    /* A file that does not exist yet, or that is going to be
	     * truncated, does not need its content fetched.  */
	    if (!new_file && (flags & O_DIRECTORY) == 0
		&& (flags & O_PATH) == 0 && (flags & O_TRUNC) == 0
		&& node != NULL && !node->is_dir) {
		if (netfs_fetch(tracee, mount, node) < 0)
		    return 0;
	    }
	}
	break;
    }

    case PR_unlink:
    case PR_mkdir:
    case PR_rmdir:
    case PR_truncate:
    case PR_truncate64:
	capture_path(tracee, SYSARG_1, entry->pend_a);
	entry->pend_valid = (entry->pend_a[0] != '\0');
        entry->pend_node = NULL;
        entry->pend_node2 = NULL;
	status = deny_if_read_only(config, entry->pend_a);
	if (status < 0)
	    return status;
	break;

    case PR_unlinkat:
    case PR_mkdirat:
	capture_path(tracee, SYSARG_2, entry->pend_a);
	entry->pend_valid = (entry->pend_a[0] != '\0');
        entry->pend_node = NULL;
        entry->pend_node2 = NULL;
	status = deny_if_read_only(config, entry->pend_a);
	if (status < 0)
	    return status;
	break;

    case PR_rename:
	capture_path(tracee, SYSARG_1, entry->pend_a);
	capture_path(tracee, SYSARG_2, entry->pend_b);
	entry->pend_valid = (entry->pend_a[0] != '\0');
        entry->pend_node = NULL;
        entry->pend_node2 = NULL;
	status = deny_if_read_only(config, entry->pend_a);
	if (status >= 0)
	    status = deny_if_read_only(config, entry->pend_b);
	if (status < 0)
	    return status;
	break;

    case PR_renameat:
    case PR_renameat2:
	capture_path(tracee, SYSARG_2, entry->pend_a);
	capture_path(tracee, SYSARG_4, entry->pend_b);
	entry->pend_valid = (entry->pend_a[0] != '\0');
        entry->pend_node = NULL;
        entry->pend_node2 = NULL;
	status = deny_if_read_only(config, entry->pend_a);
	if (status >= 0)
	    status = deny_if_read_only(config, entry->pend_b);
	if (status < 0)
	    return status;
	break;

    case PR_statfs:
    case PR_statfs64:
	capture_path(tracee, SYSARG_1, entry->pend_a);
	entry->pend_fd = -1;
	entry->pend_valid = (entry->pend_a[0] != '\0');
	entry->pend_node = NULL;
	entry->pend_node2 = NULL;
	break;

    case PR_fstatfs:
    case PR_fstatfs64:
	entry->pend_fd = (int) peek_reg(tracee, CURRENT, SYSARG_1);
	entry->pend_a[0] = '\0';
	entry->pend_valid = true;
	entry->pend_node = NULL;
	entry->pend_node2 = NULL;
	break;

    case PR_symlink:
    case PR_symlinkat:
    case PR_link:
    case PR_linkat:{
	/*
	 * The directory transports (FTP, SMB, ...) cannot represent
	 * links, so they are refused there instead of letting the local
	 * cache diverge silently.  A block backend carries a real
	 * filesystem, so links are recorded inside the image at the exit
	 * stage.
	 */
	char link_path[PATH_MAX];
	char other[PATH_MAX];
	NetfsMount *mount;
	const char *ignored;
	const NetfsDirOps *ops;
	bool is_symlink;

	is_symlink = (sysnum == PR_symlink || sysnum == PR_symlinkat);

	if (sysnum == PR_symlinkat)
	    capture_path(tracee, SYSARG_3, link_path);
	else if (sysnum == PR_linkat)
	    capture_path(tracee, SYSARG_4, link_path);
	else
	    capture_path(tracee, SYSARG_2, link_path);

	if (link_path[0] == '\0')
	    break;

	mount = find_mount_by_host(config, link_path, &ignored);
	if (mount == NULL)
	    break;

	ops = dir_ops_of(mount);
	if (mount->backend != NULL
	    && mount->backend->kind == NETFS_KIND_BLOCK
	    && !mount->read_only
	    && ((is_symlink && ops != NULL && ops->symlink != NULL)
		|| (!is_symlink && ops != NULL && ops->link != NULL))) {
	    if (is_symlink)
		capture_string(tracee, SYSARG_1, other);
	    else if (sysnum == PR_linkat)
		capture_path(tracee, SYSARG_2, other);
	    else
		capture_path(tracee, SYSARG_1, other);

	    if (other[0] == '\0')
		break;

	    /*
	     * A hard link needs its source to exist in the local cache
	     * before the kernel creates the second name, so fetch it
	     * from the image first.  Symbolic links only carry a target
	     * string and need nothing.
	     */
	    if (!is_symlink) {
		const char *source_rel = NULL;
		NetfsMount *source_mount =
		    find_mount_by_host(config, other, &source_rel);
		NetfsNode *source_node = NULL;

		if (source_mount == mount && source_rel != NULL
		    && materialize_path(tracee, source_mount, source_rel,
					&source_node) == 0
		    && source_node != NULL && !source_node->is_dir
		    && !source_node->is_link)
		    (void) netfs_fetch(tracee, source_mount, source_node);
	    }

	    snprintf(entry->pend_a, sizeof(entry->pend_a), "%s", link_path);
	    snprintf(entry->pend_b, sizeof(entry->pend_b), "%s", other);
	    entry->pend_node = NULL;
	    entry->pend_node2 = NULL;
	    entry->pend_valid = true;
	    break;
	}

	return -EPERM;
    }

    default:
	break;
    }

    return 0;
}

static int handle_sysexit_end(Tracee *tracee, NetfsConfig *config)
{
    NetfsTracee *entry = find_netfs_tracee(config, tracee->pid);
    Sysnum sysnum;
    word_t result;

    if (entry == NULL || !entry->pend_valid)
	return 0;

    sysnum = get_sysnum(tracee, ORIGINAL);
    result = peek_reg(tracee, CURRENT, SYSARG_RESULT);

    /*
     * Capacity of a virtual mount.  The kernel already filled the
     * buffer with the statistics of the host filesystem holding the
     * cache; replace them with the real ones when the path belongs to a
     * netfs mount.  For a plain -b binding the host numbers stand, but
     * "used" is rewritten to the size of the bound file or directory.
     */
    if ((int) result >= 0
	&& (sysnum == PR_statfs || sysnum == PR_statfs64
	    || sysnum == PR_fstatfs || sysnum == PR_fstatfs64)) {
	struct statfs st;
	word_t buffer = peek_reg(tracee, ORIGINAL, SYSARG_2);

	if (!is_32on64_mode(tracee) && buffer != 0) {
	    if (netfs_statfs_of(tracee, config, entry, &st) == 0) {
		(void) write_data(tracee, buffer, &st, sizeof(st));
	    } else if (entry->pend_fd < 0
		       && is_user_binding(tracee, entry->pend_a)
		       && read_data(tracee, &st, buffer, sizeof(st)) == 0) {
		adjust_hostfs_statfs(entry->pend_a, &st);
		(void) write_data(tracee, buffer, &st, sizeof(st));
	    }
	}
	entry->pend_valid = false;
	return 0;
    }

    entry->pend_valid = false;

    if ((int) result < 0)
	return 0;

    switch (sysnum) {
    case PR_open:
    case PR_openat:
    case PR_creat:{
	word_t flags;
	NetfsNode *node = entry->pend_node;
	NetfsMount *mount = NULL;
	bool was_new = (node == NULL);
	bool writable;

	if (node != NULL) {
	    mount = node->mount;
	} else {
	    const char *rel = NULL;

	    mount = find_mount_by_host(config, entry->pend_a, &rel);
	    if (mount == NULL)
		break;
	    node = node_create_from_host(config, entry->pend_a, false);
	    if (node == NULL)
		break;
	}
	(void) mount;

	if (sysnum == PR_open)
	    flags = peek_reg(tracee, ORIGINAL, SYSARG_2);
	else if (sysnum == PR_openat)
	    flags = peek_reg(tracee, ORIGINAL, SYSARG_3);
	else
	    flags = O_CREAT | O_WRONLY | O_TRUNC;

	if (node->is_dir) {
	    /* Track the descriptor as well: fstatfs(2) has only the fd to
	     * find the mount with.  It is never dirty.  */
	    fd_add(entry, (int) result, node, false);
	    break;
	}

	writable = ((flags & O_ACCMODE) != O_RDONLY);
	node->materialized = true;

	/*
	 * Creating or truncating changes the remote content even if
	 * nothing is written afterwards, so it must be pushed back.
	 * Plain writes are caught by the write syscalls instead, which
	 * avoids uploading files that are only opened for writing.
	 */
	if (writable && (was_new || (flags & O_TRUNC) != 0)) {
	    node->dirty = true;
	    node->fetched = true;
	}
	fd_add(entry, (int) result, node, writable);
	break;
    }

    case PR_unlink:
    case PR_unlinkat:{
	const char *rel;
	NetfsMount *mount = find_mount_by_host(config, entry->pend_a, &rel);
	NetfsNode *node;

	if (mount == NULL)
	    break;
	node = node_from_rel(mount, rel);
	if (node != NULL && !node->is_dir) {
	    const NetfsDirOps *ops = dir_ops_of(mount);
	    if (ops != NULL && ops->unlink != NULL && !mount->read_only)
		warn_remote(tracee, mount, "remove", rel,
			    ops->unlink(mount, rel));
	    node->deleted = true;
	    forget_child(node);
	}
	break;
    }

    case PR_mkdir:
    case PR_mkdirat:{
	const char *rel;
	NetfsMount *mount = find_mount_by_host(config, entry->pend_a, &rel);
	NetfsNode *node;

	if (mount == NULL)
	    break;
	node = node_create_from_host(config, entry->pend_a, true);
	if (node == NULL)
	    break;
	node->is_dir = true;
	node->materialized = true;
	node->listed = true;
	node->listed_at = time(NULL);

	{
	    const NetfsDirOps *ops = dir_ops_of(mount);
	    if (ops != NULL && ops->mkdir != NULL && !mount->read_only) {
		warn_remote(tracee, mount, "create directory", rel,
			    ops->mkdir(mount, rel));
	    }
	}
	break;
    }

    case PR_rmdir:{
	const char *rel;
	NetfsMount *mount = find_mount_by_host(config, entry->pend_a, &rel);
	NetfsNode *node;

	if (mount == NULL)
	    break;
	node = node_from_rel(mount, rel);
	if (node != NULL && node->is_dir) {
	    const NetfsDirOps *ops = dir_ops_of(mount);
	    if (ops != NULL && ops->rmdir != NULL && !mount->read_only)
		warn_remote(tracee, mount, "remove directory", rel,
			    ops->rmdir(mount, rel));
	    node->deleted = true;
	    forget_child(node);
	}
	break;
    }

    case PR_truncate:
    case PR_truncate64:{
	const char *rel;
	NetfsMount *mount = find_mount_by_host(config, entry->pend_a, &rel);
	NetfsNode *node;
	int status;

	if (mount == NULL)
	    break;
	node = node_from_rel(mount, rel);
	if (node != NULL && !node->is_dir) {
	    node->dirty = true;
	    node->fetched = true;
	    status = netfs_push(tracee, mount, node);
	    if (status < 0)
		return status;
	}
	break;
    }

    case PR_symlink:
    case PR_symlinkat:{
	const char *rel;
	NetfsMount *mount = find_mount_by_host(config, entry->pend_a, &rel);

	if (mount == NULL)
	    break;
	{
	    const NetfsDirOps *ops = dir_ops_of(mount);
	    if (ops != NULL && ops->symlink != NULL && !mount->read_only)
		warn_remote(tracee, mount, "create symlink", rel,
			    ops->symlink(mount, rel, entry->pend_b));
	}
	break;
    }

    case PR_link:
    case PR_linkat:{
	const char *old_rel;
	const char *new_rel;
	NetfsMount *new_mount =
	    find_mount_by_host(config, entry->pend_a, &new_rel);
	NetfsMount *old_mount =
	    find_mount_by_host(config, entry->pend_b, &old_rel);

	if (new_mount == NULL || new_mount != old_mount)
	    break;
	{
	    const NetfsDirOps *ops = dir_ops_of(new_mount);
	    if (ops != NULL && ops->link != NULL && !new_mount->read_only)
		warn_remote(tracee, new_mount, "create hard link", new_rel,
			    ops->link(new_mount, old_rel, new_rel));
	}
	break;
    }

    case PR_rename:
    case PR_renameat:
    case PR_renameat2:{
	const char *old_rel;
	const char *new_rel;
	NetfsMount *old_mount = find_mount_by_host(config, entry->pend_a, &old_rel);
	NetfsMount *new_mount = find_mount_by_host(config, entry->pend_b, &new_rel);
	NetfsNode *node;
	NetfsNode *parent;
	char *copy;
	char *base;
	char *slash;

	if (old_mount == NULL || new_mount != old_mount || old_mount->read_only)
	    break;

	node = node_from_rel(old_mount, old_rel);
	if (node == NULL)
	    break;

	{
	    const NetfsDirOps *ops = dir_ops_of(old_mount);
	    if (ops != NULL && ops->rename != NULL)
		warn_remote(tracee, old_mount, "rename", old_rel,
			    ops->rename(old_mount, old_rel, new_rel));
	}

	copy = talloc_strdup(NULL, new_rel);
	if (copy == NULL)
	    break;
	slash = strrchr(copy, '/');
	if (slash != NULL) {
	    *slash = '\0';
	    base = slash + 1;
	    parent = node_from_rel(old_mount, copy);
	} else {
	    base = copy;
	    parent = &old_mount->root;
	}
	if (parent == NULL || base[0] == '\0') {
	    talloc_free(copy);
	    break;
	}

	forget_child(node);
	node->parent = parent;
	node->sibling = parent->child;
	parent->child = node;
	talloc_free(node->name);
	node->name = talloc_strdup(node, base);
	talloc_free(copy);
	break;
    }

    case PR_dup:
	fd_duplicate(entry, (int) peek_reg(tracee, ORIGINAL, SYSARG_1),
		     (int) result);
	break;

    case PR_dup2:
    case PR_dup3:
	fd_duplicate(entry, (int) peek_reg(tracee, ORIGINAL, SYSARG_1),
		     (int) result);
	break;

    case PR_fcntl:
    case PR_fcntl64:{
	int command = (int) peek_reg(tracee, ORIGINAL, SYSARG_2);

	if (command == F_DUPFD || command == F_DUPFD_CLOEXEC)
	    fd_duplicate(entry, (int) peek_reg(tracee, ORIGINAL, SYSARG_1),
			 (int) result);
	break;
    }

    default:
	break;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Extension boilerplate                                               */
/* ------------------------------------------------------------------ */

static FilteredSysnum netfs_sysnums[] = {
    { PR_open, FILTER_SYSEXIT },
    { PR_openat, FILTER_SYSEXIT },
    { PR_creat, FILTER_SYSEXIT },
    { PR_close, 0 },
    { PR_dup, FILTER_SYSEXIT },
    { PR_dup2, FILTER_SYSEXIT },
    { PR_dup3, FILTER_SYSEXIT },
    { PR_fcntl, FILTER_SYSEXIT },
    { PR_fcntl64, FILTER_SYSEXIT },
    { PR_fsync, 0 },
    { PR_fdatasync, 0 },
    { PR_ftruncate, 0 },
    { PR_ftruncate64, 0 },
    { PR_execve, 0 },
    { PR_exit_group, 0 },
    /*
     * Writes are tracked so that content modified after an fsync() is
     * pushed again on close.  This is the price of not losing data;
     * these syscalls are only traced when netfs is in use.
     */
    { PR_write, 0 },
    { PR_writev, 0 },
    { PR_pwrite64, 0 },
    { PR_pwritev, 0 },
    /* Kernel-side copies bypass write(); they still make the
     * destination dirty.  */
    { PR_copy_file_range, 0 },
    { PR_sendfile, 0 },
    { PR_splice, 0 },
    { PR_tee, 0 },
    /* These are already traced by uvroot but without an exit stop, and
     * the remote side can only be updated once the kernel succeeded.  */
    { PR_mkdir, FILTER_SYSEXIT },
    { PR_mkdirat, FILTER_SYSEXIT },
    { PR_rmdir, FILTER_SYSEXIT },
    { PR_unlink, FILTER_SYSEXIT },
    { PR_unlinkat, FILTER_SYSEXIT },
    { PR_truncate, FILTER_SYSEXIT },
    { PR_truncate64, FILTER_SYSEXIT },
    /* df(1) asks how much room a virtual mount has.  */
    { PR_statfs, FILTER_SYSEXIT },
    { PR_statfs64, FILTER_SYSEXIT },
    { PR_fstatfs, FILTER_SYSEXIT },
    { PR_fstatfs64, FILTER_SYSEXIT },
    { PR_rename, FILTER_SYSEXIT },
    { PR_renameat, FILTER_SYSEXIT },
    { PR_renameat2, FILTER_SYSEXIT },
    { PR_symlink, FILTER_SYSEXIT },
    { PR_symlinkat, FILTER_SYSEXIT },
    { PR_link, FILTER_SYSEXIT },
    { PR_linkat, FILTER_SYSEXIT },
    FILTERED_SYSNUM_END,
};

int netfs_callback(Extension *extension, ExtensionEvent event, intptr_t d1,
		   intptr_t d2 UNUSED)
{
    switch (event) {
    case INITIALIZATION:
	if (extension->config == NULL) {
	    extension->config = talloc_zero(extension, NetfsConfig);
	    if (extension->config == NULL)
		return -1;
	}
	extension->filtered_sysnums = netfs_sysnums;
	return 0;

    case INHERIT_PARENT:{
	Tracee *parent = TRACEE(extension);
	Tracee *child = (Tracee *) d1;

	inherit_tracee_state(config_of(extension), parent->pid, child->pid);
	return 0;
    }

    case HOST_PATH:
	return handle_host_path(TRACEE(extension), config_of(extension),
				(const char *) d1, (bool) d2);

    case SYSCALL_ENTER_START:
	return handle_sysenter_start(TRACEE(extension), config_of(extension));

    case SYSCALL_ENTER_END:
	return handle_sysenter_end(TRACEE(extension), config_of(extension), d1);

    case SYSCALL_EXIT_END:
	return handle_sysexit_end(TRACEE(extension), config_of(extension));

    case REMOVED:
	return 0;

    case PRINT_CONFIG:{
	NetfsConfig *config = config_of(extension);
	NetfsMount *mount;

	for (mount = config->mounts; mount != NULL; mount = mount->next)
	    note(TRACEE(extension), INFO, USER, "netfs = %s:%s",
		 mount->guest, mount->display);
	return 0;
    }

    default:
	return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Mount creation (called from the command line interface)             */
/* ------------------------------------------------------------------ */

static char *netfs_cache_root_storage = NULL;
static bool netfs_cache_root_temporary = false;

static int netfs_remove_tree(const char *path)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir(path);
    if (dir == NULL)
	return -1;

    while ((entry = readdir(dir)) != NULL) {
	char child[PATH_MAX];
	struct stat st;

	if (strcmp(entry->d_name, ".") == 0
	    || strcmp(entry->d_name, "..") == 0)
	    continue;

	snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
	if (lstat(child, &st) < 0)
	    continue;

	if (S_ISDIR(st.st_mode))
	    (void) netfs_remove_tree(child);
	else
	    (void) unlink(child);
    }

    closedir(dir);
    return rmdir(path);
}

static void netfs_remove_cache(void)
{
    if (netfs_cache_root_storage == NULL)
	return;
    if (netfs_cache_root_temporary)
	(void) netfs_remove_tree(netfs_cache_root_storage);
    free(netfs_cache_root_storage);
    netfs_cache_root_storage = NULL;
}

/*
 * Pick a writable location for the mirror.  Android/Termux has no
 * /tmp and the shell environment is not guaranteed, so every candidate
 * is probed in turn and the first one that yields a usable directory
 * wins.  UVROOT_NETFS_CACHE overrides everything.
 */
static char *netfs_cache_root(void)
{
    const char *candidates[6];
    size_t count = 0;
    const char *environment;
    char template_path[PATH_MAX];
    size_t i;

    if (netfs_cache_root_storage != NULL)
	return netfs_cache_root_storage;

    environment = getenv("UVROOT_NETFS_CACHE");
    if (environment != NULL && environment[0] != '\0') {
	/* Treat the variable as a replacement for TMPDIR: a unique
	 * directory is created inside it so that concurrent runs never
	 * share a mirror.  */
	if (mkdir(environment, 0700) < 0 && errno != EEXIST)
	    return NULL;
	snprintf(template_path, sizeof(template_path),
		 "%s/uvroot-netfs-XXXXXX", environment);
	if (mkdtemp(template_path) == NULL)
	    return NULL;
	netfs_cache_root_storage = strdup(template_path);
	if (netfs_cache_root_storage != NULL) {
	    netfs_cache_root_temporary = true;
	    atexit(netfs_remove_cache);
	}
	return netfs_cache_root_storage;
    }

    environment = getenv("TMPDIR");
    if (environment != NULL && environment[0] != '\0')
	candidates[count++] = environment;

    /* Termux and other Android distributions.  */
    environment = getenv("PREFIX");
    if (environment != NULL && environment[0] != '\0') {
	static char prefix_tmp[PATH_MAX];
	snprintf(prefix_tmp, sizeof(prefix_tmp), "%s/tmp", environment);
	candidates[count++] = prefix_tmp;
    }

    environment = getenv("HOME");
    if (environment != NULL && environment[0] != '\0') {
	static char home_cache[PATH_MAX];
	snprintf(home_cache, sizeof(home_cache), "%s/.cache", environment);
	candidates[count++] = home_cache;
    }

    environment = getenv("EXTERNAL_STORAGE");
    if (environment != NULL && environment[0] != '\0')
	candidates[count++] = environment;

    candidates[count++] = "/tmp";

    for (i = 0; i < count; i++) {
	if (candidates[i][0] == '\0')
	    continue;
	snprintf(template_path, sizeof(template_path),
		 "%s/uvroot-netfs-XXXXXX", candidates[i]);
	if (mkdir(candidates[i], 0700) < 0 && errno != EEXIST)
	    continue;
	if (mkdtemp(template_path) == NULL)
	    continue;
	netfs_cache_root_storage = strdup(template_path);
	if (netfs_cache_root_storage != NULL) {
	    netfs_cache_root_temporary = true;
	    atexit(netfs_remove_cache);
	}
	return netfs_cache_root_storage;
    }

    return NULL;
}

static const char *scheme_of(const char *url)
{
    static char scheme[32];
    const char *marker = strstr(url, "://");
    size_t length;

    if (marker == NULL)
	return NULL;

    length = (size_t) (marker - url);
    if (length == 0 || length >= sizeof(scheme))
	return NULL;

    memcpy(scheme, url, length);
    scheme[length] = '\0';
    return scheme;
}

static const char *default_guest_for(const NetfsBackend *backend)
{
    if (strcmp(backend->name, "ftp") == 0)
	return "/mnt/ftp";
    if (strcmp(backend->name, "smb") == 0)
	return "/mnt/smb";
    if (strcmp(backend->name, "nfs") == 0)
	return "/mnt/nfs";
    if (strcmp(backend->name, "iscsi") == 0)
	return "/mnt/iscsi";
    if (strcmp(backend->name, "nbd") == 0)
	return "/mnt/nbd";
    if (strcmp(backend->name, "img") == 0)
	return "/mnt/img";
    if (strcmp(backend->name, "qcow2") == 0)
	return "/mnt/qcow2";
    return "/mnt/netfs";
}

static bool guest_is_used(NetfsConfig *config, const char *guest)
{
    NetfsMount *mount;

    for (mount = config->mounts; mount != NULL; mount = mount->next) {
	if (strcmp(mount->guest, guest) == 0)
	    return true;
    }
    return false;
}

static void make_display_url(char *buffer, size_t size, const char *url)
{
    const char *marker;
    const char *at;
    const char *cursor;

    /* Strip "user:password@" so credentials never reach the logs.  */
    marker = strstr(url, "://");
    if (marker == NULL) {
	snprintf(buffer, size, "%s", url);
	return;
    }

    cursor = marker + 3;
    at = strchr(cursor, '@');
    /* Only treat the last '@' before the next '/' as credentials.  */
    {
	const char *slash = strchr(cursor, '/');
	if (at != NULL && (slash == NULL || at < slash))
	    snprintf(buffer, size, "%.*s***@%s",
		     (int) (cursor - url), url, at + 1);
	else
	    snprintf(buffer, size, "%s", url);
    }
}

static int parse_spec(TALLOC_CTX *context, const char *spec,
		      char **guest, char **url)
{
    const char *colon;

    *guest = NULL;
    *url = NULL;

    if (spec[0] == '/' && (colon = strchr(spec, ':')) != NULL) {
	*guest = talloc_strndup(context, spec, colon - spec);
	*url = talloc_strdup(context, colon + 1);
    } else {
	*url = talloc_strdup(context, spec);
    }

    if (*url == NULL || (*guest != NULL && (*guest)[0] == '\0'))
	return -EINVAL;

    return 0;
}

const char *netfs_mount_guest_for_host(Tracee *tracee, const char *host)
{
    Extension *extension;
    NetfsConfig *config;
    NetfsMount *mount;
    const char *rel;

    extension = get_extension(tracee, netfs_callback);
    if (extension == NULL || extension->config == NULL)
	return NULL;

    config = config_of(extension);

    for (mount = config->mounts; mount != NULL; mount = mount->next) {
	if (find_mount_by_host(config, host, &rel) == mount)
	    return mount->guest;
    }

    return NULL;
}

static NetfsMount *mount_for_cache(Tracee *tracee, const char *host,
				   const char **rel)
{
    Extension *extension = get_extension(tracee, netfs_callback);
    NetfsConfig *config;
    NetfsMount *mount;

    if (extension == NULL || extension->config == NULL)
	return NULL;

    config = config_of(extension);
    mount = find_mount_by_host(config, host, rel);

    if (mount == NULL || mount->backend == NULL
	|| mount->backend->kind != NETFS_KIND_BLOCK)
	return NULL;

    return mount;
}

int netfs_block_chmod(Tracee *tracee, const char *host, mode_t mode)
{
    const char *rel = NULL;
    NetfsMount *mount = mount_for_cache(tracee, host, &rel);
    const NetfsDirOps *ops;

    if (mount == NULL)
	return -ENODEV;

    ops = dir_ops_of(mount);
    if (ops == NULL || ops->chmod == NULL)
	return -ENOSYS;

    return ops->chmod(mount, rel, mode);
}

int netfs_block_chown(Tracee *tracee, const char *host, uid_t uid, gid_t gid)
{
    const char *rel = NULL;
    NetfsMount *mount = mount_for_cache(tracee, host, &rel);
    const NetfsDirOps *ops;

    if (mount == NULL)
	return -ENODEV;

    ops = dir_ops_of(mount);
    if (ops == NULL || ops->chown == NULL)
	return -ENOSYS;

    return ops->chown(mount, rel, uid, gid);
}

int netfs_block_stat(Tracee *tracee, const char *host, struct stat *st)
{
    const char *rel = NULL;
    NetfsMount *mount = mount_for_cache(tracee, host, &rel);
    const NetfsDirOps *ops;

    if (mount == NULL)
	return -ENODEV;

    ops = dir_ops_of(mount);
    if (ops == NULL || ops->stat == NULL)
	return -ENOSYS;

    return ops->stat(mount, rel, st);
}

int netfs_lock_image(int fd, bool exclusive, const char *path)
{
    const char *override = getenv("UVROOT_NETFS_NO_LOCK");
    int operation = (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB;

    if (override != NULL && override[0] != '\0'
	&& strcmp(override, "0") != 0)
	return 0;

    if (flock(fd, operation) == 0)
	return 0;

    if (errno == EWOULDBLOCK || errno == EAGAIN) {
	note(NULL, ERROR, USER,
	     "netfs: image \"%s\" is already used by another container "
	     "(set UVROOT_NETFS_NO_LOCK=1 to override)", path);
	return -EBUSY;
    }

    /* Not every filesystem supports flock(); do not refuse the mount
     * just because the lock could not be installed.  */
    VERBOSE(NULL, 1, "netfs: cannot lock \"%s\": %s", path,
	    strerror(errno));
    return 0;
}

void netfs_block_set_identity(Tracee *tracee, uid_t uid, gid_t gid)
{
    Extension *extension = get_extension(tracee, netfs_callback);
    NetfsMount *mount;

    if (extension == NULL || extension->config == NULL)
	return;

    for (mount = config_of(extension)->mounts; mount != NULL;
	 mount = mount->next) {
	if (mount->backend != NULL
	    && mount->backend->kind == NETFS_KIND_BLOCK) {
	    mount->create_uid = uid;
	    mount->create_gid = gid;
	    mount->create_identity = true;
	}
    }
}

bool netfs_is_block_root(Tracee *tracee, const char *host)
{
    Extension *extension = get_extension(tracee, netfs_callback);
    NetfsConfig *config;
    NetfsMount *mount;

    if (extension == NULL || extension->config == NULL)
	return false;

    config = config_of(extension);

    for (mount = config->mounts; mount != NULL; mount = mount->next) {
	if (mount->backend != NULL
	    && mount->backend->kind == NETFS_KIND_BLOCK
	    && strcmp(mount->cache, host) == 0)
	    return true;
    }

    return false;
}

bool netfs_mount_at(Tracee *tracee, unsigned int index, const char **guest,
		    const char **cache)
{
    Extension *extension;
    NetfsConfig *config;
    NetfsMount *mount;
    unsigned int i = 0;

    extension = get_extension(tracee, netfs_callback);
    if (extension == NULL || extension->config == NULL)
	return false;

    config = config_of(extension);

    for (mount = config->mounts; mount != NULL; mount = mount->next) {
	if (i++ != index)
	    continue;
	if (guest != NULL)
	    *guest = mount->guest;
	if (cache != NULL)
	    *cache = mount->cache;
	return true;
    }

    return false;
}

/*
 * Open a block backend and stack a filesystem driver on it; the caller
 * then owns the image and serves the other processes.  Returns -ENODEV
 * when no driver can read the filesystem.
 */
static int open_block_mount(Tracee *tracee, NetfsMount *mount, const char *url)
{
    const NetfsBackend *backend = mount->backend;
    const NetfsFsDriver *driver = NULL;
    size_t i;
    int status;

    if (backend->init != NULL) {
	status = backend->init(mount, url);
	if (status < 0)
	    return status;
    }

    for (i = 0; netfs_fs_drivers[i] != NULL && driver == NULL; i++) {
	const NetfsFsDriver *candidate = netfs_fs_drivers[i];

	if (!candidate->implemented || candidate->open == NULL)
	    continue;
	if (candidate->open(mount) == 0)
	    driver = candidate;
    }

    if (driver == NULL) {
	if (backend->fini != NULL)
	    backend->fini(mount);
	return -ENODEV;
    }
    mount->fs = driver;
    mount->base_ops = &driver->dir;
    pthread_mutex_init(&mount->lock, NULL);
    mount->lock_ready = true;
    mount->ops = &locking_ops;

    /* Register the image, then serve every other process that wants it;
     * the containers of this process share this very mount.  */
    {
	const char *image = netfs_image_path(url);

	if (image != NULL) {
	    struct stat st;

	    if (stat(image, &st) == 0) {
		mount->image_dev = st.st_dev;
		mount->image_ino = st.st_ino;
		mount->image_path = talloc_strdup(mount, image);
		netfs_image_register(st.st_dev, st.st_ino, mount);
		mount->registered = true;
		netfs_cache_publish(mount);
	    }
	    (void) netfs_share_serve(mount, image);
	}
    }

    /*
     * The host kernel never sees the files inside a block backend, so
     * virtual ownership/permissions cannot be delegated to it: vperm is
     * mandatory there and its metadata lives in the image itself (see
     * fs_ext2.c), not in .uvroot-vperm.
     */
    if (vperm_enable(tracee) < 0) {
	if (driver->close != NULL)
	    driver->close(mount);
	if (backend->fini != NULL)
	    backend->fini(mount);
	return -1;
    }

    return 0;
}

bool netfs_block_root_lacks(Tracee *tracee, const char *rel)
{
    Extension *extension = get_extension(tracee, netfs_callback);
    NetfsMount *mount;
    const NetfsDirOps *ops;
    struct stat st;

    if (extension == NULL || extension->config == NULL)
	return false;

    for (mount = config_of(extension)->mounts; mount != NULL;
	 mount = mount->next) {
	if (mount->backend == NULL
	    || mount->backend->kind != NETFS_KIND_BLOCK)
	    continue;
	if (mount->guest == NULL || strcmp(mount->guest, "/") != 0)
	    continue;

	ops = dir_ops_of(mount);
	if (ops == NULL || ops->stat == NULL)
	    return false;
	return ops->stat(mount, rel, &st) < 0;
    }
    return false;
}

/*
 * Take the image over: open it here and serve the other containers.
 * Used when the process that owned it exited while this one was still
 * using it, so that the virtual I/O never stops before every container
 * is gone.
 */
int netfs_open_image(struct tracee *tracee, NetfsMount *mount)
{
    int status;

    if (mount == NULL || mount->backend == NULL)
	return -EINVAL;
    if (mount->backend->kind != NETFS_KIND_BLOCK)
	return -EINVAL;

    status = open_block_mount(tracee, mount, mount->url);
    if (status < 0)
	return status;

    /* The cached tree came from the service; rebuild it from the
     * filesystem we now drive ourselves.  */
    mount->root.listed = false;
    mount->root.listed_at = 0;

    VERBOSE(tracee, 1, "netfs: took over %s", mount->display);
    return 0;
}

/*
 * Ownership is identity-aware: as soon as an id0 process exists it owns
 * the image, whichever process opened it first.  A client that is id0
 * asks the current owner to step down, then takes its place.
 */
void netfs_finalize(Tracee *tracee)
{
    Extension *extension = get_extension(tracee, netfs_callback);
    NetfsConfig *config;
    NetfsMount *mount;
    uid_t uid;

    if (extension == NULL || extension->config == NULL)
	return;

    config = config_of(extension);
    uid = vperm_initial_uid(tracee);

    /*
     * --read-only / --ro may have been given before or after the mounts
     * were created, so settle that here: all of them, or only those the
     * user named.
     */
    {
	NetfsMount *item;

	for (item = config->mounts; item != NULL; item = item->next) {
	    size_t i;

	    if (config->read_only || tracee->read_only.all) {
		item->read_only = true;
		continue;
	    }
	    for (i = 0; i < tracee->read_only.count; i++) {
		if (item->guest != NULL
		    && strcmp(item->guest,
			      tracee->read_only.paths[i]) == 0) {
		    item->read_only = true;
		    break;
		}
	    }
	}
    }

    for (mount = config->mounts; mount != NULL; mount = mount->next) {
	const char *image;

	if (mount->backend == NULL || mount->backend->kind != NETFS_KIND_BLOCK)
	    continue;
	image = netfs_image_path(mount->url);
	if (image == NULL)
	    continue;

	if (mount->share_server != NULL) {
	    /* This process owns the image; tell the service who it is.  */
	    netfs_share_set_owner_identity(mount, uid);
	    continue;
	}

	if (mount->share_ops == NULL || uid != 0)
	    continue;

	/*
	 * Handing the image over from one process to another is not
	 * reliable yet (the transfer window is visible to the owner and
	 * to its other clients), so it stays opt-in until the images are
	 * managed by a single process.  See the design note.
	 */
	if (getenv("UVROOT_NETFS_TAKEOVER") == NULL)
	    continue;

	{
	    uid_t owner = 0;

	    if (netfs_share_owner_uid(mount, &owner) == 0 && owner != 0
		&& netfs_share_request_takeover(mount) == 0) {
		int attempt;

		/* The former owner released the image: take it over.  */
		TALLOC_FREE(mount->share_data);
		mount->share_ops = NULL;

		/* The local mirror was filled from the service; it has to
		 * be rebuilt from the filesystem we now own.  */
		mount->root.child = NULL;
		mount->root.listed = false;
		mount->root.materialized = false;
		mount->root.fetched = false;
		mount->root.listed_at = 0;

		for (attempt = 0; attempt < 100; attempt++) {
		    if (open_block_mount(tracee, mount, mount->url) == 0)
			break;
		    usleep(100000);
		}
	    }
	}
    }
}

/*
 * Make every netfs mount of @tracee read-only.  Enforced where the
 * changes are applied, not by the permission layer, so that the virtual
 * root cannot write either.
 */
volatile sig_atomic_t netfs_interrupt = 0;

void netfs_clear_interrupt(void)
{
    netfs_interrupt = 0;
}

int netfs_set_read_only(Tracee *tracee)
{
    Extension *extension = get_extension(tracee, netfs_callback);

    if (extension == NULL) {
	if (initialize_extension(tracee, netfs_callback, NULL) < 0)
	    return -1;
	extension = get_extension(tracee, netfs_callback);
	if (extension == NULL)
	    return -1;
    }

    config_of(extension)->read_only = true;
    return 0;
}

int netfs_add_mount(Tracee *tracee, const char *spec, const char *forced_scheme)
{
    Extension *extension;
    NetfsConfig *config;
    const NetfsBackend *backend;
    NetfsMount *mount;
    char *guest = NULL;
    char *url = NULL;
    const char *scheme;
    const char *cache_root;
    char cache[PATH_MAX];
    bool open_locked = false;
    int status;

    if (spec == NULL || spec[0] == '\0') {
	note(tracee, ERROR, USER, "netfs: empty mount specification");
	return -1;
    }

    extension = get_extension(tracee, netfs_callback);
    if (extension == NULL) {
	status = initialize_extension(tracee, netfs_callback, NULL);
	if (status < 0)
	    return status;
	extension = get_extension(tracee, netfs_callback);
	if (extension == NULL)
	    return -1;
    }
    config = config_of(extension);

    status = parse_spec(config, spec, &guest, &url);
    if (status < 0) {
	note(tracee, ERROR, USER, "netfs: invalid mount specification \"%s\"",
	     spec);
	return -1;
    }

    scheme = scheme_of(url);
    if (scheme == NULL)
	scheme = forced_scheme;

    backend = netfs_find_backend(scheme);
    if (backend == NULL) {
	note(tracee, ERROR, USER,
	     "netfs: no backend for \"%s\" (specification \"%s\")",
	     scheme != NULL ? scheme : url, spec);
	return -1;
    }

    if (!backend->implemented) {
	note(tracee, ERROR, USER,
	     "netfs: the \"%s\" backend is not available in this build%s%s",
	     backend->name,
	     backend->reserved_note != NULL ? ": " : "",
	     backend->reserved_note != NULL ? backend->reserved_note : "");
	return -1;
    }

    cache_root = netfs_cache_root();
    if (cache_root == NULL) {
	note(tracee, ERROR, SYSTEM, "netfs: cannot create the cache directory");
	return -1;
    }

    if (guest == NULL) {
	char candidate[PATH_MAX];
	const char *base = default_guest_for(backend);
	unsigned int suffix = 0;

	snprintf(candidate, sizeof(candidate), "%s", base);
	while (guest_is_used(config, candidate))
	    snprintf(candidate, sizeof(candidate), "%s-%u", base, ++suffix);

	guest = talloc_strdup(config, candidate);
	if (guest == NULL)
	    return -1;
    }

    /*
     * FTP and SMB cannot represent a POSIX tree faithfully: neither
     * protocol carries symlinks, ownership and the full mode bits in a
     * portable way, and listings disagree between servers.  A directory
     * tree served by them is fine as a data mount, but it must not be
     * used as the guest root, where a single missing symlink (Alpine's
     * /bin/sh -> /bin/busybox, for instance) breaks the whole system.
     * Block-level sources carry a real filesystem and are the supported
     * way to boot from remote storage.
     */
    if (strcmp(guest, "/") == 0 && backend->kind == NETFS_KIND_DIR
	&& getenv("UVROOT_NETFS_ALLOW_REMOTE_ROOT") == NULL) {
	note(tracee, ERROR, USER,
	     "netfs: %s cannot be used as the guest root: the protocol does "
	     "not carry symlinks, ownership and permission bits faithfully. "
	     "Use a block backend (img, qcow2, iscsi, nbd) instead, or set "
	     "UVROOT_NETFS_ALLOW_REMOTE_ROOT=1 to override.",
	     backend->name);
	return -1;
    }

    snprintf(cache, sizeof(cache), "%s/m%u", cache_root, config->next_mount_id++);
    if (mkdir(cache, 0700) < 0 && errno != EEXIST) {
	note(tracee, ERROR, SYSTEM, "netfs: cannot create \"%s\": %s",
	     cache, strerror(errno));
	return -1;
    }

    /*
     * new_binding() canonicalizes the host path, and the very same
     * canonical form is what HOST_PATH reports later on.  Resolve the
     * cache directory now so the two always match, even when TMPDIR or
     * HOME goes through a symbolic link.
     */
    {
	char resolved[PATH_MAX];

	if (realpath(cache, resolved) != NULL)
	    snprintf(cache, sizeof(cache), "%s", resolved);
    }

    mount = talloc_zero(config, NetfsMount);
    if (mount == NULL)
	return -1;
    talloc_set_destructor(mount, release_mount);

    mount->backend = backend;
    mount->tracee = tracee;
    mount->read_only = config->read_only;
    mount->scheme = talloc_strdup(mount, scheme);
    mount->url = talloc_strdup(mount, url);
    mount->guest = talloc_strdup(mount, guest);
    mount->cache = talloc_strdup(mount, cache);
    mount->display = talloc_zero_size(mount, PATH_MAX);
    if (mount->scheme == NULL || mount->url == NULL || mount->guest == NULL
	|| mount->cache == NULL || mount->display == NULL)
	return -1;
    make_display_url(mount->display, PATH_MAX, url);

    mount->root.is_dir = true;
    mount->root.materialized = true;
    mount->root.mount = mount;

    /*
     * A block image may already be owned by another uvroot process: the
     * filesystem inside it has a single driver, so it is served by its
     * owner and every other process forwards its operations there.
     */
    if (backend->kind == NETFS_KIND_BLOCK) {
	const char *image = netfs_image_path(url);

	/*
	 * Opening an image is serialized inside the process: a container
	 * may otherwise find neither the registry entry nor the service
	 * of another container that is registering the very same image,
	 * and end up opening it a second time.
	 */
	if (container_is_multi()) {
	    pthread_mutex_lock(&netfs_open_lock);
	    open_locked = true;
	}

	/*
	 * Another container of this process may already drive that very
	 * image: then this one borrows its mount, and with it the same
	 * driver, the same lock and the same mirror, so both containers
	 * see each other's writes at once.  Everything is under one
	 * address space; no socket is involved.
	 */
	if (image != NULL) {
	    struct stat image_stat;
	    char resolved[PATH_MAX];
	    char *nested;

	    memset(&image_stat, 0, sizeof(image_stat));

	    /* An image living inside an already mounted one would be
	     * written by two drivers at once.  */
	    if (realpath(image, resolved) == NULL)
		snprintf(resolved, sizeof(resolved), "%s", image);
	    nested = netfs_containing_cache(mount, resolved);
	    if (nested == NULL && stat(resolved, &image_stat) == 0)
		nested = netfs_sharing_blocks(mount, resolved,
					      image_stat.st_dev,
					      image_stat.st_ino);
	    if (nested != NULL) {
		note(tracee, ERROR, USER,
		     "netfs: \"%s\" and \"%s\", which is already mounted, "
		     "share disk blocks (or the first one lives inside the "
		     "second); mounting both as block devices would corrupt "
		     "them.  Copy the image first.", image, nested);
		TALLOC_FREE(mount);
		return -1;
	    }

	    if (stat(image, &image_stat) == 0) {
		NetfsMount *owner =
		    netfs_image_acquire(image_stat.st_dev, image_stat.st_ino);

		if (owner != NULL) {
		    char *shared_cache = talloc_strdup(mount, owner->cache);

		    if (shared_cache == NULL) {
			netfs_image_release(owner);
			if (open_locked)
			    pthread_mutex_unlock(&netfs_open_lock);
			TALLOC_FREE(mount);
			return -1;
		    }
		    /* Same driver, same lock, same mirror as the
		     * container that opened the image.  */
		    mount->cache = shared_cache;
		    mount->redirect = owner;
		    mount->ops = &redirect_ops;

		    if (vperm_enable(tracee) < 0) {
			note(tracee, ERROR, USER,
			     "netfs: the virtual permission layer is required "
			     "for %s", mount->display);
			TALLOC_FREE(mount);
			return -1;
		    }
		    goto bind;
		}
	    }
	}

	if (image != NULL && netfs_share_connect(mount, image) == 0) {
	    if (vperm_enable(tracee) < 0) {
		note(tracee, ERROR, USER,
		     "netfs: the virtual permission layer is required "
		     "for %s", mount->display);
		TALLOC_FREE(mount);
		return -1;
	    }
	    goto bind;
	}
    }

    if (backend->kind == NETFS_KIND_BLOCK) {
	status = open_block_mount(tracee, mount, url);
	if (open_locked) {
	    pthread_mutex_unlock(&netfs_open_lock);
	    open_locked = false;
	}
	if (status < 0) {
	    if (status == -ENODEV)
		note(tracee, ERROR, USER,
		     "netfs: no user-space filesystem driver could read %s "
		     "(supported: ext2/3/4)", mount->display);
	    else
		note(tracee, ERROR, USER, "netfs: cannot reach %s: %s",
		     mount->display, strerror(-status));
	    TALLOC_FREE(mount);
	    return -1;
	}
    } else if (backend->init != NULL) {
	status = backend->init(mount, url);
	if (status < 0) {
	    note(tracee, ERROR, USER, "netfs: cannot reach %s: %s",
		 mount->display, strerror(-status));
	    TALLOC_FREE(mount);
	    return -1;
	}
    }

  bind:
    if (open_locked) {
	pthread_mutex_unlock(&netfs_open_lock);
	open_locked = false;
    }
    if (new_binding(tracee, mount->cache, mount->guest, true) == NULL) {
	note(tracee, WARNING, INTERNAL, "netfs: cannot bind \"%s\" into \"%s\"",
	     mount->cache, mount->guest);
	if (mount->backend_data != NULL && backend->fini != NULL)
	    backend->fini(mount);
	TALLOC_FREE(mount);
	return -1;
    }

    mount->next = config->mounts;
    config->mounts = mount;

    note(tracee, INFO, USER, "netfs: %s mounted on %s", mount->display,
	 mount->guest);

    return 0;
}
