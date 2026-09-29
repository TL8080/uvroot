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
 */

/*
 * Shared image service.
 *
 * The filesystem inside a block image has exactly one driver: the first
 * uvroot process that opens the image.  Any other process that wants the
 * same image connects to it and forwards NetfsDirOps calls, so there is
 * a single copy of the block/inode bitmaps and concurrent access from
 * several containers (or several virtual ids) cannot corrupt it.
 */

#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>

#include "cli/note.h"
#include "extension/netfs/netfs.h"
#include "extension/netfs/share.h"

/* ------------------------------------------------------------------ */
/* Wire format                                                         */
/* ------------------------------------------------------------------ */

enum {
    SHARE_LIST = 1,
    SHARE_STAT,
    SHARE_GET,
    SHARE_PUT,
    SHARE_MKDIR,
    SHARE_RMDIR,
    SHARE_UNLINK,
    SHARE_RENAME,
    SHARE_SYMLINK,
    SHARE_LINK,
    SHARE_CHMOD,
    SHARE_CHOWN,
    SHARE_OWNER,		/* who owns the image? */
    SHARE_TAKEOVER,		/* an id0 client takes over */
};

typedef struct ShareHead {
    uint32_t op;
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;
    int32_t new_uid;
    int32_t new_gid;
    uint64_t size;		/* PUT: content length */
    uint32_t rel_len;
    uint32_t rel2_len;
} ShareHead;

typedef struct ShareReply {
    int32_t status;
    uint32_t count;		/* LIST: number of entries */
    uint64_t size;		/* GET: content length */
} ShareReply;

typedef struct ShareDirent {
    uint32_t mode;
    uint32_t is_dir;
    uint32_t is_link;
    int64_t size;
    int64_t mtime;
    uint32_t name_len;
    uint32_t target_len;
} ShareDirent;

static bool send_all(int fd, const void *data, size_t size)
{
    const char *cursor = data;

    while (size > 0) {
	ssize_t count = write(fd, cursor, size);

	if (count <= 0)
	    return false;
	cursor += count;
	size -= (size_t) count;
    }
    return true;
}

static bool recv_all(int fd, void *data, size_t size)
{
    char *cursor = data;

    while (size > 0) {
	ssize_t count = read(fd, cursor, size);

	if (count <= 0)
	    return false;
	cursor += count;
	size -= (size_t) count;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Naming                                                              */
/* ------------------------------------------------------------------ */

const char *netfs_image_path(const char *url)
{
    static const char *const schemes[] = {
	"img://", "raw://", "file://", "qcow2://", "qcow://",
	"qemu+img://", NULL
    };
    size_t i;

    for (i = 0; schemes[i] != NULL; i++) {
	size_t length = strlen(schemes[i]);

	if (strncmp(url, schemes[i], length) == 0)
	    return url + length;
    }
    return url;
}

/*
 * Directory holding the services of the current user.  $XDG_RUNTIME_DIR
 * already is private; otherwise a private directory is created under
 * /tmp.  Returns NULL when it cannot be trusted.
 */
const char *netfs_share_user_dir(void)
{
    static char directory[PATH_MAX];
    static bool ready;
    struct stat st;
    const char *runtime = getenv("XDG_RUNTIME_DIR");

    if (ready)
	return directory[0] != '\0' ? directory : NULL;

    ready = true;
    if (runtime != NULL && runtime[0] == '/') {
	snprintf(directory, sizeof(directory), "%s", runtime);
	return directory;
    }

    snprintf(directory, sizeof(directory), "/tmp/.uvroot-share-%u",
	     (unsigned) getuid());
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
	directory[0] = '\0';
	return NULL;
    }
    /* Refuse anything not owned by us and not private: another user
     * could otherwise read or replace our sockets.  */
    if (lstat(directory, &st) < 0 || st.st_uid != getuid()
	|| !S_ISDIR(st.st_mode) || (st.st_mode & 077) != 0) {
	directory[0] = '\0';
	return NULL;
    }
    return directory;
}

/*
 * The service is named after the *backing storage*, not the path: two
 * names for the same file (a symlink, a hard link, a different relative
 * path, a bind mount) must resolve to the same owner, otherwise they
 * would each drive the filesystem and corrupt it.  Transports without a
 * local file (NBD, iSCSI) keep their URL as the identity.
 */
static void share_paths(const char *image, char *socket_path, size_t socket_size,
			char *lock_path, size_t lock_size)
{
    char key[PATH_MAX + 64];
    struct stat st;
    const char *base;
    unsigned long hash = 5381;
    const char *cursor;

    if (stat(image, &st) == 0)
	snprintf(key, sizeof(key), "blk:%llu:%llu",
		 (unsigned long long) st.st_dev,
		 (unsigned long long) st.st_ino);
    else {
	char resolved[PATH_MAX];

	if (realpath(image, resolved) == NULL)
	    snprintf(resolved, sizeof(resolved), "%s", image);
	snprintf(key, sizeof(key), "url:%s", resolved);
    }

    for (cursor = key; *cursor != '\0'; cursor++)
	hash = ((hash << 5) + hash) ^ (unsigned char) *cursor;

    /*
     * Services are strictly per host user: a container started by one
     * user must never be reachable from another user's uvroot, even
     * though both may use the same image file.  The socket and the lock
     * live in a directory only the current user can enter, and their
     * names carry the uid as well.
     */
    base = netfs_share_user_dir();
    if (base == NULL)
	base = "/tmp";

    snprintf(socket_path, socket_size, "%s/.uvroot-share-%u-%lx.sock",
	     base, (unsigned) getuid(), hash);
    snprintf(lock_path, lock_size, "%s/.uvroot-share-%u-%lx.lock",
	     base, (unsigned) getuid(), hash);
}

/* ------------------------------------------------------------------ */
/* Client side                                                         */
/* ------------------------------------------------------------------ */

typedef struct ShareClient {
    int fd;
} ShareClient;

/*
 * The process that owned the image may have exited while this one was
 * still using it.  The virtual I/O must not stop before every container
 * is closed, so either another user has taken over already (reconnect
 * to it) or this one takes over (open the image and serve it).
 */
static int netfs_share_recover(NetfsMount *mount)
{
    const char *image = mount->url != NULL ? netfs_image_path(mount->url)
	: NULL;
    int attempt;

    if (image == NULL)
	return -EIO;

    if (mount->share_data != NULL) {
	ShareClient *client = mount->share_data;

	if (client->fd >= 0)
	    close(client->fd);
	TALLOC_FREE(mount->share_data);
    }
    mount->share_ops = NULL;

    for (attempt = 0; attempt < 100; attempt++) {
	if (netfs_share_connect(mount, image) == 0)
	    return 0;		/* someone else owns it now */
	if (netfs_open_image(mount->tracee, mount) == 0)
	    return 0;		/* we own it now */
	usleep(100000);
    }

    note(NULL, ERROR, USER,
	 "netfs: the image \"%s\" is gone and could not be reopened", image);
    return -EIO;
}

static int share_call_once(NetfsMount *mount, ShareHead *head,
		      const char *rel, const char *rel2,
		      const void *payload, size_t payload_size,
		      ShareReply *reply, void *out, size_t out_size)
{
    ShareClient *client = mount->share_data;
    ShareHead request = *head;
    size_t rel_len = rel != NULL ? strlen(rel) : 0;
    size_t rel2_len = rel2 != NULL ? strlen(rel2) : 0;

    request.rel_len = (uint32_t) rel_len;
    request.rel2_len = (uint32_t) rel2_len;
    request.uid = mount->create_identity ? (uint32_t) mount->create_uid
	: (uint32_t) getuid();
    request.gid = mount->create_identity ? (uint32_t) mount->create_gid
	: (uint32_t) getgid();

    if (client == NULL || client->fd < 0)
	return -ENOTCONN;

    if (!send_all(client->fd, &request, sizeof(request)))
	return -EIO;
    if (rel_len > 0 && !send_all(client->fd, rel, rel_len))
	return -EIO;
    if (rel2_len > 0 && !send_all(client->fd, rel2, rel2_len))
	return -EIO;
    if (payload_size > 0 && !send_all(client->fd, payload, payload_size))
	return -EIO;

    if (!recv_all(client->fd, reply, sizeof(*reply)))
	return -EIO;
    if (out_size > 0 && !recv_all(client->fd, out, out_size))
	return -EIO;

    return 0;
}

static int share_call(NetfsMount *mount, ShareHead *head,
		      const char *rel, const char *rel2,
		      const void *payload, size_t payload_size,
		      ShareReply *reply, void *out, size_t out_size)
{
    int status = share_call_once(mount, head, rel, rel2, payload,
				 payload_size, reply, out, out_size);

    if ((status == -EIO || status == -ENOTCONN)
	&& netfs_share_recover(mount) == 0) {
	/* The request is replayed on the new connection.  */
	status = share_call_once(mount, head, rel, rel2, payload,
				 payload_size, reply, out, out_size);
    }
    return status;
}

static int share_remote_list_once(NetfsMount *mount, const char *rel,
			     NetfsDirent **entries, size_t *count,
			     TALLOC_CTX *ctx)
{
    ShareHead head = { .op = SHARE_LIST, .uid = 0,
		       .gid = 0 };
    ShareReply reply;
    NetfsDirent *list = NULL;
    uint32_t i;
    int status;

    status = share_call(mount, &head, rel, NULL, NULL, 0, &reply, NULL, 0);
    if (status < 0)
	return status;
    if (reply.status < 0)
	return reply.status;

    if (reply.count > 0) {
	list = talloc_zero_array(ctx, NetfsDirent, reply.count);
	if (list == NULL)
	    return -ENOMEM;
    }

    for (i = 0; i < reply.count; i++) {
	ShareDirent wire;
	char *name;
	char *target = NULL;

	if (!recv_all(((ShareClient *) mount->share_data)->fd, &wire,
		      sizeof(wire)))
	    return -EIO;

	name = talloc_size(ctx, wire.name_len + 1);
	if (name == NULL)
	    return -ENOMEM;
	if (wire.name_len > 0
	    && !recv_all(((ShareClient *) mount->share_data)->fd, name,
			 wire.name_len))
	    return -EIO;
	name[wire.name_len] = '\0';

	if (wire.target_len > 0) {
	    target = talloc_size(ctx, wire.target_len + 1);
	    if (target == NULL)
		return -ENOMEM;
	    if (!recv_all(((ShareClient *) mount->share_data)->fd, target,
			  wire.target_len))
		return -EIO;
	    target[wire.target_len] = '\0';
	}

	list[i].name = name;
	list[i].target = target;
	list[i].is_dir = wire.is_dir != 0;
	list[i].is_link = wire.is_link != 0;
	list[i].size = (off_t) wire.size;
	list[i].mtime = (time_t) wire.mtime;
	list[i].mode = (mode_t) wire.mode;
    }

    *entries = list;
    *count = reply.count;
    return 0;
}

static int share_remote_stat(NetfsMount *mount, const char *rel,
			     struct stat *st)
{
    ShareHead head = { .op = SHARE_STAT, .uid = 0,
		       .gid = 0 };
    ShareReply reply;
    int status = share_call(mount, &head, rel, NULL, NULL, 0, &reply, NULL, 0);

    if (status < 0)
	return status;
    if (reply.status < 0)
	return reply.status;
    if (!recv_all(((ShareClient *) mount->share_data)->fd, st, sizeof(*st)))
	return -EIO;
    return 0;
}

static int share_remote_get_once(NetfsMount *mount, const char *rel,
			    const char *local)
{
    ShareHead head = { .op = SHARE_GET, .uid = 0,
		       .gid = 0 };
    ShareReply reply;
    FILE *file;
    char buffer[65536];
    uint64_t left;
    int status = share_call(mount, &head, rel, NULL, NULL, 0, &reply, NULL, 0);

    if (status < 0)
	return status;
    if (reply.status < 0)
	return reply.status;

    file = fopen(local, "wb");
    if (file == NULL)
	return -errno;

    left = reply.size;
    while (left > 0) {
	size_t chunk = left < sizeof(buffer) ? (size_t) left : sizeof(buffer);

	if (!recv_all(((ShareClient *) mount->share_data)->fd, buffer, chunk)) {
	    fclose(file);
	    return -EIO;
	}
	if (fwrite(buffer, 1, chunk, file) != chunk) {
	    int saved = -errno;

	    fclose(file);
	    return saved;
	}
	left -= chunk;
    }

    fclose(file);
    return 0;
}

static int share_remote_put_once(NetfsMount *mount, const char *rel,
			    const char *local)
{
    ShareHead head = { .op = SHARE_PUT, .uid = 0,
		       .gid = 0 };
    ShareReply reply;
    struct stat st;
    FILE *file;
    char buffer[65536];
    uint64_t total = 0;
    int status;

    if (stat(local, &st) == 0)
	head.size = (uint64_t) st.st_size;
    else
	head.size = 0;

    /* The payload is streamed right after the request: send the header
     * first, then the content, then read the reply.  */
    {
	ShareClient *client = mount->share_data;
	ShareHead request = head;
	size_t rel_len = strlen(rel);

	if (client == NULL || client->fd < 0)
	    return -ENOTCONN;
	request.rel_len = (uint32_t) rel_len;
	request.rel2_len = 0;
	request.uid = mount->create_identity ? (uint32_t) mount->create_uid
	    : (uint32_t) getuid();
	request.gid = mount->create_identity ? (uint32_t) mount->create_gid
	    : (uint32_t) getgid();
	if (!send_all(client->fd, &request, sizeof(request)))
	    return -EIO;
	if (!send_all(client->fd, rel, rel_len))
	    return -EIO;
    }

    file = fopen(local, "rb");
    if (file == NULL)
	return -errno;
    for (;;) {
	size_t got = fread(buffer, 1, sizeof(buffer), file);

	if (got == 0)
	    break;
	if (!send_all(((ShareClient *) mount->share_data)->fd, buffer, got)) {
	    fclose(file);
	    return -EIO;
	}
	total += got;
    }
    fclose(file);
    (void) total;

    if (!recv_all(((ShareClient *) mount->share_data)->fd, &reply,
		  sizeof(reply)))
	return -EIO;
    status = reply.status;
    return status;
}

static int share_remote_simple(NetfsMount *mount, uint32_t op, const char *rel,
			       const char *rel2, uint32_t mode,
			       int32_t new_uid, int32_t new_gid)
{
    ShareHead head = { .op = op, .uid = 0,
		       .gid = 0, .mode = mode,
		       .new_uid = new_uid, .new_gid = new_gid };
    ShareReply reply;
    int status = share_call(mount, &head, rel, rel2, NULL, 0, &reply, NULL, 0);

    if (status < 0)
	return status;
    return reply.status;
}

static int share_remote_mkdir(NetfsMount *mount, const char *rel)
{
    return share_remote_simple(mount, SHARE_MKDIR, rel, NULL, 0, -1, -1);
}

static int share_remote_rmdir(NetfsMount *mount, const char *rel)
{
    return share_remote_simple(mount, SHARE_RMDIR, rel, NULL, 0, -1, -1);
}

static int share_remote_unlink(NetfsMount *mount, const char *rel)
{
    return share_remote_simple(mount, SHARE_UNLINK, rel, NULL, 0, -1, -1);
}

static int share_remote_rename(NetfsMount *mount, const char *from,
			       const char *to)
{
    return share_remote_simple(mount, SHARE_RENAME, from, to, 0, -1, -1);
}

static int share_remote_symlink(NetfsMount *mount, const char *rel,
				const char *target)
{
    return share_remote_simple(mount, SHARE_SYMLINK, rel, target, 0, -1, -1);
}

static int share_remote_link(NetfsMount *mount, const char *from,
			     const char *to)
{
    return share_remote_simple(mount, SHARE_LINK, from, to, 0, -1, -1);
}

static int share_remote_chmod(NetfsMount *mount, const char *rel, mode_t mode)
{
    return share_remote_simple(mount, SHARE_CHMOD, rel, NULL,
			       (uint32_t) mode, -1, -1);
}

static int share_remote_chown(NetfsMount *mount, const char *rel, uid_t uid,
			      gid_t gid)
{
    return share_remote_simple(mount, SHARE_CHOWN, rel, NULL, 0,
			       (int32_t) uid, (int32_t) gid);
}

/*
 * "list", "get" and "put" exchange payloads outside share_call(), so a
 * connection that dies in the middle is retried here after the image
 * has been recovered (reconnected or taken over).
 */
static int share_remote_list(NetfsMount *mount, const char *rel,
			     NetfsDirent **entries, size_t *count,
			     TALLOC_CTX *ctx)
{
    int status = share_remote_list_once(mount, rel, entries, count, ctx);

    if ((status == -EIO || status == -ENOTCONN)
	&& netfs_share_recover(mount) == 0)
	status = share_remote_list_once(mount, rel, entries, count, ctx);
    return status;
}

static int share_remote_get(NetfsMount *mount, const char *rel,
			    const char *local)
{
    int status = share_remote_get_once(mount, rel, local);

    if ((status == -EIO || status == -ENOTCONN)
	&& netfs_share_recover(mount) == 0)
	status = share_remote_get_once(mount, rel, local);
    return status;
}

static int share_remote_put(NetfsMount *mount, const char *rel,
			    const char *local)
{
    int status = share_remote_put_once(mount, rel, local);

    if ((status == -EIO || status == -ENOTCONN)
	&& netfs_share_recover(mount) == 0)
	status = share_remote_put_once(mount, rel, local);
    return status;
}

static const NetfsDirOps share_remote_ops = {
    .list = share_remote_list,
    .stat = share_remote_stat,
    .get = share_remote_get,
    .put = share_remote_put,
    .mkdir = share_remote_mkdir,
    .rmdir = share_remote_rmdir,
    .unlink = share_remote_unlink,
    .rename = share_remote_rename,
    .symlink = share_remote_symlink,
    .link = share_remote_link,
    .chmod = share_remote_chmod,
    .chown = share_remote_chown,
};

/* Connect once; a stale socket is reported as "no service".  */
static int share_connect_once(const char *socket_path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
	return -errno;

    if (strlen(socket_path) >= sizeof(address.sun_path)) {
	close(fd);
	return -ENAMETOOLONG;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);

    if (connect(fd, (struct sockaddr *) &address, sizeof(address)) < 0) {
	int saved = errno;

	close(fd);
	return (saved == ECONNREFUSED || saved == ENOENT) ? -ENOENT : -saved;
    }
    return fd;
}

int netfs_share_connect(NetfsMount *mount, const char *image)
{
    char socket_path[PATH_MAX];
    char lock_path[PATH_MAX];
    ShareClient *client;
    int fd;
    int attempt;

    share_paths(image, socket_path, sizeof(socket_path),
		lock_path, sizeof(lock_path));

    fd = share_connect_once(socket_path);
    if (fd < 0) {
	int lock_fd = open(lock_path, O_RDONLY | O_CLOEXEC);

	/* Somebody else owns the image but its socket is not ready yet
	 * (both processes started at the same time): wait for it.  */
	if (lock_fd >= 0) {
	    bool busy = flock(lock_fd, LOCK_EX | LOCK_NB) < 0
		&& errno == EWOULDBLOCK;

	    close(lock_fd);
	    if (busy) {
		for (attempt = 0; attempt < 50; attempt++) {
		    usleep(100000);
		    fd = share_connect_once(socket_path);
		    if (fd >= 0)
			break;
		}
	    }
	}
	if (fd < 0)
	    return -ENOENT;
    }

    client = talloc_zero(mount, ShareClient);
    if (client == NULL) {
	close(fd);
	return -ENOMEM;
    }
    client->fd = fd;

    mount->share_data = client;
    mount->share_ops = &share_remote_ops;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Server side                                                         */
/* ------------------------------------------------------------------ */

typedef struct ShareServer {
    NetfsMount *mount;
    const NetfsDirOps *local;
    const char *image;
    uid_t owner_uid;		/* virtual identity of this owner */
    int fd;
    int lock_fd;
    const char *socket_path;
    pthread_mutex_t lock;
} ShareServer;

static void share_server_list(ShareServer *server, int fd, ShareHead *head UNUSED,
			      const char *rel)
{
    NetfsDirent *entries = NULL;
    size_t count = 0;
    ShareReply reply;
    uint32_t i;
    int status;

    status = server->local->list(server->mount, rel != NULL ? rel : "",
				 &entries, &count, server->mount);
    if (status < 0) {
	reply.status = status;
	reply.count = 0;
	reply.size = 0;
	(void) send_all(fd, &reply, sizeof(reply));
	return;
    }

    reply.status = 0;
    reply.count = (uint32_t) count;
    reply.size = 0;
    if (!send_all(fd, &reply, sizeof(reply)))
	return;

    for (i = 0; i < count; i++) {
	ShareDirent wire;
	size_t name_len = entries[i].name != NULL ? strlen(entries[i].name) : 0;
	size_t target_len =
	    entries[i].target != NULL ? strlen(entries[i].target) : 0;

	memset(&wire, 0, sizeof(wire));
	wire.mode = (uint32_t) entries[i].mode;
	wire.is_dir = entries[i].is_dir ? 1 : 0;
	wire.is_link = entries[i].is_link ? 1 : 0;
	wire.size = (int64_t) entries[i].size;
	wire.mtime = (int64_t) entries[i].mtime;
	wire.name_len = (uint32_t) name_len;
	wire.target_len = (uint32_t) target_len;

	if (!send_all(fd, &wire, sizeof(wire)))
	    return;
	if (name_len > 0 && !send_all(fd, entries[i].name, name_len))
	    return;
	if (target_len > 0 && !send_all(fd, entries[i].target, target_len))
	    return;
    }
}

/* The relative path of the request, or NULL.  */
static char *share_read_rel(int fd, uint32_t length)
{
    char *rel;

    if (length == 0)
	return NULL;
    rel = malloc(length + 1);
    if (rel == NULL)
	return NULL;
    if (!recv_all(fd, rel, length)) {
	free(rel);
	return NULL;
    }
    rel[length] = '\0';
    return rel;
}

static void share_dispatch(ShareServer *server, int fd, ShareHead *head,
			   const char *rel, const char *rel2)
{
    ShareReply reply;
    NetfsMount *mount = server->mount;
    int status = -ENOSYS;

    memset(&reply, 0, sizeof(reply));

    /* Entries created by a client belong to its virtual identity.  */
    pthread_mutex_lock(&server->lock);
    if ((head->op == SHARE_PUT || head->op == SHARE_MKDIR
	 || head->op == SHARE_SYMLINK || head->op == SHARE_LINK)
	&& (head->uid != 0 || head->gid != 0)) {
	mount->create_uid = (uid_t) head->uid;
	mount->create_gid = (gid_t) head->gid;
	mount->create_identity = true;
    }

    switch (head->op) {
    case SHARE_STAT:{
	struct stat st;

	status = server->local->stat(mount, rel != NULL ? rel : "", &st);
	if (status == 0) {
	    reply.status = 0;
	    if (!send_all(fd, &reply, sizeof(reply))) {
		pthread_mutex_unlock(&server->lock);
		return;
	    }
	    (void) send_all(fd, &st, sizeof(st));
	    pthread_mutex_unlock(&server->lock);
	    return;
	}
	break;
    }

    case SHARE_GET:{
	char temporary[] = "/tmp/uvroot-share-get-XXXXXX";
	struct stat st;
	int tmp_fd = mkstemp(temporary);

	if (tmp_fd < 0) {
	    status = -errno;
	    break;
	}
	close(tmp_fd);
	status = server->local->get(mount, rel != NULL ? rel : "", temporary);
	if (status == 0 && stat(temporary, &st) == 0) {
	    FILE *file = fopen(temporary, "rb");
	    char buffer[65536];

	    reply.status = 0;
	    reply.size = (uint64_t) st.st_size;
	    if (file == NULL || !send_all(fd, &reply, sizeof(reply))) {
		if (file != NULL)
		    fclose(file);
		unlink(temporary);
		status = -EIO;
		break;
	    }
	    for (;;) {
		size_t got = fread(buffer, 1, sizeof(buffer), file);

		if (got == 0)
		    break;
		if (!send_all(fd, buffer, got)) {
		    status = -EIO;
		    break;
		}
	    }
	    fclose(file);
	    unlink(temporary);
	    pthread_mutex_unlock(&server->lock);
	    return;
	}
	unlink(temporary);
	break;
    }

    case SHARE_PUT:{
	char temporary[] = "/tmp/uvroot-share-put-XXXXXX";
	int tmp_fd = mkstemp(temporary);
	uint64_t left = head->size;
	char buffer[65536];

	if (tmp_fd < 0) {
	    status = -errno;
	    break;
	}
	while (left > 0) {
	    size_t chunk = left < sizeof(buffer) ? (size_t) left : sizeof(buffer);

	    if (!recv_all(fd, buffer, chunk)) {
		status = -EIO;
		break;
	    }
	    if (write(tmp_fd, buffer, chunk) != (ssize_t) chunk) {
		status = -EIO;
		break;
	    }
	    left -= chunk;
	}
	close(tmp_fd);
	if (left == 0)
	    status = server->local->put(mount, rel != NULL ? rel : "",
					temporary);
	unlink(temporary);
	break;
    }

    case SHARE_MKDIR:
	status = server->local->mkdir(mount, rel != NULL ? rel : "");
	break;
    case SHARE_RMDIR:
	status = server->local->rmdir(mount, rel != NULL ? rel : "");
	break;
    case SHARE_UNLINK:
	status = server->local->unlink(mount, rel != NULL ? rel : "");
	break;
    case SHARE_RENAME:
	status = server->local->rename(mount, rel != NULL ? rel : "",
				       rel2 != NULL ? rel2 : "");
	break;
    case SHARE_SYMLINK:
	status = server->local->symlink(mount, rel != NULL ? rel : "",
					rel2 != NULL ? rel2 : "");
	break;
    case SHARE_LINK:
	status = server->local->link(mount, rel != NULL ? rel : "",
				     rel2 != NULL ? rel2 : "");
	break;
    case SHARE_CHMOD:
	status = server->local->chmod(mount, rel != NULL ? rel : "",
				      (mode_t) head->mode);
	break;
    case SHARE_CHOWN:
	status = server->local->chown(mount, rel != NULL ? rel : "",
				      (uid_t) head->new_uid,
				      (gid_t) head->new_gid);
	break;
    default:
	break;
    }

    pthread_mutex_unlock(&server->lock);

    reply.status = status;
    (void) send_all(fd, &reply, sizeof(reply));
}

/*
 * Ownership moved to an id0 process: release the image and become a
 * client of the new owner.  Only called from the serving thread, at a
 * point where no request of this owner is in flight.
 */
static void share_step_down(ShareServer *server)
{
    NetfsMount *mount = server->mount;
    const char *image = server->image;
    int attempt;

    close(server->fd);
    server->fd = -1;

    /* Remove our socket *before* releasing the image: the new owner
     * will bind that very path and must not have it removed under its
     * feet.  */
    unlink(server->socket_path);

    pthread_mutex_lock(&server->lock);
    if (mount->fs != NULL && mount->fs->close != NULL)
	mount->fs->close(mount);
    if (mount->backend != NULL && mount->backend->fini != NULL
	&& mount->backend_data != NULL)
	mount->backend->fini(mount);
    mount->fs = NULL;
    pthread_mutex_unlock(&server->lock);

    for (attempt = 0; attempt < 100; attempt++) {
	if (netfs_share_connect(mount, image) == 0) {
	    VERBOSE(NULL, 1, "netfs: became a client of the new owner");
	    return;
	}
	usleep(100000);
    }
    note(NULL, WARNING, USER, "netfs: cannot reconnect after the takeover");
}

static void share_serve_client(ShareServer *server, int fd)
{
    for (;;) {
	ShareHead head;
	char *rel;
	char *rel2;

	if (!recv_all(fd, &head, sizeof(head)))
	    break;
	rel = share_read_rel(fd, head.rel_len);
	rel2 = share_read_rel(fd, head.rel2_len);

	if (head.op == SHARE_OWNER) {
	    ShareReply reply = { .status = 0, .count = 0, .size = 0 };
	    uint32_t value = (uint32_t) server->owner_uid;

	    (void) send_all(fd, &reply, sizeof(reply));
	    (void) send_all(fd, &value, sizeof(value));
	    free(rel);
	    free(rel2);
	    continue;
	}

	if (head.op == SHARE_TAKEOVER) {
	    ShareReply reply;

	    /* Only an id0 process may take the image from another id.  */
	    if (head.uid == 0 && server->owner_uid != 0) {
		reply.status = 0;
		reply.count = 0;
		reply.size = 0;
		(void) send_all(fd, &reply, sizeof(reply));
		free(rel);
		free(rel2);
		share_step_down(server);
		return;		/* this server is done */
	    }
	    reply.status = -EPERM;
	    reply.count = 0;
	    reply.size = 0;
	    (void) send_all(fd, &reply, sizeof(reply));
	    free(rel);
	    free(rel2);
	    continue;
	}

	if (head.op == SHARE_LIST) {
	    pthread_mutex_lock(&server->lock);
	    share_server_list(server, fd, &head, rel);
	    pthread_mutex_unlock(&server->lock);
	} else {
	    share_dispatch(server, fd, &head, rel, rel2);
	}

	free(rel);
	free(rel2);
    }
    close(fd);
}

static void *share_accept_thread(void *data)
{
    ShareServer *server = data;

    for (;;) {
	int fd = accept(server->fd, NULL, NULL);

	if (fd < 0) {
	    if (errno == EINTR)
		continue;
	    break;
	}
	share_serve_client(server, fd);
    }
    return NULL;
}

int netfs_share_serve(NetfsMount *mount, const char *image)
{
    struct sockaddr_un address;
    char socket_path[PATH_MAX];
    char lock_path[PATH_MAX];
    ShareServer *server;
    pthread_t thread;
    int lock_fd;
    int fd;

    share_paths(image, socket_path, sizeof(socket_path),
		lock_path, sizeof(lock_path));

    /* Whoever wins this lock owns the image for the whole session.  */
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0)
	return -errno;
    if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
	close(lock_fd);
	return -EBUSY;
    }

    server = talloc_zero(mount, ShareServer);
    if (server == NULL) {
	close(lock_fd);
	return -ENOMEM;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
	close(lock_fd);
	return -errno;
    }

    if (strlen(socket_path) >= sizeof(address.sun_path)) {
	close(fd);
	close(lock_fd);
	return -ENAMETOOLONG;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    unlink(socket_path);

    if (bind(fd, (struct sockaddr *) &address, sizeof(address)) < 0
	|| listen(fd, 16) < 0) {
	close(fd);
	close(lock_fd);
	return -errno;
    }
    (void) chmod(socket_path, 0600);

    server->mount = mount;
    server->local = netfs_dir_ops_of(mount);
    server->image = talloc_strdup(server, image);
    server->owner_uid = (uid_t) -1;	/* set once the options are known */
    mount->share_server = server;
    server->fd = fd;
    server->lock_fd = lock_fd;
    server->socket_path = talloc_strdup(server, socket_path);
    pthread_mutex_init(&server->lock, NULL);

    if (pthread_create(&thread, NULL, share_accept_thread, server) != 0) {
	close(fd);
	close(lock_fd);
	return -EAGAIN;
    }
    pthread_detach(thread);

    VERBOSE(NULL, 1, "netfs: serving %s on %s", image, socket_path);
    return 0;
}

void netfs_share_set_owner_identity(NetfsMount *mount, uid_t uid)
{
    ShareServer *server = mount->share_server;

    if (server != NULL)
	server->owner_uid = uid;
}

static int share_send_head(NetfsMount *mount, ShareHead *head)
{
    ShareClient *client = mount->share_data;

    if (client == NULL || client->fd < 0)
	return -ENOTCONN;
    if (!send_all(client->fd, head, sizeof(*head)))
	return -EIO;
    return 0;
}

int netfs_share_owner_uid(NetfsMount *mount, uid_t *uid)
{
    ShareHead head;
    ShareReply reply;
    uint32_t value = 0;
    ShareClient *client = mount->share_data;

    if (client == NULL || client->fd < 0)
	return -ENOTCONN;

    memset(&head, 0, sizeof(head));
    head.op = SHARE_OWNER;
    if (share_send_head(mount, &head) < 0)
	return -EIO;
    if (!recv_all(client->fd, &reply, sizeof(reply)))
	return -EIO;
    if (reply.status < 0)
	return reply.status;
    if (!recv_all(client->fd, &value, sizeof(value)))
	return -EIO;

    *uid = (uid_t) value;
    return 0;
}

int netfs_share_request_takeover(NetfsMount *mount)
{
    ShareHead head;
    ShareReply reply;
    ShareClient *client = mount->share_data;

    if (client == NULL || client->fd < 0)
	return -ENOTCONN;

    memset(&head, 0, sizeof(head));
    head.op = SHARE_TAKEOVER;
    head.uid = 0;		/* the caller is the virtual root */
    head.gid = 0;
    if (share_send_head(mount, &head) < 0)
	return -EIO;
    if (!recv_all(client->fd, &reply, sizeof(reply)))
	return -EIO;

    return reply.status;
}
