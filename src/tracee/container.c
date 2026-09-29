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

#include <stdlib.h>
#include <string.h>

#include "tracee/container.h"

/* Container of the calling thread.  Each thread that hosts containers
 * (one per container) gets its own.  */
static __thread Container *current_container;

/* Set when several containers share this process.  */
static bool multi_mode;

bool container_is_multi(void)
{
    return multi_mode;
}

void container_mark_multi(void)
{
    multi_mode = true;
}

/* Guards the container id counter.  */
static pthread_mutex_t id_lock = PTHREAD_MUTEX_INITIALIZER;

/* Every container of this process.  Prepended on creation so that a
 * signal handler can walk it without taking a lock.  */
static Container *all_containers;

Container *container_first(void)
{
    return all_containers;
}

Container *container_next(Container *container)
{
    return container != NULL ? container->next_all : NULL;
}

static int next_container_id;

static void container_init(Container *container, int id)
{
    memset(container, 0, sizeof(*container));
    LIST_INIT(&container->tracees);
    container->id = id;
    container->next_vpid = 1;
    container->exit_status = -1;
    pthread_mutex_init(&container->lock, NULL);
}

Container *container_current(void)
{
    if (current_container == NULL)
	current_container = container_create();

    return current_container;
}

void container_set_current(Container *container)
{
    current_container = container;
}

Container *container_create(void)
{
    /* Deliberately not talloc: a container outlives every talloc
     * context of its thread, and allocating from the global null
     * context concurrently from several threads is not safe.  */
    Container *container = calloc(1, sizeof(Container));
    int id;

    if (container == NULL)
	return NULL;

    pthread_mutex_lock(&id_lock);
    id = next_container_id++;
    pthread_mutex_unlock(&id_lock);

    container_init(container, id);
    container->next_all = all_containers;
    all_containers = container;
    return container;
}

void container_destroy(Container *container)
{
    if (container == NULL)
	return;

    {
	Container **link;

	for (link = &all_containers; *link != NULL; link = &(*link)->next_all) {
	    if (*link == container) {
		*link = container->next_all;
		break;
	    }
	}
    }

    pthread_mutex_destroy(&container->lock);
    free(container);
}
