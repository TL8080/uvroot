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

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <talloc.h>
#include <unistd.h>

#include "cli/note.h"
#include "extension/bindv/bindv.h"
#include "extension/extension.h"
#include "tracee/mem.h"
#include "tracee/reg.h"
#include "tracee/tracee.h"

/* Privileged ports [1, 1023] are moved to [20001, 21023].  */
#define BINDV_OFFSET	20000
#define BINDV_FIRST	1
#define BINDV_LAST	1023

typedef struct BindvEntry {
    struct BindvEntry *next;
    pid_t pid;
    int fd;
    uint16_t requested;		/* the port the guest asked for */
    uint16_t actual;		/* the port really bound */
} BindvEntry;

typedef struct BindvConfig {
    bool enabled;
    BindvEntry *entries;
} BindvConfig;

static FilteredSysnum bindv_sysnums[] = {
    { PR_bind, FILTER_SYSEXIT },
    { PR_getsockname, FILTER_SYSEXIT },
    { PR_connect, 0 },
    { PR_accept, FILTER_SYSEXIT },
    { PR_accept4, FILTER_SYSEXIT },
    { PR_dup, FILTER_SYSEXIT },
    { PR_dup2, FILTER_SYSEXIT },
    { PR_dup3, FILTER_SYSEXIT },
    { PR_close, FILTER_SYSEXIT },
    FILTERED_SYSNUM_END,
};

static BindvConfig *config_of(Extension *extension)
{
    return talloc_get_type_abort(extension->config, BindvConfig);
}

/* ------------------------------------------------------------------ */
/* Address helpers                                                     */
/* ------------------------------------------------------------------ */

static int read_address(Tracee *tracee, word_t pointer, unsigned long length,
			uint8_t buffer[128], int *family, uint16_t *port)
{
    if (length < 4 || length > 128)
	return -1;
    if (read_data(tracee, buffer, pointer, length) < 0)
	return -1;
    *family = buffer[0] | (buffer[1] << 8);	/* sa_family_t, host order */
    *port = (uint16_t) ((buffer[2] << 8) | buffer[3]);
    return 0;
}

static void write_port(Tracee *tracee, word_t pointer, uint16_t port)
{
    uint8_t bytes[2];

    bytes[0] = (uint8_t) (port >> 8);
    bytes[1] = (uint8_t) (port & 0xFF);
    (void) write_data(tracee, pointer + 2, bytes, sizeof(bytes));
}

static bool is_loopback(const uint8_t *buffer, int family)
{
    if (family == AF_INET)
	return buffer[4] == 127;
    if (family == AF_INET6) {
	static const uint8_t loopback[16] = {
	    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
	};

	return memcmp(buffer + 8, loopback, sizeof(loopback)) == 0;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* Port table                                                          */
/* ------------------------------------------------------------------ */

static BindvEntry *entry_by_fd(BindvConfig *config, pid_t pid, int fd)
{
    BindvEntry *entry;

    for (entry = config->entries; entry != NULL; entry = entry->next) {
	if (entry->pid == pid && entry->fd == fd)
	    return entry;
    }
    return NULL;
}

static BindvEntry *entry_by_port(BindvConfig *config, uint16_t requested)
{
    BindvEntry *entry;

    for (entry = config->entries; entry != NULL; entry = entry->next) {
	if (entry->requested == requested)
	    return entry;
    }
    return NULL;
}

static void entry_add(BindvConfig *config, pid_t pid, int fd,
		      uint16_t requested, uint16_t actual)
{
    BindvEntry *entry = entry_by_fd(config, pid, fd);

    if (entry == NULL) {
	entry = talloc_zero(config, BindvEntry);
	if (entry == NULL)
	    return;
	entry->next = config->entries;
	config->entries = entry;
    }
    entry->pid = pid;
    entry->fd = fd;
    entry->requested = requested;
    entry->actual = actual;
}

static void entry_remove(BindvConfig *config, pid_t pid, int fd)
{
    BindvEntry **link;

    for (link = &config->entries; *link != NULL; link = &(*link)->next) {
	if ((*link)->pid == pid && (*link)->fd == fd) {
	    BindvEntry *entry = *link;

	    *link = entry->next;
	    TALLOC_FREE(entry);
	    return;
	}
    }
}

/* ------------------------------------------------------------------ */
/* Syscall handling                                                    */
/* ------------------------------------------------------------------ */

static int enter_bind(Tracee *tracee)
{
    uint8_t buffer[128];
    unsigned long length = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_3);
    word_t pointer = peek_reg(tracee, CURRENT, SYSARG_2);
    int family;
    uint16_t port;

    if (read_address(tracee, pointer, length, buffer, &family, &port) < 0)
	return 0;
    if (port < BINDV_FIRST || port > BINDV_LAST)
	return 0;
    if (family != AF_INET && family != AF_INET6)
	return 0;

    write_port(tracee, pointer, (uint16_t) (port + BINDV_OFFSET));
    return 0;
}

static int enter_connect(Tracee *tracee, BindvConfig *config)
{
    uint8_t buffer[128];
    unsigned long length = (unsigned long) peek_reg(tracee, CURRENT, SYSARG_3);
    word_t pointer = peek_reg(tracee, CURRENT, SYSARG_2);
    BindvEntry *entry;
    int family;
    uint16_t port;

    if (read_address(tracee, pointer, length, buffer, &family, &port) < 0)
	return 0;
    if (!is_loopback(buffer, family))
	return 0;

    entry = entry_by_port(config, port);
    if (entry == NULL)
	return 0;

    write_port(tracee, pointer, entry->actual);
    return 0;
}

static int exit_bind(Tracee *tracee, BindvConfig *config)
{
    uint8_t buffer[128];
    unsigned long length = (unsigned long) peek_reg(tracee, ORIGINAL, SYSARG_3);
    word_t pointer = peek_reg(tracee, ORIGINAL, SYSARG_2);
    int fd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
    int family;
    uint16_t port;

    if ((long) peek_reg(tracee, CURRENT, SYSARG_RESULT) != 0)
	return 0;
    if (read_address(tracee, pointer, length, buffer, &family, &port) < 0)
	return 0;
    (void) family;
    if (port > BINDV_OFFSET + BINDV_FIRST
	&& port <= BINDV_OFFSET + BINDV_LAST)
	entry_add(config, tracee->pid, fd,
		  (uint16_t) (port - BINDV_OFFSET), port);
    return 0;
}

static int exit_getsockname(Tracee *tracee, BindvConfig *config)
{
    unsigned long length = (unsigned long) peek_reg(tracee, ORIGINAL, SYSARG_3);
    word_t pointer = peek_reg(tracee, ORIGINAL, SYSARG_2);
    int fd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
    BindvEntry *entry;

    if ((long) peek_reg(tracee, CURRENT, SYSARG_RESULT) != 0)
	return 0;
    entry = entry_by_fd(config, tracee->pid, fd);
    if (entry == NULL)
	return 0;
    if (length > 2)
	write_port(tracee, pointer, entry->requested);
    return 0;
}

static int exit_inherit_fd(Tracee *tracee, BindvConfig *config, int from_reg)
{
    int source = (int) peek_reg(tracee, ORIGINAL, from_reg);
    int result = (int) peek_reg(tracee, CURRENT, SYSARG_RESULT);
    BindvEntry *entry;

    if (result < 0)
	return 0;
    entry = entry_by_fd(config, tracee->pid, source);
    if (entry != NULL)
	entry_add(config, tracee->pid, result,
		  entry->requested, entry->actual);
    return 0;
}

static int exit_syscall(Tracee *tracee, BindvConfig *config)
{
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);

    switch (sysnum) {
    case PR_bind:
	return exit_bind(tracee, config);
    case PR_getsockname:
	return exit_getsockname(tracee, config);
    case PR_accept:
    case PR_accept4:
    case PR_dup:
    case PR_dup2:
    case PR_dup3:
	return exit_inherit_fd(tracee, config, SYSARG_1);
    case PR_close:
	entry_remove(config, tracee->pid,
		     (int) peek_reg(tracee, ORIGINAL, SYSARG_1));
	return 0;
    default:
	return 0;
    }
}

static int enter_syscall(Tracee *tracee, BindvConfig *config)
{
    switch (get_sysnum(tracee, ORIGINAL)) {
    case PR_bind:
	return enter_bind(tracee);
    case PR_connect:
	return enter_connect(tracee, config);
    default:
	return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Extension plumbing                                                  */
/* ------------------------------------------------------------------ */

/*
 * A real (not virtual) root can bind privileged ports, and so can a
 * process with CAP_NET_BIND_SERVICE: in that case there is nothing to
 * emulate.  Probe once, with a few ports in case one is taken.
 */
static bool privileged_ports_denied(void)
{
    unsigned int port;

    for (port = BINDV_LAST; port >= 1000; port--) {
	struct sockaddr_in address;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int status;

	if (fd < 0)
	    return false;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((uint16_t) port);
	status = bind(fd, (struct sockaddr *) &address, sizeof(address));
	close(fd);
	if (status == 0)
	    return false;	/* we may bind privileged ports */
	if (errno == EACCES)
	    return true;
    }
    return true;
}

int bindv_callback(Extension *extension, ExtensionEvent event, intptr_t d1,
		   intptr_t d2 UNUSED)
{
    switch (event) {
    case INITIALIZATION:{
	BindvConfig *config;
	const char *value = (const char *) d1;
	long uid = 1;		/* not root by default */

	if (value != NULL)
	    uid = strtol(value, NULL, 10);

	config = talloc_zero(extension, BindvConfig);
	if (config == NULL)
	    return -1;
	config->enabled = (uid == 0) && privileged_ports_denied();
	extension->config = config;
	extension->filtered_sysnums = config->enabled ? bindv_sysnums : NULL;
	return 0;
    }

    case SYSCALL_ENTER_START:{
	BindvConfig *config = config_of(extension);

	if (!config->enabled)
	    return 0;
	return enter_syscall(TRACEE(extension), config);
    }

    case SYSCALL_EXIT_START:{
	BindvConfig *config = config_of(extension);

	if (!config->enabled)
	    return 0;
	return exit_syscall(TRACEE(extension), config);
    }

    case PRINT_CONFIG:{
	BindvConfig *config = config_of(extension);

	if (config->enabled)
	    note(TRACEE(extension), INFO, USER,
		 "bind = privileged ports [%d, %d] are emulated as "
		 "[%d, %d]", BINDV_FIRST, BINDV_LAST,
		 BINDV_FIRST + BINDV_OFFSET, BINDV_LAST + BINDV_OFFSET);
	return 0;
    }

    default:
	return 0;
    }
}
