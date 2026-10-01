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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <talloc.h>
#include <unistd.h>

#include "cli/note.h"
#include "extension/extension.h"
#include "extension/vpid/vpid.h"
#include "path/temp.h"
#include "syscall/syscall.h"
#include "syscall/sysnum.h"
#include "tracee/mem.h"
#include "tracee/reg.h"
#include "tracee/tracee.h"

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

typedef struct VpidFile {
    struct VpidFile *next;
    int fd;
    uint64_t vpid;
} VpidFile;

typedef struct VpidTracee {
    struct VpidTracee *next;
    pid_t pid;
    VpidFile *files;
} VpidTracee;

typedef struct VpidEntry {
    struct VpidEntry *next;
    pid_t real;
    uint64_t vpid;
} VpidEntry;

typedef struct VpidConfig {
    bool enabled;
    uint64_t first_vpid;
    bool seeded;
    char *temp_dir;
    VpidTracee *tracees;
    /* Kept independently of the tracee list so that a child that has
     * exited but not been reaped still has a pid mapping.  */
    VpidEntry *map;
} VpidConfig;

static VpidConfig *config_of(Extension *extension)
{
    return talloc_get_type_abort(extension->config, VpidConfig);
}

static const FilteredSysnum vpid_sysnums[] = {
    /* pid-returning syscalls: rewritten at the exit stage.  */
    { PR_getpid, FILTER_SYSEXIT },
    { PR_gettid, FILTER_SYSEXIT },
    { PR_getppid, FILTER_SYSEXIT },
    { PR_fork, FILTER_SYSEXIT },
    { PR_vfork, FILTER_SYSEXIT },
    { PR_clone, FILTER_SYSEXIT },
    { PR_wait4, FILTER_SYSEXIT },
    { PR_waitid, FILTER_SYSEXIT },
    { PR_getpgrp, FILTER_SYSEXIT },
    { PR_getpgid, FILTER_SYSEXIT },
    { PR_setpgid, FILTER_SYSEXIT },
    { PR_getsid, FILTER_SYSEXIT },
    { PR_setsid, FILTER_SYSEXIT },

    /* pid-argument syscalls: translated at the enter stage.  */
    { PR_kill, 0 },
    { PR_tkill, 0 },
    { PR_tgkill, 0 },
    { PR_rt_sigqueueinfo, 0 },
    { PR_rt_tgsigqueueinfo, 0 },

    /* /proc paths and listings.  */
    { PR_open, FILTER_SYSEXIT },
    { PR_openat, FILTER_SYSEXIT },
    { PR_stat, 0 },
    { PR_lstat, 0 },
    { PR_newfstatat, 0 },
    { PR_fstatat64, 0 },
    { PR_statx, 0 },
    { PR_access, 0 },
    { PR_faccessat, 0 },
    { PR_faccessat2, 0 },
    { PR_readlink, 0 },
    { PR_readlinkat, 0 },
    { PR_chdir, 0 },
    { PR_statfs, 0 },
    { PR_statfs64, 0 },
    { PR_execve, FILTER_SYSEXIT },
    { PR_getdents, FILTER_SYSEXIT },
    { PR_getdents64, FILTER_SYSEXIT },
    { PR_close, FILTER_SYSEXIT },
    FILTERED_SYSNUM_END,
};

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static bool is_number(const char *text)
{
    if (text[0] == '\0')
	return false;
    for (; *text != '\0'; text++) {
	if (*text < '0' || *text > '9')
	    return false;
    }
    return true;
}

static Tracee *tracee_by_real(Tracee *current, pid_t pid)
{
    Tracee *tracee;

    if (current->container == NULL)
	return NULL;
    LIST_FOREACH(tracee, &current->container->tracees, link) {
	if (tracee->pid == pid)
	    return tracee;
    }
    return NULL;
}

static Tracee *tracee_by_virtual(Tracee *current, uint64_t vpid)
{
    Tracee *tracee;

    if (current->container == NULL)
	return NULL;
    LIST_FOREACH(tracee, &current->container->tracees, link) {
	if (tracee->vpid == vpid)
	    return tracee;
    }
    return NULL;
}

static uint64_t map_virtual(VpidConfig *config, pid_t real);
static pid_t map_real(VpidConfig *config, uint64_t vpid);

static uint64_t virtual_of_real(Tracee *tracee, VpidConfig *config, pid_t pid)
{
    uint64_t vpid = map_virtual(config, pid);
    Tracee *owner;

    if (vpid != 0)
	return vpid;
    owner = tracee_by_real(tracee, pid);
    return (owner != NULL) ? owner->vpid : 0;
}

static pid_t real_of_virtual(Tracee *tracee, VpidConfig *config, uint64_t vpid)
{
    pid_t real = map_real(config, vpid);
    Tracee *owner;

    if (real > 0)
	return real;
    owner = tracee_by_virtual(tracee, vpid);
    return (owner != NULL) ? owner->pid : -1;
}

static uint64_t parent_virtual(Tracee *tracee)
{
    if (tracee->parent == NULL)
	return 0;
    if (tracee->parent->container != tracee->container)
	return 0;
    return tracee->parent->vpid;
}

static void map_add(VpidConfig *config, pid_t real, uint64_t vpid)
{
    VpidEntry *entry;

    if (real <= 0 || vpid == 0)
	return;
    for (entry = config->map; entry != NULL; entry = entry->next) {
	if (entry->real == real) {
	    entry->vpid = vpid;
	    return;
	}
    }
    entry = talloc_zero(config, VpidEntry);
    if (entry == NULL)
	return;
    entry->real = real;
    entry->vpid = vpid;
    entry->next = config->map;
    config->map = entry;
}

static void map_remove(VpidConfig *config, pid_t real)
{
    VpidEntry **link;

    for (link = &config->map; *link != NULL; link = &(*link)->next) {
	if ((*link)->real == real) {
	    VpidEntry *entry = *link;

	    *link = entry->next;
	    TALLOC_FREE(entry);
	    return;
	}
    }
}

static uint64_t map_virtual(VpidConfig *config, pid_t real)
{
    VpidEntry *entry;

    for (entry = config->map; entry != NULL; entry = entry->next) {
	if (entry->real == real)
	    return entry->vpid;
    }
    return 0;
}

static pid_t map_real(VpidConfig *config, uint64_t vpid)
{
    VpidEntry *entry;

    for (entry = config->map; entry != NULL; entry = entry->next) {
	if (entry->vpid == vpid)
	    return entry->real;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* /proc path translation                                              */
/* ------------------------------------------------------------------ */

/*
 * Rewrite "/proc/<vpid>[/...]" into the host "/proc/<realpid>[/...]".
 * Returns 0 when the path is usable (rewritten or left alone) and
 * -ENOENT when it refers to a host process, so that the container
 * cannot see the host pid space at all.
 */
static int rewrite_proc_path(Tracee *tracee, VpidConfig *config,
			     char path[PATH_MAX])
{
    char original[PATH_MAX];
    char rebuilt[PATH_MAX];
    const char *cursor;
    char component[64];
    const char *tail;
    size_t length = 0;
    uint64_t value;
    pid_t real;

    if (strncmp(path, "/proc", 5) != 0
	|| (path[5] != '\0' && path[5] != '/'))
	return 0;
    if (path[5] == '\0' || path[6] == '\0')
	return 0;		/* the /proc root itself */

    snprintf(original, sizeof(original), "%s", path);
    cursor = original + 6;	/* skip "/proc/" */

    /* First component: a pid, "self" or "thread-self".  */
    while (*cursor != '\0' && *cursor != '/' && length < sizeof(component) - 1)
	component[length++] = *cursor++;
    component[length] = '\0';
    tail = cursor;

    if (strcmp(component, "self") == 0) {
	snprintf(rebuilt, sizeof(rebuilt), "/proc/%d%s", tracee->pid, tail);
	snprintf(path, PATH_MAX, "%s", rebuilt);
	return 0;
    }
    if (strcmp(component, "thread-self") == 0) {
	snprintf(rebuilt, sizeof(rebuilt), "/proc/%d/task/%d%s",
		 tracee->pid, tracee->pid, tail);
	snprintf(path, PATH_MAX, "%s", rebuilt);
	return 0;
    }
    if (!is_number(component))
	return 0;		/* /proc/net, /proc/sys, ... */

    value = strtoull(component, NULL, 10);
    real = real_of_virtual(tracee, config, value);
    if (real < 0) {
	/* Not a virtual pid: accept it only when it is the real pid of
	 * a container process, and normalise it to that process.  */
	if (virtual_of_real(tracee, config, (pid_t) value) == 0)
	    return -ENOENT;
	real = (pid_t) value;
    }

    /* Map a virtual thread id inside /proc/<pid>/task/<tid>.  */
    if (strncmp(tail, "/task/", 6) == 0 && is_number(tail + 6)) {
	char thread[64];
	const char *rest = tail + 6;
	size_t thread_length = 0;
	pid_t thread_real;

	while (*rest != '\0' && *rest != '/'
	       && thread_length < sizeof(thread) - 1)
	    thread[thread_length++] = *rest++;
	thread[thread_length] = '\0';

	thread_real = real_of_virtual(tracee, config, strtoull(thread, NULL, 10));
	if (thread_real > 0)
	    snprintf(rebuilt, sizeof(rebuilt), "/proc/%d/task/%d%s",
		     real, thread_real, rest);
	else
	    snprintf(rebuilt, sizeof(rebuilt), "/proc/%d/task/%s%s",
		     real, thread, rest);
    } else
	snprintf(rebuilt, sizeof(rebuilt), "/proc/%d%s", real, tail);

    snprintf(path, PATH_MAX, "%s", rebuilt);
    return 0;
}

static Reg path_register(Sysnum sysnum)
{
    switch (sysnum) {
    case PR_openat:
    case PR_newfstatat:
    case PR_fstatat64:
    case PR_statx:
    case PR_faccessat:
    case PR_faccessat2:
    case PR_readlinkat:
	return SYSARG_2;
    default:
	return SYSARG_1;
    }
}

static bool takes_path(Sysnum sysnum)
{
    switch (sysnum) {
    case PR_open:
    case PR_openat:
    case PR_stat:
    case PR_lstat:
    case PR_newfstatat:
    case PR_fstatat64:
    case PR_statx:
    case PR_access:
    case PR_faccessat:
    case PR_faccessat2:
    case PR_readlink:
    case PR_readlinkat:
    case PR_chdir:
    case PR_statfs:
    case PR_statfs64:
    case PR_execve:
	return true;
    default:
	return false;
    }
}

/* ------------------------------------------------------------------ */
/* Generated /proc/<pid>/status and /proc/<pid>/stat                   */
/* ------------------------------------------------------------------ */

static const char *temp_directory(VpidConfig *config)
{
    if (config->temp_dir == NULL)
	config->temp_dir = (char *) create_temp_directory(config,
							  "uvroot-vpid");
    return config->temp_dir;
}

static pid_t real_pid_from_status_path(const char *host_path)
{
    const char *file = strrchr(host_path, '/');
    const char *pid_end;
    const char *pid_start;
    char component[64];
    size_t length;

    if (file == NULL)
	return -1;
    file++;
    if (strcmp(file, "status") != 0 && strcmp(file, "stat") != 0)
	return -1;

    pid_end = file - 1;		/* the '/' before the file name */
    if (pid_end <= host_path)
	return -1;
    pid_start = pid_end;
    while (pid_start > host_path && pid_start[-1] != '/')
	pid_start--;

    length = (size_t) (pid_end - pid_start);
    if (length == 0 || length >= sizeof(component))
	return -1;
    memcpy(component, pid_start, length);
    component[length] = '\0';
    if (!is_number(component))
	return -1;
    return (pid_t) strtol(component, NULL, 10);
}

/*
 * Write a synthetic /proc/<pid>/status or /proc/<pid>/stat where the
 * pid-like fields are the virtual ones, and return its host path.  The
 * real file is read by uvroot itself (it runs as the same user) and the
 * tracee only ever sees the generated copy.
 */
static int write_status_file(Tracee *tracee, VpidConfig *config, pid_t real,
			     uint64_t vpid, bool is_stat, char output[PATH_MAX])
{
    char source[PATH_MAX];
    char line[4096];
    const char *directory = temp_directory(config);
    FILE *input;
    FILE *out;

    if (directory == NULL)
	return -1;
    snprintf(source, sizeof(source), "/proc/%d/%s", real,
	     is_stat ? "stat" : "status");
    input = fopen(source, "r");
    if (input == NULL)
	return -1;
    snprintf(output, PATH_MAX, "%s/%llu.%s", directory,
	     (unsigned long long) vpid, is_stat ? "stat" : "status");
    out = fopen(output, "w");
    if (out == NULL) {
	fclose(input);
	return -1;
    }

    if (is_stat) {
	/* "pid (comm) state ppid pgrp session tty_nr tpgid ..." -- the
	 * pid-like fields (1, 4, 5, 6 and 8) must all be virtual, or the
	 * real host pids leak through /proc/<pid>/stat.  */
	while (fgets(line, sizeof(line), input) != NULL) {
	    char *closing = strrchr(line, ')');
	    char *first_space = strchr(line, ' ');
	    char tokens[64][64];
	    int count = 0;
	    char *cursor;
	    Tracee *owner = tracee_by_real(tracee, real);
	    int index;

	    if (closing == NULL || first_space == NULL
		|| first_space > closing) {
		fputs(line, out);
		continue;
	    }

	    cursor = closing + 1;
	    while (*cursor != '\0' && count < 64) {
		char *start;
		size_t length;

		while (*cursor == ' ')
		    cursor++;
		if (*cursor == '\0')
		    break;
		start = cursor;
		while (*cursor != '\0' && *cursor != ' ')
		    cursor++;
		length = (size_t) (cursor - start);
		if (length >= sizeof(tokens[0]))
		    length = sizeof(tokens[0]) - 1;
		memcpy(tokens[count], start, length);
		tokens[count][length] = '\0';
		count++;
	    }

	    fprintf(out, "%llu%.*s", (unsigned long long) vpid,
		    (int) (closing + 1 - first_space), first_space);
	    for (index = 0; index < count; index++) {
		/* Field number is index + 3; 4=ppid, 5=pgrp, 6=session,
		 * 8=tpgid.  */
		int field = index + 3;
		uint64_t value = 0;

		if (field == 4)
		    value = (owner != NULL) ? parent_virtual(owner) : 0;
		else if ((field == 5 || field == 6 || field == 8)
			 && is_number(tokens[index])) {
		    value = virtual_of_real(tracee, config,
					    (pid_t) strtol(tokens[index],
							   NULL, 10));
		    /* An inherited group/session leader lives outside the
		     * container: report the process itself, never a host
		     * pid.  */
		    if (value == 0 && (field == 5 || field == 6))
			value = vpid;
		} else {
		    fprintf(out, " %s", tokens[index]);
		    continue;
		}
		fprintf(out, " %llu", (unsigned long long) value);
	    }
	    fputc('\n', out);
	}
    } else {
	char header[32];
	size_t header_length;
	char *colon;

	while (fgets(line, sizeof(line), input) != NULL) {
	    colon = strchr(line, ':');
	    if (colon == NULL) {
		fputs(line, out);
		continue;
	    }
	    header_length = (size_t) (colon - line);
	    if (header_length >= sizeof(header))
		header_length = sizeof(header) - 1;
	    memcpy(header, line, header_length);
	    header[header_length] = '\0';

	    if (strcmp(header, "Pid") == 0 || strcmp(header, "Tgid") == 0
		|| strcmp(header, "NSpid") == 0
		|| strcmp(header, "NStgid") == 0)
		fprintf(out, "%s:\t%llu\n", header,
			(unsigned long long) vpid);
	    else if (strcmp(header, "PPid") == 0) {
		Tracee *owner = tracee_by_real(tracee, real);

		fprintf(out, "PPid:\t%llu\n",
			(unsigned long long) (owner != NULL
					      ? parent_virtual(owner) : 0));
	    } else
		fputs(line, out);
	}
    }

    fclose(input);
    fclose(out);
    return 0;
}

/* ------------------------------------------------------------------ */
/* /proc directory listing                                             */
/* ------------------------------------------------------------------ */

static bool fd_is_proc_directory(Tracee *tracee, int fd, bool *is_task)
{
    char link[64];
    char target[PATH_MAX];
    ssize_t length;

    snprintf(link, sizeof(link), "/proc/%d/fd/%d", tracee->pid, fd);
    length = readlink(link, target, sizeof(target) - 1);
    if (length <= 0)
	return false;
    target[length] = '\0';

    if (strcmp(target, "/proc") == 0) {
	*is_task = false;
	return true;
    }
    if (length >= 5 && strcmp(target + length - 5, "/proc") == 0) {
	*is_task = false;
	return true;
    }
    if (length >= 5 && strcmp(target + length - 5, "/task") == 0) {
	*is_task = true;
	return true;
    }
    return false;
}

static ssize_t rewrite_dirents(Tracee *tracee, VpidConfig *config,
			       uint8_t *buffer, size_t length, bool is64)
{
    size_t name_offset = is64 ? 19 : 18;	/* d_name offset */
    size_t record_header = is64 ? 19 : 18;
    size_t in = 0;
    size_t out = 0;

    while (in + record_header <= length) {
	uint8_t *entry = buffer + in;
	uint16_t record_length;

	memcpy(&record_length, entry + (is64 ? 16 : 16), sizeof(record_length));
	if (record_length < record_header || in + record_length > length)
	    break;

	{
	    char *name = (char *) entry + name_offset;
	    size_t name_length = record_length - name_offset;
	    bool keep = true;

	    if (name_length > 0 && is_number(name)) {
		uint64_t vpid =
		    virtual_of_real(tracee, config, (pid_t) strtol(name, NULL, 10));

		if (vpid == 0)
		    keep = false;
		else {
		    char text[32];
		    size_t text_length;

		    snprintf(text, sizeof(text), "%llu",
			     (unsigned long long) vpid);
		    text_length = strlen(text);
		    if (text_length > name_length)
			keep = false;
		    else {
			memset(name, 0, name_length);
			memcpy(name, text, text_length);
		    }
		}
	    }

	    if (keep) {
		if (out != in)
		    memmove(buffer + out, entry, record_length);
		out += record_length;
	    }
	}

	in += record_length;
    }

    return (ssize_t) out;
}

/* ------------------------------------------------------------------ */
/* Syscall handling                                                    */
/* ------------------------------------------------------------------ */

static int enter_kill(Tracee *tracee, VpidConfig *config)
{
    /* kill/tkill/tgkill take the pid as the first argument, the
     * sigqueue variants too.  Translate virtual pids to real ones and
     * leave the special values (0, -1, negative groups) alone.  */
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    int count = (sysnum == PR_tgkill || sysnum == PR_rt_tgsigqueueinfo)
	? 2 : 1;
    Reg reg;

    for (reg = SYSARG_1; count > 0; count--, reg++) {
	long value = (long) peek_reg(tracee, CURRENT, reg);
	pid_t real;

	if (value <= 0)
	    continue;
	real = real_of_virtual(tracee, config, (uint64_t) value);
	if (real > 0)
	    poke_reg(tracee, reg, (word_t) real);
    }
    return 0;
}

static int enter_path(Tracee *tracee, VpidConfig *config)
{
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    char path[PATH_MAX];
    int status;

    (void) config;
    status = get_sysarg_path(tracee, path, path_register(sysnum));
    if (status < 0)
	return status;
    if (strncmp(path, "/proc", 5) != 0)
	return 0;

    status = rewrite_proc_path(tracee, config, path);
    if (status < 0)
	return status;
    if (path[0] == '\0')
	return 0;

    status = set_sysarg_path(tracee, path, path_register(sysnum));
    if (status < 0)
	return status;
    return 0;
}

static int enter_syscall(Tracee *tracee, VpidConfig *config)
{
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);

    if (takes_path(sysnum))
	return enter_path(tracee, config);
    switch (sysnum) {
    case PR_kill:
    case PR_tkill:
    case PR_tgkill:
    case PR_rt_sigqueueinfo:
    case PR_rt_tgsigqueueinfo:
	return enter_kill(tracee, config);

    case PR_wait4:{
	long pid = (long) peek_reg(tracee, CURRENT, SYSARG_1);

	if (pid > 0) {
	    pid_t real = real_of_virtual(tracee, config, (uint64_t) pid);

	    if (real > 0)
		poke_reg(tracee, SYSARG_1, (word_t) real);
	}
	return 0;
    }

    case PR_waitid:{
	long idtype = (long) peek_reg(tracee, CURRENT, SYSARG_1);
	long id = (long) peek_reg(tracee, CURRENT, SYSARG_2);

	/* P_PID == 1.  */
	if (idtype == 1 && id > 0) {
	    pid_t real = real_of_virtual(tracee, config, (uint64_t) id);

	    if (real > 0)
		poke_reg(tracee, SYSARG_2, (word_t) real);
	}
	return 0;
    }

    default:
	return 0;
    }
}

static int exit_syscall(Tracee *tracee, VpidConfig *config)
{
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    long result = (long) peek_reg(tracee, CURRENT, SYSARG_RESULT);

    (void) config;
    switch (sysnum) {
    case PR_getpid:
    case PR_gettid:
	poke_reg(tracee, SYSARG_RESULT, (word_t) tracee->vpid);
	return 1;

    case PR_getppid:
	poke_reg(tracee, SYSARG_RESULT, (word_t) parent_virtual(tracee));
	return 1;

    /* The container is its own session and process group: report the
     * tracee's own virtual pid so that a shell believes it is a group
     * leader and skips setpgid(), which would otherwise fail under
     * ptrace.  */
    case PR_getpgrp:
    case PR_getpgid:
    case PR_getsid:
    case PR_setsid:
	poke_reg(tracee, SYSARG_RESULT, (word_t) tracee->vpid);
	return 1;

    case PR_setpgid:
	poke_reg(tracee, SYSARG_RESULT, 0);
	return 1;

    case PR_fork:
    case PR_vfork:
    case PR_clone:
	if (result > 0) {
	    uint64_t vpid = virtual_of_real(tracee, config, (pid_t) result);

	    if (vpid != 0)
		poke_reg(tracee, SYSARG_RESULT, (word_t) vpid);
	}
	return 1;

    case PR_getdents:
    case PR_getdents64:{
	int fd = (int) peek_reg(tracee, ORIGINAL, SYSARG_1);
	bool is_task = false;
	word_t address;
	uint8_t *buffer;
	ssize_t rewritten;

	if (result <= 0)
	    return 0;
	if (!fd_is_proc_directory(tracee, fd, &is_task))
	    return 0;
	(void) is_task;

	address = peek_reg(tracee, ORIGINAL, SYSARG_2);
	buffer = talloc_size(tracee->ctx, (size_t) result);
	if (buffer == NULL)
	    return 0;
	if (read_data(tracee, buffer, address, (word_t) result) < 0) {
	    talloc_free(buffer);
	    return 0;
	}
	rewritten = rewrite_dirents(tracee, config, buffer, (size_t) result,
				    sysnum == PR_getdents64);
	if (write_data(tracee, address, buffer, (word_t) rewritten) >= 0)
	    poke_reg(tracee, SYSARG_RESULT, (word_t) rewritten);
	talloc_free(buffer);
	return 1;
    }

    default:
	return 0;
    }
}

static int exit_end_syscall(Tracee *tracee, VpidConfig *config)
{
    Sysnum sysnum = get_sysnum(tracee, ORIGINAL);
    long result = (long) peek_reg(tracee, CURRENT, SYSARG_RESULT);

    if ((sysnum == PR_wait4 || sysnum == PR_waitid) && result > 0) {
	uint64_t vpid = virtual_of_real(tracee, config, (pid_t) result);

	if (vpid != 0)
	    poke_reg(tracee, SYSARG_RESULT, (word_t) vpid);
	map_remove(config, (pid_t) result);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Extension plumbing                                                  */
/* ------------------------------------------------------------------ */

static Extension *vpid_extension(Tracee *tracee)
{
    Extension *extension = get_extension(tracee, vpid_callback);

    if (extension == NULL) {
	if (initialize_extension(tracee, vpid_callback, NULL) < 0)
	    return NULL;
	extension = get_extension(tracee, vpid_callback);
    }
    return extension;
}

static VpidConfig *vpid_config(Tracee *tracee, bool create)
{
    Extension *extension;

    if (create)
	extension = vpid_extension(tracee);
    else
	extension = get_extension(tracee, vpid_callback);
    return (extension != NULL) ? config_of(extension) : NULL;
}

int vpid_enable(Tracee *tracee)
{
    VpidConfig *config = vpid_config(tracee, true);

    if (config == NULL)
	return -ENOMEM;
    if (!config->enabled) {
	config->enabled = true;
	config->first_vpid = 1;
    }
    return 0;
}

int vpid_set(Tracee *tracee, const char *value)
{
    VpidConfig *config;
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0') {
	note(tracee, ERROR, USER, "vpid: missing process id");
	return -EINVAL;
    }
    parsed = strtoull(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > 0x40000000ULL) {
	note(tracee, ERROR, USER, "vpid: invalid process id \"%s\"", value);
	return -EINVAL;
    }

    config = vpid_config(tracee, true);
    if (config == NULL)
	return -ENOMEM;
    config->enabled = true;
    config->first_vpid = parsed;
    config->seeded = false;
    return 0;
}

int vpid_callback(Extension *extension, ExtensionEvent event, intptr_t d1,
		  intptr_t d2 UNUSED)
{
    switch (event) {
    case INITIALIZATION:{
	VpidConfig *config;

	if (extension->config == NULL) {
	    config = talloc_zero(extension, VpidConfig);
	    if (config == NULL)
		return -1;
	    config->first_vpid = 1;
	    extension->config = config;
	}
	extension->filtered_sysnums = vpid_sysnums;
	return 0;
    }

    case INHERIT_PARENT:{
	Tracee *parent = TRACEE(extension);
	Tracee *child = (Tracee *) d1;
	VpidConfig *config = config_of(extension);

	if (config->enabled && child != NULL)
	    map_add(config, child->pid, child->vpid);
	(void) parent;
	return 0;
    }

    case SYSCALL_ENTER_START:{
	Tracee *tracee = TRACEE(extension);
	VpidConfig *config = config_of(extension);

	if (!config->enabled)
	    return 0;

	/* Seed the first program's pid once the tracee really exists.  */
	if (!config->seeded) {
	    config->seeded = true;
	    tracee->vpid = config->first_vpid;
	    if (tracee->container != NULL)
		tracee->container->next_vpid = config->first_vpid + 1;
	    map_add(config, tracee->pid, tracee->vpid);
	}
	return enter_syscall(tracee, config);
    }

    case SYSCALL_EXIT_START:{
	VpidConfig *config = config_of(extension);

	if (!config->enabled)
	    return 0;
	return exit_syscall(TRACEE(extension), config);
    }

    case SYSCALL_EXIT_END:{
	VpidConfig *config = config_of(extension);

	if (!config->enabled)
	    return 0;
	return exit_end_syscall(TRACEE(extension), config);
    }

    case TRANSLATED_PATH:{
	Tracee *tracee = TRACEE(extension);
	VpidConfig *config = config_of(extension);
	char *host_path = (char *) d1;
	pid_t real;
	uint64_t vpid;
	bool is_stat;
	char generated[PATH_MAX];

	if (!config->enabled || host_path == NULL)
	    return 0;
	real = real_pid_from_status_path(host_path);
	if (real <= 0)
	    return 0;
	vpid = virtual_of_real(tracee, config, real);
	if (vpid == 0)
	    return 0;
	is_stat = (strcmp(host_path + strlen(host_path) - 4, "stat") == 0);
	if (write_status_file(tracee, config, real, vpid, is_stat,
			      generated) == 0) {
	    snprintf(host_path, PATH_MAX, "%s", generated);
	}
	return 0;
    }

    case REMOVED:
	return 0;

    case PRINT_CONFIG:{
	VpidConfig *config = config_of(extension);

	if (config->enabled)
	    note(TRACEE(extension), INFO, USER,
		 "vpid = first process gets the virtual pid %llu",
		 (unsigned long long) config->first_vpid);
	return 0;
    }

    default:
	return 0;
    }
}
