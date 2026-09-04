/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Concurrency test for the IPAM allocator: N processes each allocate one
 * address from the same range at the same instant. Every address handed out
 * must be distinct. Before the bitmap read-modify-write was performed under an
 * exclusive flock(2) on the map file, racing allocators all observed the same
 * free bit and returned the same address (a lost update); this test failed
 * with duplicates. It is the red-green witness for that fix.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <err.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "network.h"

#define	NPROC	16

int
main(void)
{
	struct ipam_range range;
	uint32_t *slot, *go;
	int i, dup, status, failures;
	pid_t pid;

	memset(&range, 0, sizeof(range));
	/* A throwaway range so the test never collides with a real network. */
	if (inet_pton(AF_INET, "10.253.253.0", &range.start) != 1 ||
	    inet_pton(AF_INET, "10.253.253.255", &range.end) != 1)
		errx(1, "inet_pton");
	range.prefix_len = 24;

	/* Start from a clean map so the run is repeatable. */
	{
		char path[256];

		snprintf(path, sizeof(path),
		    "/var/run/ocifbsd/networks/ipam-%08x.map",
		    ntohl(range.start.s_addr));
		(void)unlink(path);
	}

	/*
	 * Shared page: slot[0..NPROC-1] hold each child's allocation, and the
	 * word past them is the start barrier. Forking is sequential and each
	 * allocation is sub-millisecond, so without a barrier the children
	 * simply do not overlap and the race never fires — the test has to
	 * release them all at the same instant to be a real witness.
	 */
	slot = mmap(NULL, sizeof(uint32_t) * (NPROC + 1),
	    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (slot == MAP_FAILED)
		err(1, "mmap");
	memset(slot, 0, sizeof(uint32_t) * (NPROC + 1));
	go = &slot[NPROC];

	for (i = 0; i < NPROC; i++) {
		pid = fork();
		if (pid < 0)
			err(1, "fork");
		if (pid == 0) {
			struct in_addr a;

			while (atomic_load((_Atomic uint32_t *)go) == 0)
				;		/* spin until released */
			if (ipam_alloc(&range, &a) != 0)
				_exit(1);
			slot[i] = a.s_addr;
			_exit(0);
		}
	}
	/* Let every child reach the spin, then release them together. */
	usleep(200000);
	atomic_store((_Atomic uint32_t *)go, 1);
	failures = 0;
	for (i = 0; i < NPROC; i++) {
		if (wait(&status) < 0)
			err(1, "wait");
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			failures++;
	}
	if (failures != 0)
		errx(1, "FAIL: %d allocator(s) returned an error", failures);

	dup = 0;
	for (i = 0; i < NPROC; i++) {
		int j;

		if (slot[i] == 0) {
			printf("FAIL: slot %d never got an address\n", i);
			dup++;
			continue;
		}
		for (j = i + 1; j < NPROC; j++) {
			if (slot[i] == slot[j]) {
				struct in_addr a;
				char buf[INET_ADDRSTRLEN];

				a.s_addr = slot[i];
				printf("FAIL: %s handed to both %d and %d\n",
				    inet_ntop(AF_INET, &a, buf, sizeof(buf)),
				    i, j);
				dup++;
			}
		}
	}
	if (dup != 0) {
		printf("ipam_race_test: FAILED (%d duplicate/missing)\n", dup);
		return (1);
	}
	printf("ipam_race_test: PASSED (%d concurrent allocations, "
	    "all distinct)\n", NPROC);
	return (0);
}
