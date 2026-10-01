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

#ifndef BINDV_H
#define BINDV_H

#include "extension/extension.h"
#include "tracee/tracee.h"

/*
 * Privileged port emulation for a virtual root: an unprivileged process
 * cannot bind(2) to ports below 1024, so the port is moved to a high
 * one for the real system call and moved back in getsockname(2) and in
 * a loopback connect(2).  The guest believes it owns port 80 or 22.
 */

extern int bindv_callback(Extension *extension, ExtensionEvent event,
			  intptr_t d1, intptr_t d2);

#endif				/* BINDV_H */
