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

#ifndef VPID_H
#define VPID_H

#include "extension/extension.h"
#include "tracee/tracee.h"

/*
 * Virtual process ids: the first program of the container gets a chosen
 * pid (``--vpid=N``), its children get the following ones, and /proc is
 * presented from the virtual point of view -- /proc/<vpid> maps to the
 * real process, host pids are hidden, and the listings only show the
 * container's own pids.
 */

extern int vpid_callback(Extension *extension, ExtensionEvent event,
			 intptr_t d1, intptr_t d2);

/* Command-line entry points.  */
extern int vpid_set(Tracee *tracee, const char *value);	/* --vpid=N */
extern int vpid_enable(Tracee *tracee);			/* --vpid */

#endif				/* VPID_H */
