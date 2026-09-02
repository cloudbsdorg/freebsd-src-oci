/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * $FreeBSD$
 *
 * Ring buffer implementation
 */

#include <sys/param.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "logd.h"

/*
 * Create a new ring buffer
 */
struct log_ringbuf *
ringbuf_create(uint64_t size)
{
	struct log_ringbuf *rb;
	size_t alloc_size;

	if (size == 0)
		size = 100000;  /* Default 100K entries */

	rb = calloc(1, sizeof(*rb));
	if (rb == NULL)
		return (NULL);

	rb->size = size;
	rb->head = 0;
	rb->tail = 0;
	rb->count = 0;
	rb->total_written = 0;

	/* Allocate entries array. Record whether it came from mmap or the calloc
	 * fallback so it is released with the matching call — the error path and
	 * ringbuf_destroy previously always munmap'd, which is UB on a calloc'd
	 * buffer. */
	alloc_size = size * sizeof(struct log_entry);
	rb->entries_bytes = alloc_size;
	rb->entries = mmap(NULL, alloc_size,
		PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);

	if (rb->entries == MAP_FAILED) {
		/* Fall back to calloc */
		rb->entries = calloc(size, sizeof(struct log_entry));
		if (rb->entries == NULL) {
			free(rb);
			return (NULL);
		}
		rb->entries_mmapped = false;
	} else {
		rb->entries_mmapped = true;
	}

	/* Allocate ID array for tracking */
	rb->ids = calloc(size, sizeof(uint64_t));
	if (rb->ids == NULL) {
		if (rb->entries_mmapped)
			munmap(rb->entries, alloc_size);
		else
			free(rb->entries);
		free(rb);
		return (NULL);
	}

	pthread_mutex_init(&rb->lock, NULL);

	return (rb);
}

/*
 * Destroy ring buffer
 */
void
ringbuf_destroy(struct log_ringbuf *rb)
{
	if (rb == NULL)
		return;

	pthread_mutex_destroy(&rb->lock);

	if (rb->entries_mmapped)
		munmap(rb->entries, rb->entries_bytes);
	else
		free(rb->entries);
	free(rb->ids);
	free(rb);
}

/*
 * Write entry to ring buffer
 */
int
ringbuf_write(struct log_ringbuf *rb, struct log_entry *entry)
{
	uint64_t pos;

	if (rb == NULL || entry == NULL)
		return (-1);

	pthread_mutex_lock(&rb->lock);

	pos = rb->head;

	/* Copy entry to buffer */
	memcpy(&rb->entries[pos], entry, sizeof(struct log_entry));

	/* Store ID for lookup */
	rb->ids[pos] = entry->id;

	/* Advance head */
	rb->head = (rb->head + 1) % rb->size;
	rb->total_written++;

	if (rb->count < rb->size) {
		rb->count++;
	} else {
		/* Buffer full, advance tail */
		rb->tail = (rb->tail + 1) % rb->size;
	}

	pthread_mutex_unlock(&rb->lock);

	return (0);
}

/*
 * Read entry by ID
 */
int
ringbuf_read(struct log_ringbuf *rb, uint64_t id, struct log_entry *out)
{
	if (rb == NULL || out == NULL)
		return (-1);

	pthread_mutex_lock(&rb->lock);

	/* Search for ID; copy the entry out under the lock. */
	for (uint64_t i = 0; i < rb->size; i++) {
		if (rb->ids[i] == id) {
			*out = rb->entries[i];
			pthread_mutex_unlock(&rb->lock);
			return (0);
		}
	}

	pthread_mutex_unlock(&rb->lock);

	return (-1);
}

/*
 * Iterate through buffer
 */
int
ringbuf_iterate(struct log_ringbuf *rb, uint64_t *cursor,
	struct log_entry *out)
{
	uint64_t slot;

	if (rb == NULL || cursor == NULL || out == NULL)
		return (-1);

	pthread_mutex_lock(&rb->lock);

	/*
	 * *cursor is the opaque count of entries already returned (0 to
	 * start). This avoids overloading slot index 0 as a "start" sentinel,
	 * which caused an infinite loop returning entries[0] forever whenever
	 * tail == 0 (i.e. until the buffer first wraps).
	 */
	if (*cursor >= rb->count) {
		pthread_mutex_unlock(&rb->lock);
		return (-1);
	}

	slot = (rb->tail + *cursor) % rb->size;
	(*cursor)++;
	*out = rb->entries[slot];		/* copy out under the lock */

	pthread_mutex_unlock(&rb->lock);

	return (0);
}

/*
 * Query buffer with filters
 */
int
ringbuf_query(struct log_ringbuf *rb, struct log_query *query,
	struct log_entry ***results, uint64_t *count)
{
	struct log_entry **res = NULL;
	uint64_t n = 0;
	uint64_t cursor = 0;
	struct log_entry e;

	if (rb == NULL || query == NULL || results == NULL || count == NULL)
		return (-1);

	*results = NULL;
	*count = 0;

	/*
	 * Each result is a heap-allocated COPY of the entry (ringbuf_iterate
	 * copies out under the lock), so the caller owns them: free every
	 * results[i] and then the array.
	 */
	while (ringbuf_iterate(rb, &cursor, &e) == 0) {
		struct log_entry *copy;

		/* Apply filters */
		if (query->start_time && e.timestamp < query->start_time)
			continue;
		if (query->end_time && e.timestamp > query->end_time)
			continue;
		if (query->severity_min && e.severity < query->severity_min)
			continue;
		if (query->severity_max && e.severity > query->severity_max)
			continue;

		void *_new = realloc(res, (n + 1) * sizeof(*res));
		if (_new == NULL)
			break;
		res = _new;
		copy = malloc(sizeof(*copy));
		if (copy == NULL)
			break;
		*copy = e;
		res[n++] = copy;

		if (query->limit && n >= query->limit)
			break;
	}

	*results = res;
	*count = n;

	return (0);
}

/*
 * Get oldest entry ID
 */
uint64_t
ringbuf_oldest_id(struct log_ringbuf *rb)
{
	uint64_t id = 0;

	if (rb == NULL)
		return (0);

	pthread_mutex_lock(&rb->lock);

	if (rb->count > 0 && rb->ids[rb->tail] != 0)
		id = rb->ids[rb->tail];

	pthread_mutex_unlock(&rb->lock);

	return (id);
}

/*
 * Get newest entry ID
 */
uint64_t
ringbuf_newest_id(struct log_ringbuf *rb)
{
	uint64_t id = 0;

	if (rb == NULL)
		return (0);

	pthread_mutex_lock(&rb->lock);

	if (rb->count > 0) {
		uint64_t pos = (rb->head + rb->size - 1) % rb->size;
		id = rb->ids[pos];
	}

	pthread_mutex_unlock(&rb->lock);

	return (id);
}
