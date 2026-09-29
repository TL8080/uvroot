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

#ifndef NETFS_SHARE_H
#define NETFS_SHARE_H

#include "extension/netfs/netfs.h"

/*
 * Several containers may want to use the same image at once.  Two
 * processes cannot both drive the filesystem inside it (each one keeps
 * its own block and inode bitmaps and would write them back on top of
 * the other), so the first process to open the image owns it and serves
 * the others: they send directory operations over a Unix socket instead
 * of touching the file.  The service is bound to the image path, so a
 * second `uvroot --img=...:disk.img` transparently shares the filesystem
 * with the first one instead of corrupting it.
 */

/* Private directory of the current user, where services and the cache
 * registry live; NULL when it cannot be trusted.  */
extern const char *netfs_share_user_dir(void);

/* Strip the scheme of @url and return the underlying image path.  */
extern const char *netfs_image_path(const char *url);

/*
 * Try to use an existing service for @image.  Returns 0 and installs
 * the remote operations when another process owns the image, -ENOENT
 * when there is no service yet, or -errno on error.
 */
extern int netfs_share_connect(NetfsMount * mount, const char *image);

/*
 * Become the owner of @image: create the socket and start serving
 * @mount in a background thread.  Returns 0 on success.
 */
extern int netfs_share_serve(NetfsMount * mount, const char *image);

/* Ownership is identity-aware: an id0 process always owns the image,
 * whichever process opened it first.  */
extern void netfs_share_set_owner_identity(NetfsMount * mount, uid_t uid);
extern int netfs_share_owner_uid(NetfsMount * mount, uid_t *uid);
extern int netfs_share_request_takeover(NetfsMount * mount);

#endif /* NETFS_SHARE_H */
