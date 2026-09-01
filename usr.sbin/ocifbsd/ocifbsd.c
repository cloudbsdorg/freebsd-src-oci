/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by Klara, Inc. under sponsorship
 * from the FreeBSD Foundation.
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
 * FreeBSD Native OCI Runtime - CLI Entry Point
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "include/ocifbsd.h"
#include "image/pull.h"
#include "image/push.h"
#include "image/load.h"
#include "image/zfs_store.h"
#include "network/netcfg.h"
#include "src/jsonfmt.h"

/* Global verbosity flag */
static bool verbose = false;

/*
 * Global: pretty-print JSON output. On by default so human-facing output
 * (inspect, state) reads nicely; --compact / -c restores single-line JSON
 * for scripts and pipelines.
 */
static bool pretty = true;

/*
 * Emit a JSON document to stdout. With --pretty it is reformatted with
 * indentation for human reading; otherwise it is printed verbatim. A value
 * that does not parse as JSON is printed as-is so nothing is ever swallowed.
 */
static void
emit_json(const char *json)
{
	char *p = NULL;

	if (pretty)
		p = ocifbsd_json_pretty(json);
	if (p != NULL) {
		printf("%s\n", p);
		free(p);
	} else {
		printf("%s\n", json);
	}
}

/* Command options */
static struct cmd_options {
	int verbose;
	int help;
	int version;
} opt = {
	.verbose = 0,
	.help = 0,
	.version = 0,
};

/* Command usage */
static void
usage(const char *prog, const char *cmd)
{
	if (cmd == NULL) {
		fprintf(stderr, "Usage: %s [OPTIONS] COMMAND [ARGS...]\n\n", prog);
		fprintf(stderr, "FreeBSD Native OCI Runtime\n\n");
		fprintf(stderr, "Options:\n");
		fprintf(stderr, "  -v, --verbose    Enable verbose output\n");
		fprintf(stderr, "  -J, --pretty     Pretty-print JSON output (default)\n");
		fprintf(stderr, "  -c, --compact    Compact single-line JSON output (for scripts)\n");
		fprintf(stderr, "  -h, --help       Show this help message\n");
		fprintf(stderr, "  -V, --version    Show version information\n");
		fprintf(stderr, "\nCommands:\n");
		fprintf(stderr, "  create [--name N] [--image REF|bundle]  Create a container\n");
		fprintf(stderr, "  start <container-id>              Start a created container\n");
		fprintf(stderr, "  kill <container-id> [signal]      Send signal to container\n");
		fprintf(stderr, "  delete <container-id> [--force]   Delete a container\n");
		fprintf(stderr, "  state <container-id>              Show container state\n");
		fprintf(stderr, "  list                               List containers\n");
		fprintf(stderr, "  inspect <container-id>            Show container details\n");
		fprintf(stderr, "  run [--name N] [--image REF|bundle]     Create and start\n");
		fprintf(stderr, "  exec [--cwd D] <container-id> <cmd> [args]  Run command in container\n");
		fprintf(stderr, "  stop [--timeout S] <container-id> Gracefully stop (TERM, then KILL)\n");
		fprintf(stderr, "  pause <container-id>              Pause a running container\n");
		fprintf(stderr, "  resume <container-id>             Resume a paused container\n");
		fprintf(stderr, "  pull <reference> [--dry-run]      Resolve/pull OCI image reference\n");
		fprintf(stderr, "  push <reference>                  Push local image to its registry\n");
		fprintf(stderr, "  load [--name ref] <archive|dir>   Import a local OCI image archive\n");
		fprintf(stderr, "  images                            List local image store paths\n");
		fprintf(stderr, "  rmi <reference>                   Remove a local image store\n");
		fprintf(stderr, "  network <list|set> [args]         View/modify container network config\n");
		fprintf(stderr, "\nRun '%s help <command>' for more information on a command.\n",
		    prog);
	} else {
		fprintf(stderr, "Usage: %s %s\n", prog, cmd);
	}
}

static void
version(void)
{
	printf("ocifbsd version %s\n", OCIFBSD_VERSION);
	printf("FreeBSD OCI Runtime - %s\n", OCIFBSD_NAME);
}

/*
 * Reject an image reference component that could traverse outside — or
 * collapse onto — the image store. Any empty, ".", or ".." slash-separated
 * component, an absolute component, or a NULL/empty string is refused.
 *
 * The empty/"." cases matter as much as "..": `rmi alpine:.` or `rmi alpine:`
 * would otherwise resolve to the repository directory itself, and cmd_rmi
 * would rm_rf every tag under it as root. This mirrors image/load.c's
 * ref_part_safe() so both entry points enforce the same rule.
 */
static int
ref_component_is_safe(const char *s)
{
	const char *start, *p;

	if (s == NULL)
		return (1);	/* absent slot; zfs_image_path() rejects a NULL */
	if (s[0] == '\0' || s[0] == '/')
		return (0);
	for (start = s, p = s;; p++) {
		if (*p == '/' || *p == '\0') {
			size_t len = (size_t)(p - start);

			if (len == 0)
				return (0);			/* empty component */
			if (len == 1 && start[0] == '.')
				return (0);			/* "." */
			if (len == 2 && start[0] == '.' && start[1] == '.')
				return (0);			/* ".." */
			if (*p == '\0')
				break;
			start = p + 1;
		}
	}
	return (1);
}

/*
 * Resolve an image reference to a local store path (OCIFBSD_DATA_DIR layout).
 */
static char *
resolve_image_store(const char *ref)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;
	char *path = NULL;

	if (ref == NULL || ref[0] == '\0')
		return (NULL);
	if (parse_reference(ref, &registry, &repo, &tag, &digest) != 0)
		return (NULL);
	if (!ref_component_is_safe(registry) || !ref_component_is_safe(repo) ||
	    !ref_component_is_safe(tag)) {
		fprintf(stderr, "error: unsafe image reference: %s\n", ref);
		free(registry);
		free(repo);
		free(tag);
		free(digest);
		return (NULL);
	}
	path = zfs_image_path(registry, repo, tag);
	free(registry);
	free(repo);
	free(tag);
	free(digest);
	return (path);
}

/*
 * Ensure a pulled/local image store can be used as an OCI bundle.
 */
static int
image_store_ready(const char *store)
{
	char path[PATH_MAX];
	struct stat st;

	if (store == NULL)
		return (0);
	/* A truncated path would stat the wrong file; treat as not ready. */
	if ((size_t)snprintf(path, sizeof(path), "%s/config.json", store) >=
	    sizeof(path))
		return (0);
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return (0);
	if ((size_t)snprintf(path, sizeof(path), "%s/rootfs", store) >=
	    sizeof(path))
		return (0);
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		return (0);
	return (1);
}

/*
 * Parse a signal specification: a number ("15"), a name ("TERM"), or a
 * name with SIG prefix ("SIGTERM"), case-insensitive. Returns the signal
 * number, or -1 if unrecognized.
 */
static int
parse_signal(const char *spec)
{
	int i;

	if (spec == NULL || spec[0] == '\0')
		return (-1);
	if (isdigit((unsigned char)spec[0])) {
		i = atoi(spec);
		return (i > 0 && i < NSIG ? i : -1);
	}
	if (strncasecmp(spec, "SIG", 3) == 0)
		spec += 3;
	for (i = 1; i < NSIG; i++) {
		if (sys_signame[i] != NULL &&
		    strcasecmp(spec, sys_signame[i]) == 0)
			return (i);
	}
	return (-1);
}

/*
 * Recursively remove a directory tree without following symlinks in any path
 * component. This is defense-in-depth against a component being swapped to a
 * symlink mid-walk (a TOCTOU the earlier fts(3) + FTS_NOCHDIR walk left open,
 * since it built full paths and then acted on them). Every step is an *at()
 * syscall relative to an open directory fd — openat with O_NOFOLLOW|O_DIRECTORY
 * to descend, fdopendir + unlinkat/fchflags to act — so a raced symlink cannot
 * redirect a removal outside the tree. The walk stays on a single filesystem
 * (like the old FTS_XDEV): a child directory on a different device is not
 * descended into. Best-effort immutable-flag clearing (via fchflags on an
 * O_NOFOLLOW fd) mirrors the behavior needed to remove FreeBSD image rootfses,
 * whose files carry schg/uchg flags; it cannot succeed at securelevel >= 1,
 * which is a system policy the runtime does not override.
 */
static int
rm_rf_at(int parentfd, const char *name, dev_t top_dev)
{
	struct stat st;
	int ret = 0;

	if (fstatat(parentfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (errno == ENOENT ? 0 : -1);

	if (S_ISDIR(st.st_mode)) {
		if (st.st_dev == top_dev) {
			int fd = openat(parentfd, name,
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			DIR *d;
			struct dirent *e;

			if (fd < 0) {
				/* Raced to a symlink/non-dir since fstatat:
				 * fall through and remove it as a leaf. */
				if (errno == ELOOP || errno == ENOTDIR)
					goto leaf;
				fprintf(stderr, "error: cannot open %s: %s\n",
				    name, strerror(errno));
				return (-1);
			}
			/* Clear any immutable flag on the directory itself so
			 * its entries can be removed (a schg/uchg directory
			 * blocks deletion of the files inside it). */
			(void)fchflags(fd, 0);
			d = fdopendir(fd);
			if (d == NULL) {
				close(fd);
				return (-1);
			}
			while ((e = readdir(d)) != NULL) {
				if (strcmp(e->d_name, ".") == 0 ||
				    strcmp(e->d_name, "..") == 0)
					continue;
				if (rm_rf_at(fd, e->d_name, top_dev) != 0)
					ret = -1;
			}
			closedir(d);	/* also closes fd */
		}
		if (unlinkat(parentfd, name, AT_REMOVEDIR) != 0 &&
		    errno != ENOENT) {
			fprintf(stderr, "error: cannot remove dir %s: %s\n",
			    name, strerror(errno));
			ret = -1;
		}
		return (ret);
	}
leaf:
	if (unlinkat(parentfd, name, 0) != 0 && errno != ENOENT) {
		/*
		 * Immutable/append file: clear its flags via an O_NOFOLLOW fd
		 * (so a swapped symlink is not followed) and retry.
		 */
		if (errno == EPERM) {
			int lf = openat(parentfd, name,
			    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

			if (lf >= 0) {
				(void)fchflags(lf, 0);
				close(lf);
				if (unlinkat(parentfd, name, 0) == 0 ||
				    errno == ENOENT)
					return (0);
			}
		}
		fprintf(stderr, "error: cannot remove %s: %s\n", name,
		    strerror(errno));
		return (-1);
	}
	return (0);
}

/* Recursive remove for rmi (depth-first), symlink-safe (see rm_rf_at). */
static int
rm_rf(const char *path)
{
	char *dup, *slash;
	const char *parent, *leaf;
	struct stat st;
	dev_t top_dev;
	size_t n;
	int parentfd, ret;

	if (path == NULL || path[0] == '\0' || strcmp(path, "/") == 0) {
		errno = EINVAL;
		return (-1);
	}
	/* Anchor the walk to one filesystem via the top-level device. */
	if (lstat(path, &st) != 0)
		return (errno == ENOENT ? 0 : -1);
	top_dev = st.st_dev;

	/*
	 * Split into parent directory + leaf name and operate relative to the
	 * parent's fd, so even the leaf is reached through a single *at() call
	 * rather than a re-resolved path.
	 */
	dup = strdup(path);
	if (dup == NULL)
		return (-1);
	n = strlen(dup);
	while (n > 1 && dup[n - 1] == '/')	/* trim trailing slashes */
		dup[--n] = '\0';
	slash = strrchr(dup, '/');
	if (slash == dup) {			/* leaf directly under "/" */
		parent = "/";
		leaf = dup + 1;
	} else if (slash != NULL) {
		*slash = '\0';
		parent = dup;
		leaf = slash + 1;
	} else {				/* bare name, relative to cwd */
		parent = ".";
		leaf = dup;
	}
	parentfd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (parentfd < 0) {
		free(dup);
		return (-1);
	}
	ret = rm_rf_at(parentfd, leaf, top_dev);
	close(parentfd);
	free(dup);
	return (ret);
}

/* Command handlers */
static int
cmd_create(int argc, char **argv)
{
	const char *bundle = NULL;
	const char *name = NULL;
	const char *image_ref = NULL;
	struct ocifbsd_container *c;
	char *bundle_path = NULL;
	char *cname;
	int ret;
	int from_image = 0;

	/* Parse create-specific options */
	static struct option longopts[] = {
		{ "name",	required_argument,	NULL, 'n' },
		{ "image",	required_argument,	NULL, 'i' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	int ch;

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "n:i:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'i':
			image_ref = optarg;
			from_image = 1;
			break;
		case 'h':
			usage(argv[0],
			    "create [--name name] [--image ref | <bundle>]");
			return (0);
		default:
			usage(argv[0], "create");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (from_image) {
		bundle_path = resolve_image_store(image_ref);
		if (bundle_path == NULL) {
			fprintf(stderr, "error: invalid image reference: %s\n",
			    image_ref);
			return (1);
		}
		if (!image_store_ready(bundle_path)) {
			fprintf(stderr,
			    "error: image not ready (pull first): %s\n"
			    "  store=%s\n", image_ref, bundle_path);
			free(bundle_path);
			return (1);
		}
	} else {
		if (argc < 1) {
			fprintf(stderr,
			    "error: bundle path or --image required\n");
			usage("ocifbsd", "create");
			return (1);
		}
		bundle = argv[0];
		bundle_path = resolve_bundle_path(bundle);
		if (bundle_path == NULL) {
			fprintf(stderr, "error: invalid bundle path: %s\n",
			    bundle);
			return (1);
		}
	}

	/* Canonicalize name if provided */
	if (name != NULL) {
		cname = canonical_name(name);
		if (cname == NULL) {
			fprintf(stderr, "error: invalid container name: %s\n",
			    name);
			free(bundle_path);
			return (1);
		}
	} else {
		cname = NULL;
	}

	/* Create container */
	if (verbose) {
		fprintf(stderr, "Creating container from %s: %s\n",
		    from_image ? "image" : "bundle", bundle_path);
		if (cname)
			fprintf(stderr, "Container name: %s\n", cname);
	}

	ret = container_create(&c, bundle_path, cname);
	free(bundle_path);
	free(cname);

	if (ret != 0) {
		fprintf(stderr, "error: failed to create container: %s\n",
		    strerror(errno));
		return (1);
	}

	printf("%s\n", c->id);

	container_free(c);
	return (0);
}

static int
cmd_start(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;
	int ret;
	int lockfd;

	/* Parse start-specific options */
	static struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage(argv[0], "start <container-id>");
			return (0);
		default:
			usage(argv[0], "start");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage(argv[-optind], "start");
		return (1);
	}

	id = argv[0];

	/*
	 * Serialize this lifecycle op against other processes acting on the
	 * same container. Fail closed if the lock cannot be taken.
	 */
	lockfd = state_lock_container(id);
	if (lockfd < 0) {
		fprintf(stderr, "error: failed to lock container %s: %s\n",
		    id, strerror(errno));
		return (1);
	}

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		state_unlock_container(lockfd);
		return (1);
	}

	/* Start container */
	if (verbose)
		fprintf(stderr, "Starting container: %s\n", id);

	ret = container_start(c);
	if (ret != 0) {
		fprintf(stderr, "error: failed to start container: %s\n",
		    strerror(errno));
		container_free(c);
		state_unlock_container(lockfd);
		return (1);
	}

	printf("%s\n", c->id);

	container_free(c);
	state_unlock_container(lockfd);
	return (0);
}

static int
cmd_kill(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;
	int sig = SIGTERM;
	int ret;
	int lockfd;

	/* Parse kill-specific options */
	static struct option longopts[] = {
		{ "signal", required_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "s:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 's':
			sig = parse_signal(optarg);
			if (sig <= 0) {
				fprintf(stderr, "error: unknown signal: %s\n",
				    optarg);
				return (1);
			}
			break;
		case 'h':
			usage(argv[0], "kill <container-id> [--signal <signal>]");
			return (0);
		default:
			usage(argv[0], "kill");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage(argv[-optind], "kill");
		return (1);
	}

	id = argv[0];

	/*
	 * A positional signal (kill <id> TERM / kill <id> 9) is documented in
	 * the usage and accepted alongside --signal. Parse it here; a second,
	 * unrecognized operand is an error.
	 */
	if (argc >= 2) {
		int psig = parse_signal(argv[1]);

		if (psig <= 0) {
			fprintf(stderr, "error: unknown signal: %s\n", argv[1]);
			return (1);
		}
		sig = psig;
	}

	lockfd = state_lock_container(id);
	if (lockfd < 0) {
		fprintf(stderr, "error: failed to lock container %s: %s\n",
		    id, strerror(errno));
		return (1);
	}

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		state_unlock_container(lockfd);
		return (1);
	}

	/* Kill container */
	if (verbose)
		fprintf(stderr, "Sending signal %d to container: %s\n", sig, id);

	ret = container_kill(c, sig);
	if (ret != 0) {
		fprintf(stderr, "error: failed to send signal: %s\n",
		    strerror(errno));
		container_free(c);
		state_unlock_container(lockfd);
		return (1);
	}

	printf("%s\n", c->id);

	container_free(c);
	state_unlock_container(lockfd);
	return (0);
}

static int
cmd_delete(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;
	bool force = false;
	int ret;
	int lockfd;

	/* Parse delete-specific options */
	static struct option longopts[] = {
		{ "force", no_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "fh", longopts, NULL)) != -1) {
		switch (ch) {
		case 'f':
			force = true;
			break;
		case 'h':
			usage(argv[0], "delete <container-id> [--force]");
			return (0);
		default:
			usage(argv[0], "delete");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage(argv[-optind], "delete");
		return (1);
	}

	id = argv[0];

	lockfd = state_lock_container(id);
	if (lockfd < 0) {
		fprintf(stderr, "error: failed to lock container %s: %s\n",
		    id, strerror(errno));
		return (1);
	}

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		state_unlock_container(lockfd);
		return (1);
	}

	/* Stop if running and not forced */
	if (c->state == OCIFBSD_STATE_RUNNING && !force) {
		fprintf(stderr, "error: container is running, use --force to stop and delete\n");
		container_free(c);
		state_unlock_container(lockfd);
		return (1);
	}

	/* Delete container */
	if (verbose)
		fprintf(stderr, "Deleting container: %s\n", id);

	ret = container_delete(c);
	if (ret != 0) {
		fprintf(stderr, "error: failed to delete container: %s\n",
		    strerror(errno));
		container_free(c);
		state_unlock_container(lockfd);
		return (1);
	}

	printf("%s\n", id);

	container_free(c);
	state_unlock_container(lockfd);
	return (0);
}

static int
cmd_state(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;

	/* Parse state-specific options */
	static struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage(argv[0], "state <container-id>");
			return (0);
		default:
			usage(argv[0], "state");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage(argv[-optind], "state");
		return (1);
	}

	id = argv[0];

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		return (1);
	}

	/* Print state as JSON (pretty when --pretty). */
	{
		char buf[256];

		snprintf(buf, sizeof(buf), "{\"id\":\"%s\",\"status\":\"%s\"}",
		    c->id, ocifbsd_state_to_string(c->state));
		emit_json(buf);
	}

	container_free(c);
	return (0);
}

static int
cmd_list(int argc, char **argv)
{
	struct ocifbsd_container **list;
	int n, i;

	/* Parse list-specific options */
	static struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage(argv[0], "list");
			return (0);
		default:
			usage(argv[0], "list");
			return (1);
		}
	}

	/*
	 * Get all containers. state_list() returns NULL both for an empty
	 * store and on error; distinguish via errno so a listing failure is
	 * reported as a nonzero exit rather than looking like "no containers".
	 */
	errno = 0;
	list = state_list(&n);
	if (list == NULL) {
		if (errno != 0) {
			fprintf(stderr, "error: failed to list containers: %s\n",
			    strerror(errno));
			return (1);
		}
		return (0);	/* empty store */
	}

	/* Print header */
	printf("CONTAINER ID    NAME                STATUS\n");
	printf("--------------- ------------------- ---------------\n");

	/* Print each container */
	for (i = 0; i < n; i++) {
		printf("%-15s %-19s %s\n",
		    list[i]->id ? list[i]->id : "",
		    list[i]->name ? list[i]->name : "",
		    ocifbsd_state_to_string(list[i]->state));
		container_free(list[i]);
	}
	free(list);

	return (0);
}

static int
cmd_inspect(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;
	char *json;
	int ret;

	/* Parse inspect-specific options */
	static struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage(argv[0], "inspect <container-id>");
			return (0);
		default:
			usage(argv[0], "inspect");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage(argv[-optind], "inspect");
		return (1);
	}

	id = argv[0];

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		return (1);
	}

	/* Get inspect JSON */
	ret = container_inspect(c, &json);
	if (ret != 0) {
		fprintf(stderr, "error: failed to inspect container: %s\n",
		    strerror(errno));
		container_free(c);
		return (1);
	}

	emit_json(json);
	free(json);

	container_free(c);
	return (0);
}

static int
cmd_run(int argc, char **argv)
{
	const char *bundle = NULL;
	const char *name = NULL;
	const char *image_ref = NULL;
	struct ocifbsd_container *c;
	char *bundle_path = NULL;
	char *cname;
	int ret;
	int from_image = 0;

	/* Parse run-specific options */
	static struct option longopts[] = {
		{ "name",	required_argument,	NULL, 'n' },
		{ "image",	required_argument,	NULL, 'i' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "n:i:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'i':
			image_ref = optarg;
			from_image = 1;
			break;
		case 'h':
			usage(argv[0],
			    "run [--name name] [--image ref | <bundle>]");
			return (0);
		default:
			usage(argv[0], "run");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (from_image) {
		bundle_path = resolve_image_store(image_ref);
		if (bundle_path == NULL) {
			fprintf(stderr, "error: invalid image reference: %s\n",
			    image_ref);
			return (1);
		}
		if (!image_store_ready(bundle_path)) {
			fprintf(stderr,
			    "error: image not ready (pull first): %s\n"
			    "  store=%s\n", image_ref, bundle_path);
			free(bundle_path);
			return (1);
		}
	} else {
		if (argc < 1) {
			fprintf(stderr,
			    "error: bundle path or --image required\n");
			usage("ocifbsd", "run");
			return (1);
		}
		bundle = argv[0];
		bundle_path = resolve_bundle_path(bundle);
		if (bundle_path == NULL) {
			fprintf(stderr, "error: invalid bundle path: %s\n",
			    bundle);
			return (1);
		}
	}

	/* Canonicalize name if provided */
	if (name != NULL) {
		cname = canonical_name(name);
		if (cname == NULL) {
			fprintf(stderr, "error: invalid container name: %s\n",
			    name);
			free(bundle_path);
			return (1);
		}
	} else {
		cname = NULL;
	}

	/* Create container */
	if (verbose) {
		fprintf(stderr, "Creating container from %s: %s\n",
		    from_image ? "image" : "bundle", bundle_path);
		if (cname)
			fprintf(stderr, "Container name: %s\n", cname);
	}

	ret = container_create(&c, bundle_path, cname);
	free(bundle_path);
	free(cname);

	if (ret != 0) {
		fprintf(stderr, "error: failed to create container: %s\n",
		    strerror(errno));
		return (1);
	}

	/* Start container */
	if (verbose)
		fprintf(stderr, "Starting container: %s\n", c->id);

	ret = container_start(c);
	if (ret != 0) {
		/*
		 * `run` is create+start; the start failed, so tear the created
		 * container back down rather than leaving an orphaned container
		 * on disk that a later run could collide with.
		 */
		fprintf(stderr, "error: failed to start container %s: %s\n",
		    c->id, strerror(errno));
		container_delete(c);
		container_free(c);
		return (1);
	}

	printf("%s\n", c->id);

	container_free(c);
	return (0);
}

/*
 * pull — resolve an OCI reference; without --dry-run, fetch via
 * registry_pull(3) into the local store path.
 */
static int
cmd_pull(int argc, char **argv)
{
	const char *ref = NULL;
	int dry_run = 0;
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;
	char *store_path = NULL;
	struct registry reg;
	int ch, ret = 1;

	static struct option longopts[] = {
		{ "dry-run",	no_argument,		NULL, 'n' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "nh", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			dry_run = 1;
			break;
		case 'h':
			usage(argv[0], "pull [--dry-run] <reference>");
			return (0);
		default:
			usage(argv[0], "pull");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		usage("ocifbsd", "pull [--dry-run] <reference>");
		return (1);
	}
	ref = argv[0];

	if (parse_reference(ref, &registry, &repo, &tag, &digest) != 0) {
		fprintf(stderr, "error: invalid image reference: %s\n", ref);
		return (1);
	}
	if (!ref_component_is_safe(registry) || !ref_component_is_safe(repo) ||
	    !ref_component_is_safe(tag)) {
		fprintf(stderr, "error: unsafe image reference: %s\n", ref);
		goto out;
	}

	printf("reference=%s\n", ref);
	printf("registry=%s\n", registry ? registry : "");
	printf("repository=%s\n", repo ? repo : "");
	printf("tag=%s\n", tag ? tag : "");
	if (digest != NULL)
		printf("digest=%s\n", digest);

	store_path = zfs_image_path(registry, repo, tag);
	if (store_path == NULL) {
		fprintf(stderr, "error: cannot allocate store path\n");
		goto out;
	}
	printf("store_path=%s\n", store_path);

	if (dry_run) {
		ret = 0;
		goto out;
	}

	if (registry_init(&reg, ref) != 0) {
		fprintf(stderr, "error: registry_init failed for %s\n", ref);
		goto out;
	}

	if (verbose)
		fprintf(stderr, "pulling %s -> %s\n", ref, store_path);

	if (registry_pull(&reg, ref, store_path, NULL, NULL) != 0) {
		fprintf(stderr, "error: pull failed for %s\n", ref);
		registry_free(&reg);
		ret = 1;
		goto out;
	}

	registry_free(&reg);
	printf("status=ok\n");
	ret = 0;

out:
	free(registry);
	free(repo);
	free(tag);
	free(digest);
	free(store_path);
	return (ret);
}

/*
 * images — list local images by walking the store for directories that are
 * usable image roots (contain config.json and rootfs/). Each such directory
 * is <base>/<registry>/<repo...>/<tag>; the row shows registry/repo, the tag
 * (final component), and the path.
 */
static int
cmd_images(int argc, char **argv)
{
	const char *base;
	char basebuf[PATH_MAX];
	char *paths[2];
	FTS *fts;
	FTSENT *ent;
	int printed_header = 0;

	(void)argc;
	(void)argv;

	base = getenv("OCIFBSD_DATA_DIR");
	if (base == NULL || base[0] == '\0')
		base = OCIFBSD_DATA_DIR;
	/*
	 * Copy into basebuf and walk THAT. If the copy truncated, bail: we
	 * later strip the prefix from fts_path by length, and using the
	 * original (longer) length against the truncated walk path would read
	 * past the end of fts_path.
	 */
	if (strlcpy(basebuf, base, sizeof(basebuf)) >= sizeof(basebuf)) {
		fprintf(stderr, "error: OCIFBSD_DATA_DIR too long\n");
		return (1);
	}

	paths[0] = basebuf;
	paths[1] = NULL;
	fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if (fts == NULL) {
		if (errno == ENOENT)
			return (0);	/* empty store is success */
		fprintf(stderr, "error: cannot open %s: %s\n", base,
		    strerror(errno));
		return (1);
	}

	int walk_err = 0;

	errno = 0;
	while ((ent = fts_read(fts)) != NULL) {
		char cfg[PATH_MAX], rfs[PATH_MAX];
		struct stat st;
		const char *rel, *tag, *lastslash;

		/*
		 * A genuine read error mid-walk (FTS_ERR) must not look like a
		 * complete listing, so flag it. But a missing store root or an
		 * unreadable/cyclic subdir (FTS_NS/FTS_DNR/FTS_DC) is a
		 * can't-see-this-part condition — a listing tolerates it like
		 * ls(1): skip it without failing the whole command (this is also
		 * the empty/missing-store case, which must exit 0).
		 */
		if (ent->fts_info == FTS_ERR) {
			fprintf(stderr, "error: %s: %s\n", ent->fts_path,
			    strerror(ent->fts_errno));
			walk_err = 1;
			continue;
		}
		if (ent->fts_info == FTS_DNR || ent->fts_info == FTS_DC) {
			fprintf(stderr, "warning: %s: %s\n", ent->fts_path,
			    strerror(ent->fts_errno));
			continue;
		}

		if (ent->fts_info != FTS_D)
			continue;

		/* Is this directory a usable image root? */
		snprintf(cfg, sizeof(cfg), "%s/config.json", ent->fts_path);
		snprintf(rfs, sizeof(rfs), "%s/rootfs", ent->fts_path);
		if (stat(cfg, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (stat(rfs, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;

		/* repo/tag are relative to base. Strip the prefix using the
		 * length of the string we actually walked (basebuf), then split
		 * off the final component as the tag. */
		rel = ent->fts_path + strlen(basebuf);
		while (*rel == '/')
			rel++;
		lastslash = strrchr(rel, '/');
		tag = (lastslash != NULL) ? lastslash + 1 : rel;

		if (!printed_header) {
			printf("REPOSITORY\tTAG\tPATH\n");
			printed_header = 1;
		}
		if (lastslash != NULL)
			printf("%.*s\t%s\t%s\n", (int)(lastslash - rel), rel,
			    tag, ent->fts_path);
		else
			printf("%s\t%s\t%s\n", rel, tag, ent->fts_path);

		/* Don't descend into an image's own rootfs. */
		fts_set(fts, ent, FTS_SKIP);
		errno = 0;
	}
	if (errno != 0) {	/* fts_read() aborted the walk */
		fprintf(stderr, "error: store walk failed: %s\n",
		    strerror(errno));
		walk_err = 1;
	}
	fts_close(fts);
	return (walk_err ? 1 : 0);
}

/*
 * rmi — remove a local image store directory for a reference.
 */
static int
cmd_rmi(int argc, char **argv)
{
	const char *ref = NULL;
	char *store_path = NULL;
	struct stat st;
	bool force = false;
	int ch;

	static struct option longopts[] = {
		{ "force",	no_argument,	NULL, 'f' },
		{ "help",	no_argument,	NULL, 'h' },
		{ NULL,		0,		NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "fh", longopts, NULL)) != -1) {
		switch (ch) {
		case 'f':
			force = true;
			break;
		case 'h':
			usage(argv[0], "rmi [--force] <reference>");
			return (0);
		default:
			usage(argv[0], "rmi");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		usage("ocifbsd", "rmi <reference>");
		return (1);
	}
	ref = argv[0];

	store_path = resolve_image_store(ref);
	if (store_path == NULL) {
		fprintf(stderr, "error: invalid image reference: %s\n", ref);
		return (1);
	}
	if (stat(store_path, &st) != 0) {
		fprintf(stderr, "error: image not found: %s\n", ref);
		free(store_path);
		return (1);
	}

	/*
	 * Refuse to remove an image that a container still references, unless
	 * forced: a container created with --image records the image store as
	 * its bundle_path, and pulling the store out from under it would delete
	 * that container's rootfs while it may still be running.
	 */
	if (!force) {
		int n = 0, i;
		struct ocifbsd_container **list = state_list(&n);
		char *inuse = NULL;

		for (i = 0; list != NULL && i < n; i++) {
			if (inuse == NULL && list[i]->bundle_path != NULL &&
			    strcmp(list[i]->bundle_path, store_path) == 0)
				inuse = strdup(list[i]->id);
			container_free(list[i]);
		}
		free(list);
		if (inuse != NULL) {
			fprintf(stderr, "error: image %s is in use by container "
			    "%s (use --force to remove anyway)\n", ref, inuse);
			free(inuse);
			free(store_path);
			return (1);
		}
	}
	if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "error: not an image store: %s\n", store_path);
		free(store_path);
		return (1);
	}

	if (verbose)
		fprintf(stderr, "removing image store %s\n", store_path);

	if (rm_rf(store_path) != 0) {
		fprintf(stderr, "error: failed to remove %s\n", store_path);
		free(store_path);
		return (1);
	}
	printf("deleted=%s\n", store_path);
	free(store_path);
	return (0);
}

/*
 * exec — run a command inside a running container.
 */
static int
cmd_exec(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;
	const char *cwd = NULL;
	int ch, ret;

	static struct option longopts[] = {
		{ "cwd",	required_argument,	NULL, 'w' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "+w:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'w':
			cwd = optarg;
			break;
		case 'h':
			usage(argv[0],
			    "exec [--cwd dir] <container-id> <command> [args...]");
			return (0);
		default:
			usage(argv[0], "exec");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 2) {
		fprintf(stderr, "error: container id and command required\n");
		usage("ocifbsd",
		    "exec [--cwd dir] <container-id> <command> [args...]");
		return (1);
	}

	id = argv[0];
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		return (1);
	}

	if (verbose)
		fprintf(stderr, "Executing in container %s: %s\n", id, argv[1]);

	ret = container_exec(c, argv + 1, cwd);
	if (ret < 0) {
		fprintf(stderr, "error: exec failed: %s\n", strerror(errno));
		container_free(c);
		return (1);
	}

	container_free(c);
	return (ret);
}

/*
 * stop — graceful shutdown: SIGTERM, wait, then SIGKILL.
 */
static int
cmd_stop(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;
	int timeout = 10;
	int ch, ret;
	int lockfd;

	static struct option longopts[] = {
		{ "timeout",	required_argument,	NULL, 't' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "t:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 't': {
			char *end = NULL;
			long v = strtol(optarg, &end, 10);

			/* Reject junk and cap the value: container_stop converts
			 * seconds to milliseconds in an int, so an unbounded
			 * timeout would overflow and skip the graceful wait. */
			if (end == optarg || *end != '\0' || v <= 0 ||
			    v > 86400) {
				fprintf(stderr, "error: invalid timeout: %s "
				    "(1..86400 seconds)\n", optarg);
				return (1);
			}
			timeout = (int)v;
			break;
		}
		case 'h':
			usage(argv[0],
			    "stop [--timeout sec] <container-id>");
			return (0);
		default:
			usage(argv[0], "stop");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage("ocifbsd", "stop [--timeout sec] <container-id>");
		return (1);
	}

	id = argv[0];

	lockfd = state_lock_container(id);
	if (lockfd < 0) {
		fprintf(stderr, "error: failed to lock container %s: %s\n",
		    id, strerror(errno));
		return (1);
	}

	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		state_unlock_container(lockfd);
		return (1);
	}

	if (verbose)
		fprintf(stderr, "Stopping container: %s\n", id);

	ret = container_stop(c, timeout);
	if (ret != 0) {
		fprintf(stderr, "error: failed to stop container: %s\n",
		    strerror(errno));
		container_free(c);
		state_unlock_container(lockfd);
		return (1);
	}

	printf("%s\n", c->id);
	container_free(c);
	state_unlock_container(lockfd);
	return (0);
}

/*
 * pause/resume — freeze/thaw the container init process.
 */
static int
cmd_pause_resume(int argc, char **argv, bool do_pause)
{
	struct ocifbsd_container *c;
	const char *id;
	const char *name = do_pause ? "pause" : "resume";
	int ch, ret;
	int lockfd;

	static struct option longopts[] = {
		{ "help",	no_argument,	NULL, 'h' },
		{ NULL,		0,		NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
		default:
			usage(argv[0], do_pause ?
			    "pause <container-id>" : "resume <container-id>");
			return (ch == 'h' ? 0 : 1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		return (1);
	}

	id = argv[0];

	lockfd = state_lock_container(id);
	if (lockfd < 0) {
		fprintf(stderr, "error: failed to lock container %s: %s\n",
		    id, strerror(errno));
		return (1);
	}

	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		state_unlock_container(lockfd);
		return (1);
	}

	if (verbose)
		fprintf(stderr, "%s container: %s\n", name, id);

	ret = do_pause ? container_pause(c) : container_resume(c);
	if (ret != 0) {
		fprintf(stderr, "error: failed to %s container: %s\n",
		    name, strerror(errno));
		container_free(c);
		state_unlock_container(lockfd);
		return (1);
	}

	printf("%s\n", c->id);
	container_free(c);
	state_unlock_container(lockfd);
	return (0);
}

static int
cmd_pause(int argc, char **argv)
{
	return (cmd_pause_resume(argc, argv, true));
}

static int
cmd_resume(int argc, char **argv)
{
	return (cmd_pause_resume(argc, argv, false));
}

/*
 * push — upload a local image store to its registry.
 */
static int
cmd_push(int argc, char **argv)
{
	const char *ref = NULL;
	char *store_path = NULL;
	struct registry reg;
	int ch, ret = 1;

	static struct option longopts[] = {
		{ "help",	no_argument,	NULL, 'h' },
		{ NULL,		0,		NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage(argv[0], "push <reference>");
			return (0);
		default:
			usage(argv[0], "push");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		usage("ocifbsd", "push <reference>");
		return (1);
	}
	ref = argv[0];

	store_path = resolve_image_store(ref);
	if (store_path == NULL) {
		fprintf(stderr, "error: invalid image reference: %s\n", ref);
		return (1);
	}
	if (!image_store_ready(store_path)) {
		fprintf(stderr, "error: image not found locally: %s\n"
		    "  store=%s\n", ref, store_path);
		free(store_path);
		return (1);
	}

	if (registry_init(&reg, ref) != 0) {
		fprintf(stderr, "error: registry_init failed for %s\n", ref);
		free(store_path);
		return (1);
	}

	if (verbose)
		fprintf(stderr, "pushing %s <- %s\n", ref, store_path);

	if (push_image(&reg, ref, store_path, NULL, NULL) != 0) {
		fprintf(stderr, "error: push failed for %s\n", ref);
		registry_free(&reg);
		free(store_path);
		return (1);
	}

	registry_free(&reg);
	printf("status=ok\n");
	ret = 0;
	free(store_path);
	return (ret);
}

/*
 * load — import a local OCI image archive (oci-layout dir or .txz/.tar) into
 * the image store so `create --image`/`run --image` can use it.
 */
static int
cmd_load(int argc, char **argv)
{
	const char *archive = NULL;
	const char *ref = NULL;
	char *store_path = NULL;
	int ch;

	static struct option longopts[] = {
		{ "name",	required_argument,	NULL, 'n' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "n:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			ref = optarg;
			break;
		case 'h':
			usage(argv[0], "load [--name ref] <oci-archive|dir>");
			return (0);
		default:
			usage(argv[0], "load");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		usage("ocifbsd", "load [--name ref] <oci-archive|dir>");
		return (1);
	}
	archive = argv[0];

	if (verbose)
		fprintf(stderr, "loading OCI archive %s%s%s\n", archive,
		    ref ? " as " : "", ref ? ref : "");

	if (load_oci_archive(archive, ref, &store_path) != 0) {
		fprintf(stderr, "error: failed to load %s\n", archive);
		return (1);
	}

	printf("loaded=%s\n", store_path ? store_path : "");
	free(store_path);
	return (0);
}

/*
 * Build the path to a container's persisted network configuration:
 * <DATA_DIR>/networks/<id>.json. Returns 0 on success.
 */
static int
network_config_path(const char *id, char *buf, size_t len)
{
	const char *base;

	base = getenv("OCIFBSD_DATA_DIR");
	if (base == NULL || base[0] == '\0')
		base = OCIFBSD_DATA_DIR;
	if ((size_t)snprintf(buf, len, "%s/networks/%s.json", base, id) >= len) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	return (0);
}

/*
 * Resolve an id-or-name argument to a container by scanning on-disk state
 * (container_get_by_name only consults the in-memory registry, which is
 * empty for a fresh CLI invocation). Matches an exact name, an exact id, or
 * an unambiguous id prefix of at least 6 hex digits. The returned container
 * is owned by the caller, which frees it with container_free().
 */
static struct ocifbsd_container *
network_resolve(const char *ref)
{
	struct ocifbsd_container **list;
	struct ocifbsd_container *match = NULL;
	size_t reflen;
	int n, i;

	if (ref == NULL)
		return (NULL);
	list = state_list(&n);
	if (list == NULL || n <= 0) {
		free(list);
		return (NULL);
	}
	reflen = strlen(ref);
	for (i = 0; i < n; i++) {
		struct ocifbsd_container *c = list[i];
		bool hit = false;

		if (c->name != NULL && strcmp(c->name, ref) == 0)
			hit = true;
		else if (c->id != NULL && (strcmp(c->id, ref) == 0 ||
		    (reflen >= 6 && strncmp(c->id, ref, reflen) == 0)))
			hit = true;

		if (hit && match == NULL)
			match = c;
		else
			container_free(c);
	}
	free(list);
	return (match);
}

static void
network_print_list(const char *label, char **arr, size_t n)
{
	size_t i;

	printf("  %-10s ", label);
	if (n == 0) {
		printf("(none)\n");
		return;
	}
	for (i = 0; i < n; i++)
		printf("%s%s", arr[i], (i + 1 < n) ? ", " : "");
	printf("\n");
}

/* Print one container's network configuration in human-readable form. */
static void
network_show(struct ocifbsd_container *c)
{
	char path[PATH_MAX];
	struct netcfg nc;
	char *json;
	size_t jlen;

	netcfg_init(&nc);
	if (network_config_path(c->id, path, sizeof(path)) == 0) {
		json = read_file(path, &jlen);
		if (json != NULL) {
			(void)netcfg_parse(json, &nc);
			free(json);
		}
	}

	printf("CONTAINER: %s (id %.12s)\n",
	    c->name ? c->name : "(unnamed)", c->id);
	printf("  %-10s %s\n", "vnet:",
	    nc.vnet == 1 ? "enabled" : nc.vnet == 0 ? "disabled" : "(unset)");
	network_print_list("ip4:", nc.ip4, nc.n_ip4);
	network_print_list("ip6:", nc.ip6, nc.n_ip6);
	printf("  %-10s %s\n", "gateway4:",
	    nc.gateway4 ? nc.gateway4 : "(none)");
	printf("  %-10s %s\n", "gateway6:",
	    nc.gateway6 ? nc.gateway6 : "(none)");
	network_print_list("dns:", nc.dns, nc.n_dns);
	netcfg_free(&nc);
}

/*
 * network list [<container>] — show one or all containers' network config.
 * Requires view privilege (root or the admin group).
 */
static int
cmd_network_list(int argc, char **argv)
{
	struct ocifbsd_container *c;

	if (ocifbsd_require_access(OCIFBSD_OP_VIEW) != 0) {
		fprintf(stderr, "error: permission denied: viewing container "
		    "network configuration requires root or the %s group\n",
		    OCIFBSD_ADMIN_GROUP);
		return (1);
	}

	if (argc >= 1) {
		c = network_resolve(argv[0]);
		if (c == NULL) {
			fprintf(stderr, "error: no such container: %s\n",
			    argv[0]);
			return (1);
		}
		network_show(c);
		container_free(c);
		return (0);
	}

	/* No argument: list every known container. */
	{
		struct ocifbsd_container **list;
		int n, i;

		list = state_list(&n);
		if (list == NULL || n <= 0) {
			free(list);
			return (0);
		}
		for (i = 0; i < n; i++) {
			network_show(list[i]);
			if (i + 1 < n)
				printf("\n");
			container_free(list[i]);
		}
		free(list);
	}
	return (0);
}

/*
 * network set <container> [options] — modify persisted network config.
 * Requires modify privilege (root or the admin group).
 */
static int
cmd_network_set(int argc, char **argv)
{
	struct ocifbsd_container *c;
	struct netcfg nc;
	char path[PATH_MAX], dir[PATH_MAX];
	const char *base, *id;
	char *json;
	size_t jlen;
	int ch, ret = 1;
	int lockfd = -1;
	bool changed = false;

	enum {
		OPT_VNET = 256, OPT_IP4, OPT_IP6, OPT_GW4, OPT_GW6, OPT_DNS,
		OPT_CLEAR_IP4, OPT_CLEAR_IP6, OPT_CLEAR_DNS, OPT_CLEAR
	};
	static struct option longopts[] = {
		{ "vnet",	required_argument,	NULL, OPT_VNET },
		{ "ip4",	required_argument,	NULL, OPT_IP4 },
		{ "ip6",	required_argument,	NULL, OPT_IP6 },
		{ "gateway4",	required_argument,	NULL, OPT_GW4 },
		{ "gateway6",	required_argument,	NULL, OPT_GW6 },
		{ "dns",	required_argument,	NULL, OPT_DNS },
		{ "clear-ip4",	no_argument,		NULL, OPT_CLEAR_IP4 },
		{ "clear-ip6",	no_argument,		NULL, OPT_CLEAR_IP6 },
		{ "clear-dns",	no_argument,		NULL, OPT_CLEAR_DNS },
		{ "clear",	no_argument,		NULL, OPT_CLEAR },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	if (ocifbsd_require_access(OCIFBSD_OP_MODIFY) != 0) {
		fprintf(stderr, "error: permission denied: modifying container "
		    "network configuration requires root or the %s group\n",
		    OCIFBSD_ADMIN_GROUP);
		return (1);
	}

	if (argc < 1) {
		usage("ocifbsd", "network set <container> [options]");
		return (1);
	}
	c = network_resolve(argv[0]);
	if (c == NULL) {
		fprintf(stderr, "error: no such container: %s\n", argv[0]);
		return (1);
	}
	id = c->id;

	/*
	 * Hold the per-container lock across the load-modify-write-apply
	 * sequence so a concurrent lifecycle op (e.g. start) cannot race the
	 * jail rebuild in container_reconfigure_network. Fail closed.
	 */
	lockfd = state_lock_container(id);
	if (lockfd < 0) {
		fprintf(stderr, "error: failed to lock container %s: %s\n",
		    id, strerror(errno));
		container_free(c);
		return (1);
	}

	/* Load any existing config so options accumulate onto it. */
	netcfg_init(&nc);
	if (network_config_path(id, path, sizeof(path)) != 0) {
		fprintf(stderr, "error: network config path too long\n");
		state_unlock_container(lockfd);
		container_free(c);
		return (1);
	}
	json = read_file(path, &jlen);
	if (json != NULL) {
		(void)netcfg_parse(json, &nc);
		free(json);
	}

	/* getopt consumes from argv[0]; keep the container name at argv[0]. */
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "+h", longopts, NULL)) != -1) {
		int rc = 0;

		switch (ch) {
		case OPT_VNET:
			if (strcasecmp(optarg, "on") == 0 ||
			    strcasecmp(optarg, "true") == 0 ||
			    strcmp(optarg, "1") == 0)
				rc = netcfg_set_vnet(&nc, true);
			else if (strcasecmp(optarg, "off") == 0 ||
			    strcasecmp(optarg, "false") == 0 ||
			    strcmp(optarg, "0") == 0)
				rc = netcfg_set_vnet(&nc, false);
			else {
				fprintf(stderr, "error: --vnet expects "
				    "on|off\n");
				goto out;
			}
			break;
		case OPT_IP4:
			rc = netcfg_add_ip4(&nc, optarg);
			break;
		case OPT_IP6:
			rc = netcfg_add_ip6(&nc, optarg);
			break;
		case OPT_GW4:
			rc = netcfg_set_gateway4(&nc, optarg);
			break;
		case OPT_GW6:
			rc = netcfg_set_gateway6(&nc, optarg);
			break;
		case OPT_DNS:
			rc = netcfg_add_dns(&nc, optarg);
			break;
		case OPT_CLEAR_IP4:
			netcfg_clear_ip4(&nc);
			break;
		case OPT_CLEAR_IP6:
			netcfg_clear_ip6(&nc);
			break;
		case OPT_CLEAR_DNS:
			netcfg_clear_dns(&nc);
			break;
		case OPT_CLEAR:
			netcfg_clear_ip4(&nc);
			netcfg_clear_ip6(&nc);
			netcfg_clear_dns(&nc);
			break;
		case 'h':
			usage(argv[0], "network set <container> "
			    "[--vnet on|off] [--ip4 CIDR] [--ip6 CIDR] "
			    "[--gateway4 ADDR] [--gateway6 ADDR] [--dns ADDR] "
			    "[--clear|--clear-ip4|--clear-ip6|--clear-dns]");
			ret = 0;
			goto out;
		default:
			usage(argv[0], "network set");
			goto out;
		}
		if (rc != 0) {
			fprintf(stderr, "error: invalid value: %s\n",
			    optarg ? optarg : "(none)");
			goto out;
		}
		changed = true;
	}

	if (!changed) {
		fprintf(stderr, "error: nothing to set (see 'network set -h')\n");
		goto out;
	}

	/* Ensure the per-runtime networks directory exists and is private. */
	base = getenv("OCIFBSD_DATA_DIR");
	if (base == NULL || base[0] == '\0')
		base = OCIFBSD_DATA_DIR;
	if ((size_t)snprintf(dir, sizeof(dir), "%s/networks", base) >=
	    sizeof(dir)) {
		fprintf(stderr, "error: networks dir path too long\n");
		goto out;
	}
	if (ensure_directory(dir, OCIFBSD_STATE_DIR_MODE) != 0) {
		fprintf(stderr, "error: cannot create %s: %s\n", dir,
		    strerror(errno));
		goto out;
	}
	ocifbsd_secure_path(dir, OCIFBSD_STATE_DIR_MODE);

	json = netcfg_to_json(&nc);
	if (json == NULL) {
		fprintf(stderr, "error: cannot serialize network config\n");
		goto out;
	}
	if (write_file(path, json, strlen(json)) != 0) {
		fprintf(stderr, "error: cannot write %s: %s\n", path,
		    strerror(errno));
		free(json);
		goto out;
	}
	free(json);
	ocifbsd_secure_path(path, OCIFBSD_STATE_FILE_MODE);

	printf("updated network configuration for %s\n",
	    c->name ? c->name : id);

	/*
	 * Apply immediately when it is safe to do so. A created-but-not-started
	 * container has an empty jail we can rebuild in place; a running or
	 * paused container is left untouched (its processes must not be
	 * disrupted) and the change takes effect on its next start.
	 */
	if (container_reconfigure_network(c) == 0) {
		printf("applied to jail (container is not running)\n");
	} else if (errno == EBUSY) {
		printf("stored; restart the container to apply "
		    "(it is currently running)\n");
	} else {
		fprintf(stderr, "warning: could not apply to jail now: %s\n",
		    strerror(errno));
	}

	network_show(c);
	ret = 0;
out:
	netcfg_free(&nc);
	state_unlock_container(lockfd);
	container_free(c);
	return (ret);
}

/*
 * network — list and modify container network configuration.
 */
static int
cmd_network(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr,
		    "usage: ocifbsd network <list|set> [args]\n");
		return (1);
	}
	if (strcmp(argv[1], "list") == 0)
		return (cmd_network_list(argc - 2, argv + 2));
	if (strcmp(argv[1], "set") == 0)
		return (cmd_network_set(argc - 2, argv + 2));
	fprintf(stderr, "error: unknown network subcommand: %s\n", argv[1]);
	fprintf(stderr, "usage: ocifbsd network <list|set> [args]\n");
	return (1);
}

/* Command table */
struct command {
	const char *name;
	int (*func)(int argc, char **argv);
	const char *help;
};

static struct command commands[] = {
	{ "create",	cmd_create,	"Create a container from OCI bundle" },
	{ "start",	cmd_start,	"Start a created container" },
	{ "kill",	cmd_kill,	"Send signal to container" },
	{ "delete",	cmd_delete,	"Delete a container" },
	{ "state",	cmd_state,	"Show container state" },
	{ "list",	cmd_list,	"List containers" },
	{ "inspect",	cmd_inspect,	"Show container details" },
	{ "run",	cmd_run,	"Create and start container" },
	{ "exec",	cmd_exec,	"Run a command in a running container" },
	{ "stop",	cmd_stop,	"Gracefully stop a container" },
	{ "pause",	cmd_pause,	"Pause a running container" },
	{ "resume",	cmd_resume,	"Resume a paused container" },
	{ "pull",	cmd_pull,	"Resolve or pull an OCI image" },
	{ "push",	cmd_push,	"Push a local image to a registry" },
	{ "load",	cmd_load,	"Import a local OCI image archive" },
	{ "images",	cmd_images,	"List local image store" },
	{ "rmi",	cmd_rmi,	"Remove a local image" },
	{ "network",	cmd_network,	"List/modify container network config" },
	{ NULL,		NULL,		NULL },
};

int
main(int argc, char **argv)
{
	struct command *cmd;

	/* Parse global options */
	static struct option longopts[] = {
		{ "verbose",	no_argument,		NULL, 'v' },
		{ "pretty",	no_argument,		NULL, 'J' },
		{ "compact",	no_argument,		NULL, 'c' },
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ NULL,		0,			NULL, 0 }
	};

	int ch;
	/*
	 * Leading '+' is POSIXLY_CORRECT: stop at the first non-option so
	 * subcommands keep their own flags (e.g. create --name ...).
	 */
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "+vhVJc", longopts, NULL)) != -1) {
		switch (ch) {
		case 'v':
			verbose = true;
			ocifbsd_set_verbose(true);
			break;
		case 'J':
			pretty = true;
			break;
		case 'c':
			pretty = false;
			break;
		case 'h':
			opt.help = 1;
			usage(argv[0], NULL);
			return (0);
		case 'V':
			opt.version = 1;
			version();
			return (0);
		default:
			usage(argv[0], NULL);
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage(argv[-optind], NULL);
		return (1);
	}

	/* Find and execute command */
	for (cmd = commands; cmd->name != NULL; cmd++) {
		if (strcmp(argv[0], cmd->name) == 0) {
			return (cmd->func(argc, argv));
		}
	}

	/* Unknown command */
	fprintf(stderr, "error: unknown command: %s\n\n", argv[0]);
	fprintf(stderr, "Available commands:");
	for (cmd = commands; cmd->name != NULL; cmd++) {
		fprintf(stderr, " %s", cmd->name);
	}
	fprintf(stderr, "\n\n");
	usage(argv[-optind], NULL);

	return (1);
}
