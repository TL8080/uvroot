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
 * Filesystem drivers that can be stacked on a block backend.  They are
 * reserved for future work: only the ext2/3/4 driver (fs_ext2.c) is
 * implemented so far.
 *
 * Block backends expose a byte-addressable device through NetfsBlockOps
 * and become directory backends once a NetfsFsDriver is stacked on top
 * of them.  The driver registry below is the extension point.
 */

#include <stddef.h>
#include <string.h>

#include "extension/netfs/netfs.h"

/*
 * Filesystem drivers that can be stacked on a block backend.  All of
 * them are reserved for now.
 */
static const NetfsFsDriver fs_driver_fat = {
    .name = "fat",
    .implemented = false,
    .reserved_note = "FAT/exFAT driver is planned",
};

static const NetfsFsDriver fs_driver_ntfs = {
    .name = "ntfs",
    .implemented = false,
    .reserved_note = "NTFS driver is planned",
};

const NetfsFsDriver *const netfs_fs_drivers[] = {
    &netfs_fs_driver_ext2,
    &fs_driver_fat,
    &fs_driver_ntfs,
    NULL,
};

const NetfsFsDriver *netfs_find_fs_driver(const char *name)
{
    size_t i;

    if (name == NULL)
	return NULL;

    for (i = 0; netfs_fs_drivers[i] != NULL; i++) {
	if (strcmp(netfs_fs_drivers[i]->name, name) == 0)
	    return netfs_fs_drivers[i];
    }

    return NULL;
}
