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
 * A container owns everything that was process-global before uvroot
 * could host more than one: its tracee tree, its virtual pid counter,
 * its exit status and its seccomp detection.  State that used to be a
 * file-scope variable now lives here, so that several containers can
 * later run as several threads of one process and share the images
 * through that process' own address space.
 */

#ifndef CONTAINER_H
#define CONTAINER_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/queue.h>

struct tracee;

/* The tracees of one container.  */
LIST_HEAD(tracees, tracee);
typedef struct tracees Tracees;

typedef struct Container {
    struct Container *next;
    struct Container *next_all;	/* every container of this process */

    int id;			/* container number, for logs */
    Tracees tracees;		/* its tracee tree */
    uint64_t next_vpid;		/* virtual pid counter */
    int exit_status;		/* status of the last exited tracee */

    /* Detection of seccomp mode 2, per container (it used to be a
     * function-local static shared by every tracee of the process).  */
    bool seccomp_detected;
    bool seccomp_enabled;
    bool deliver_sigtrap;

    /* Serializes access to the images this container shares with the
     * other containers of the process.  */
    pthread_mutex_t lock;
} Container;

/* The container of the calling thread.  */
extern Container *container_current(void);

/* Run @container as the container of the calling thread.  */
extern void container_set_current(Container * container);

/* True when this process hosts several containers (--multi): a
 * container may then have to wait for another one to open an image.  */
extern bool container_is_multi(void);
extern void container_mark_multi(void);

/* Allocate a new container (id is assigned automatically).  */
extern Container *container_create(void);

/* Release a container created by container_create().  */
extern void container_destroy(Container * container);

/*
 * Every container of this process, for signal handling: a fatal signal
 * must stop all of them, not only the one of the interrupted thread.
 */
extern Container *container_first(void);
extern Container *container_next(Container * container);

#endif /* CONTAINER_H */
