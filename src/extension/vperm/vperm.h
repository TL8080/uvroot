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

#ifndef VPERM_H
#define VPERM_H

#include "extension/extension.h"
#include "tracee/tracee.h"

/* vperm.c */
extern int vperm_callback(Extension * extension, ExtensionEvent event,
			  intptr_t d1, intptr_t d2);
extern int vperm_enable(Tracee * tracee);

/* The initial virtual identity of @tracee (the real one without vperm).  */
extern uid_t vperm_initial_uid(Tracee * tracee);
extern int vperm_set_file(Tracee * tracee, const char *value);
extern int vperm_set_id(Tracee * tracee, const char *value);
extern int vperm_set_map(Tracee * tracee, const char *value);
extern int vperm_set_no_shims(Tracee * tracee);
extern int vperm_finalize(Tracee * tracee);

#endif				/* VPERM_H */
