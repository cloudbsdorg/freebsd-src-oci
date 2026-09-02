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
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <netdb.h>
#include <poll.h>
#include <inttypes.h>

#include "image/pull.h"
#include "image/push.h"
#include "image/load.h"
#include "image/build.h"
#include "image/zfs_store.h"
#include "network/netcfg.h"
#include "network/network.h"
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

/*
 * Command usage. The program is always invoked as "ocifbsd", so the name is
 * fixed here rather than taken from argv[0]: subcommand handlers call
 * usage("start <container-id>") with just the command string, and previously
 * passing argv[0] (which for a subcommand is the subcommand name) produced a
 * doubled "Usage: start start ..." line.
 */
static void
usage(const char *cmd)
{
	static const char *const prog = "ocifbsd";

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
		fprintf(stderr, "  network <create|ls|rm|inspect|list|set>  Manage networks / container net config\n");
		fprintf(stderr, "  pod <create|list|delete|logs>     Manage pods (orchestration)\n");
		fprintf(stderr, "  service <create|scale|...>        Manage services (orchestration)\n");
		fprintf(stderr, "  stack <create|up|...>             Manage stacks (orchestration)\n");
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

/* Resolve a container spec (ID or name) to its canonical ID; see definition. */
static const char *resolve_cid(const char *spec);

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
 * Backing container-lifecycle shims for the orchestration library.
 *
 * The orchestration module (pods, stacks, services) drives containers through
 * these ocifbsd_*_container() entry points. In the standalone library they are
 * -1 stubs (orchestration/stubs.c); linked into this binary these real
 * definitions take precedence and bridge to the container_* runtime, giving
 * orchestration an actual backing runtime.
 *
 * image may be an image reference resolved through the local store, or a path
 * to an OCI bundle directory. command/args/pod_id are advisory only — the
 * process to run comes from the bundle's config.json — and are accepted for
 * API compatibility. The prototypes come from <ocifbsd.h>.
 */
int
ocifbsd_create_container(const char *name, const char *image,
	const char *command, const char *args, const char *pod_id,
	char **container_id)
{
	struct ocifbsd_container *c = NULL;
	char *store = NULL;
	const char *bundle = NULL;
	int ret;

	(void)command;
	(void)args;
	(void)pod_id;

	if (image == NULL)
		return (-1);

	/* Prefer a local image store; fall back to a bundle directory path. */
	store = resolve_image_store(image);
	if (store != NULL && image_store_ready(store)) {
		bundle = store;
	} else if (image_store_ready(image)) {
		bundle = image;
	} else {
		fprintf(stderr, "error: no usable image store or bundle for "
		    "'%s'\n", image);
		free(store);
		return (-1);
	}

	ret = container_create(&c, bundle, name);
	free(store);
	if (ret != 0 || c == NULL)
		return (-1);

	if (container_id != NULL)
		*container_id = (c->id != NULL) ? strdup(c->id) : NULL;
	container_free(c);
	return (0);
}

int
ocifbsd_start_container(const char *container_id)
{
	struct ocifbsd_container *c;
	int ret;

	if (container_id == NULL)
		return (-1);
	c = container_get_by_id(container_id);
	if (c == NULL)
		return (-1);
	ret = container_start(c);
	container_free(c);
	return (ret);
}

int
ocifbsd_stop_container(const char *cid, int sig)
{
	struct ocifbsd_container *c;
	int ret;

	if (cid == NULL)
		return (-1);
	c = container_get_by_id(cid);
	if (c == NULL)
		return (-1);
	ret = container_kill(c, sig != 0 ? sig : SIGTERM);
	container_free(c);
	return (ret);
}

int
ocifbsd_delete_container(const char *cid, bool force)
{
	struct ocifbsd_container *c;
	int ret;

	(void)force;
	if (cid == NULL)
		return (-1);
	c = container_get_by_id(cid);
	if (c == NULL)
		return (-1);
	ret = container_delete(c);
	container_free(c);
	return (ret);
}

int
ocifbsd_get_container_state(const char *cid, container_state_t *state,
	int *exit_code)
{
	struct ocifbsd_container *c;

	if (cid == NULL)
		return (-1);
	c = container_get_by_id(cid);
	if (c == NULL)
		return (-1);
	if (state != NULL)
		*state = (container_state_t)c->state;
	if (exit_code != NULL)
		*exit_code = c->exit_code;
	container_free(c);
	return (0);
}

int
ocifbsd_logs(const char *cid, int tail, bool follow)
{
	struct ocifbsd_container *c;
	FILE *f;
	char line[4096];

	(void)tail;
	(void)follow;
	if (cid == NULL)
		return (-1);
	c = container_get_by_id(cid);
	if (c == NULL)
		return (-1);
	if (c->log_path == NULL) {
		container_free(c);
		return (-1);
	}
	f = fopen(c->log_path, "r");
	if (f == NULL) {
		container_free(c);
		return (-1);
	}
	while (fgets(line, sizeof(line), f) != NULL)
		fputs(line, stdout);
	fclose(f);
	container_free(c);
	return (0);
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
			/*
			 * Unlinking while readdir() walks the same directory
			 * may (per POSIX) cause other entries to be skipped in
			 * that pass. Rescan until a full pass removes nothing,
			 * so no entry is left behind by the iteration itself.
			 */
			for (;;) {
				int removed = 0;

				rewinddir(d);
				while ((e = readdir(d)) != NULL) {
					if (strcmp(e->d_name, ".") == 0 ||
					    strcmp(e->d_name, "..") == 0)
						continue;
					if (rm_rf_at(fd, e->d_name,
					    top_dev) != 0)
						ret = -1;
					else
						removed = 1;
				}
				if (!removed || ret == -1)
					break;
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
	/* O_NOFOLLOW so a symlink swapped in as the immediate parent cannot
	 * redirect the subsequent O_NOFOLLOW *at() walk out of the store. */
	parentfd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
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
			usage(
			    "create [--name name] [--image ref | <bundle>]");
			return (0);
		default:
			usage("create");
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
			usage("create");
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
			usage("start <container-id>");
			return (0);
		default:
			usage("start");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage("start");
		return (1);
	}

	id = resolve_cid(argv[0]);

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
			usage("kill <container-id> [--signal <signal>]");
			return (0);
		default:
			usage("kill");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage("kill");
		return (1);
	}

	id = resolve_cid(argv[0]);

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

/*
 * Resolve a container spec (an ID or a --name) to its canonical container ID.
 * A direct ID hit wins; otherwise the on-disk state is scanned for a container
 * whose name matches. Returns a pointer to an internal buffer holding the ID,
 * or the original spec unchanged when nothing matches (so the downstream
 * "not found" error still fires). Lets every container subcommand accept a
 * name as well as an ID, matching common runtime UX. Not reentrant, which is
 * fine for the single-threaded, one-command-per-process CLI.
 */
static const char *
resolve_cid(const char *spec)
{
	static char buf[128];
	struct ocifbsd_container *c;
	struct ocifbsd_container **list;
	int n = 0;
	const char *out = spec;

	if (spec == NULL)
		return (spec);

	/* Direct ID hit. */
	c = container_get_by_id(spec);
	if (c != NULL) {
		if (c->id != NULL && strlcpy(buf, c->id, sizeof(buf)) <
		    sizeof(buf))
			out = buf;
		container_free(c);
		return (out);
	}

	/* Otherwise match by human-readable name. */
	list = state_list(&n);
	if (list == NULL)
		return (spec);
	for (int i = 0; i < n && list[i] != NULL; i++)
		if (out == spec && list[i]->name != NULL &&
		    list[i]->id != NULL &&
		    strcmp(list[i]->name, spec) == 0 &&
		    strlcpy(buf, list[i]->id, sizeof(buf)) < sizeof(buf))
			out = buf;
	for (int i = 0; i < n && list[i] != NULL; i++)
		container_free(list[i]);
	free(list);
	return (out);
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
			usage("delete <container-id> [--force]");
			return (0);
		default:
			usage("delete");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage("delete");
		return (1);
	}

	id = resolve_cid(argv[0]);

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

	/*
	 * Refuse live containers unless forced. A PAUSED container still has
	 * (SIGSTOPped) processes — deleting it without --force would tear down
	 * live workload just like deleting a running one.
	 */
	if ((c->state == OCIFBSD_STATE_RUNNING ||
	    c->state == OCIFBSD_STATE_PAUSED ||
	    c->state == OCIFBSD_STATE_PAUSED_HIGH) && !force) {
		fprintf(stderr, "error: container is %s, use --force to stop "
		    "and delete\n", c->state == OCIFBSD_STATE_RUNNING ?
		    "running" : "paused");
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
			usage("state <container-id>");
			return (0);
		default:
			usage("state");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage("state");
		return (1);
	}

	id = resolve_cid(argv[0]);

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		return (1);
	}

	/*
	 * Print the container state in the schema the OCI runtime spec defines
	 * for the `state` operation: ociVersion, id, status, bundle, and pid
	 * (the latter required only when the container is created or running).
	 * Pretty by default; --compact for one line.
	 */
	{
		char buf[PATH_MAX + 256];
		const char *bundle = c->bundle_path ? c->bundle_path : "";
		const char *status = ocifbsd_state_to_string(c->state);

		if (c->init_pid > 0) {
			snprintf(buf, sizeof(buf),
			    "{\"ociVersion\":\"1.0.2\",\"id\":\"%s\","
			    "\"status\":\"%s\",\"pid\":%d,\"bundle\":\"%s\"}",
			    c->id, status, (int)c->init_pid, bundle);
		} else {
			snprintf(buf, sizeof(buf),
			    "{\"ociVersion\":\"1.0.2\",\"id\":\"%s\","
			    "\"status\":\"%s\",\"bundle\":\"%s\"}",
			    c->id, status, bundle);
		}
		emit_json(buf);
	}

	container_free(c);
	return (0);
}

/*
 * ocifbsd proxy — a small L4 (TCP) round-robin load balancer with failover, so
 * a service's replicas can be fronted by a single endpoint without pulling in
 * an external proxy. Connections are distributed across the --backend targets
 * in round-robin order; a backend that will not accept a connection is skipped
 * and the next tried, giving health-based failover. Each accepted connection is
 * handled by a forked child that splices bytes in both directions.
 */
struct proxy_backend {
	char	*host;
	char	*port;
};

/* Load-balancing algorithms. */
enum proxy_algo {
	ALGO_ROUNDROBIN,	/* even rotation (default) */
	ALGO_RANDOM,		/* uniformly random backend */
	ALGO_SOURCEHASH,	/* hash(client IP) -> sticky affinity */
	ALGO_LEASTCONN		/* fewest active connections */
};

static enum proxy_algo
proxy_algo_parse(const char *s)
{
	if (strcmp(s, "random") == 0)
		return (ALGO_RANDOM);
	if (strcmp(s, "source-hash") == 0 || strcmp(s, "source") == 0 ||
	    strcmp(s, "iphash") == 0)
		return (ALGO_SOURCEHASH);
	if (strcmp(s, "least-conn") == 0 || strcmp(s, "leastconn") == 0)
		return (ALGO_LEASTCONN);
	return (ALGO_ROUNDROBIN);
}

/*
 * FNV-1a hash of a client's IP address (NOT the port), for source-hash
 * affinity: every connection from the same client sticks to the same backend,
 * which is how a stateful session survives without a shared store.
 */
static unsigned long
proxy_addr_hash(const struct sockaddr *sa, socklen_t slen)
{
	const unsigned char *p;
	size_t n;
	unsigned long h = 1469598103934665603UL;
	size_t i;

	if (sa->sa_family == AF_INET) {
		p = (const unsigned char *)
		    &((const struct sockaddr_in *)(const void *)sa)->sin_addr;
		n = sizeof(struct in_addr);
	} else if (sa->sa_family == AF_INET6) {
		p = (const unsigned char *)
		    &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
		n = sizeof(struct in6_addr);
	} else {
		p = (const unsigned char *)sa;
		n = slen;
	}
	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211UL;
	}
	return (h);
}

/* Connect to host:port with a bounded timeout; returns a blocking fd or -1. */
static int
proxy_dial(const char *host, const char *port, int timeout_ms)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0)
		return (-1);
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		int fl;

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		fl = fcntl(fd, F_GETFL, 0);
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			(void)fcntl(fd, F_SETFL, fl);
			break;
		}
		if (errno == EINPROGRESS) {
			struct pollfd pfd = { fd, POLLOUT, 0 };
			int e = 0;
			socklen_t el = sizeof(e);

			if (poll(&pfd, 1, timeout_ms) == 1 &&
			    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) == 0 &&
			    e == 0) {
				(void)fcntl(fd, F_SETFL, fl);
				break;
			}
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return (fd);
}

/*
 * Splice bytes both ways with correct half-close semantics: when one side
 * sends EOF we half-close only the *other* side's write end and keep relaying
 * the opposite direction until it too closes. Tearing both sides down on the
 * first EOF (as a naive pump does) truncates an in-flight response whenever the
 * client half-closes after sending its request — which HTTP clients routinely
 * do, and which surfaces as intermittent upstream 5xx.
 */
static void
proxy_pump(int a, int b)
{
	struct pollfd pfd[2];
	char buf[65536];
	int a_readable = 1, b_readable = 1;

	for (;;) {
		if (!a_readable && !b_readable)
			return;
		pfd[0].fd = a;
		pfd[0].events = a_readable ? POLLIN : 0;
		pfd[0].revents = 0;
		pfd[1].fd = b;
		pfd[1].events = b_readable ? POLLIN : 0;
		pfd[1].revents = 0;
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		for (int i = 0; i < 2; i++) {
			if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				int from = pfd[i].fd, to = pfd[1 - i].fd;
				ssize_t n = read(from, buf, sizeof(buf));

				if (n > 0) {
					ssize_t off = 0;

					while (off < n) {
						ssize_t w = write(to, buf + off,
						    (size_t)(n - off));
						if (w <= 0)
							return;
						off += w;
					}
				} else {
					/* EOF/error on `from`: half-close the
					 * peer's write side, stop reading here,
					 * but keep draining the other way. */
					(void)shutdown(to, SHUT_WR);
					if (i == 0)
						a_readable = 0;
					else
						b_readable = 0;
				}
			}
		}
	}
}

static void
proxy_reap(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

static int
cmd_proxy(int argc, char **argv)
{
	const char *listen_spec = NULL;
	struct proxy_backend backends[64];
	int nbackends = 0, ch, timeout_ms = 2000, lfd, rr = 0;
	char lhost[256], lport[16];
	enum proxy_algo algo = ALGO_ROUNDROBIN;
	int *conns = NULL;	/* shared active-connection counts (least-conn) */

	static struct option longopts[] = {
		{ "listen",	required_argument,	NULL, 'l' },
		{ "backend",	required_argument,	NULL, 'b' },
		{ "timeout",	required_argument,	NULL, 't' },
		{ "algo",	required_argument,	NULL, 'a' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "l:b:t:a:h", longopts,
	    NULL)) != -1) {
		switch (ch) {
		case 'l':
			listen_spec = optarg;
			break;
		case 'a':
			algo = proxy_algo_parse(optarg);
			break;
		case 'b': {
			char *colon = strrchr(optarg, ':');

			if (colon == NULL) {
				fprintf(stderr, "error: --backend needs "
				    "host:port\n");
				return (1);
			}
			if (nbackends < 64) {
				*colon = '\0';
				backends[nbackends].host = strdup(optarg);
				backends[nbackends].port = strdup(colon + 1);
				nbackends++;
			}
			break;
		}
		case 't':
			timeout_ms = atoi(optarg);
			break;
		case 'h':
			usage("proxy --listen [addr:]port --backend host:port "
			    "[--backend ...] [--algo round-robin|random|"
			    "source-hash|least-conn]");
			return (0);
		default:
			usage("proxy --listen [addr:]port --backend host:port "
			    "[--backend ...] [--algo round-robin|random|"
			    "source-hash|least-conn]");
			return (1);
		}
	}
	if (listen_spec == NULL || nbackends == 0) {
		fprintf(stderr, "error: proxy requires --listen and at least "
		    "one --backend\n");
		return (1);
	}

	/* Split listen spec into [addr:]port. */
	{
		const char *colon = strrchr(listen_spec, ':');

		if (colon != NULL) {
			size_t hl = (size_t)(colon - listen_spec);

			if (hl >= sizeof(lhost))
				hl = sizeof(lhost) - 1;
			memcpy(lhost, listen_spec, hl);
			lhost[hl] = '\0';
			snprintf(lport, sizeof(lport), "%s", colon + 1);
		} else {
			lhost[0] = '\0';
			snprintf(lport, sizeof(lport), "%s", listen_spec);
		}
	}

	/* Bind + listen. */
	{
		struct addrinfo hints, *res;
		int one = 1;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		if (getaddrinfo(lhost[0] ? lhost : NULL, lport, &hints,
		    &res) != 0) {
			fprintf(stderr, "error: cannot resolve listen %s\n",
			    listen_spec);
			return (1);
		}
		lfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (lfd < 0) {
			perror("socket");
			freeaddrinfo(res);
			return (1);
		}
		(void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one,
		    sizeof(one));
		if (bind(lfd, res->ai_addr, res->ai_addrlen) != 0) {
			perror("bind");
			freeaddrinfo(res);
			return (1);
		}
		freeaddrinfo(res);
		if (listen(lfd, 128) != 0) {
			perror("listen");
			return (1);
		}
	}

	/*
	 * least-conn needs live connection counts shared across the
	 * fork-per-connection children; back them with anonymous shared memory
	 * so a child can decrement its backend on exit.
	 */
	if (algo == ALGO_LEASTCONN) {
		conns = mmap(NULL, sizeof(int) * (size_t)nbackends,
		    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
		if (conns == MAP_FAILED) {
			conns = NULL;
			algo = ALGO_ROUNDROBIN;
		}
	}

	signal(SIGCHLD, proxy_reap);
	signal(SIGPIPE, SIG_IGN);
	{
		const char *an = algo == ALGO_RANDOM ? "random" :
		    algo == ALGO_SOURCEHASH ? "source-hash (sticky)" :
		    algo == ALGO_LEASTCONN ? "least-conn" : "round-robin";

		printf("ocifbsd proxy: listening on %s -> %d backend(s), "
		    "algo=%s, failover on\n", listen_spec, nbackends, an);
		fflush(stdout);
	}

	for (;;) {
		struct sockaddr_storage ss;
		socklen_t sslen = sizeof(ss);
		int cfd = accept(lfd, (struct sockaddr *)&ss, &sslen);
		int bfd = -1, tries, start, chosen = -1;
		pid_t pid;

		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		/* Choose the preferred starting backend per algorithm. */
		switch (algo) {
		case ALGO_RANDOM:
			start = (int)(random() % nbackends);
			break;
		case ALGO_SOURCEHASH:
			start = (int)(proxy_addr_hash((struct sockaddr *)&ss,
			    sslen) % (unsigned long)nbackends);
			break;
		case ALGO_LEASTCONN: {
			int m = 0;

			for (int i = 1; i < nbackends; i++)
				if (conns[i] < conns[m])
					m = i;
			start = m;
			break;
		}
		case ALGO_ROUNDROBIN:
		default:
			start = rr;
			break;
		}

		/* Try from the preferred index, failing over to the rest. */
		for (tries = 0; tries < nbackends; tries++) {
			int idx = (start + tries) % nbackends;

			bfd = proxy_dial(backends[idx].host,
			    backends[idx].port, timeout_ms);
			if (bfd >= 0) {
				chosen = idx;
				break;
			}
		}
		if (bfd < 0) {		/* all backends unreachable */
			close(cfd);
			continue;
		}
		rr = (chosen + 1) % nbackends;
		if (conns != NULL)
			conns[chosen]++;

		pid = fork();
		if (pid == 0) {
			close(lfd);
			proxy_pump(cfd, bfd);
			if (conns != NULL)
				conns[chosen]--;
			_exit(0);
		}
		close(cfd);
		close(bfd);
	}
	close(lfd);
	return (0);
}

/*
 * Capture `rctl -u jail:<jailname>` output into buf. rctl reports live RACCT
 * resource usage for the jail (cputime, memoryuse, nthr, ...). Returns 0 on
 * success (buf NUL-terminated), -1 otherwise. jailname is ocifbsd-<hexid>, so
 * no shell is involved and the argument is not attacker-controlled.
 */
static int
capture_rctl(const char *jailname, char *buf, size_t buflen)
{
	int fds[2], status;
	pid_t pid;
	char rule[80];
	size_t o = 0;

	if (buflen == 0 || pipe(fds) != 0)
		return (-1);
	snprintf(rule, sizeof(rule), "jail:%s", jailname);
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return (-1);
	}
	if (pid == 0) {
		int dn;

		close(fds[0]);
		dup2(fds[1], STDOUT_FILENO);
		dn = open("/dev/null", O_WRONLY);
		if (dn >= 0)
			dup2(dn, STDERR_FILENO);
		execlp("rctl", "rctl", "-u", rule, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	for (;;) {
		ssize_t r = read(fds[0], buf + o, buflen - 1 - o);

		if (r <= 0)
			break;
		o += (size_t)r;
		if (o >= buflen - 1)
			break;
	}
	buf[o] = '\0';
	close(fds[0]);
	waitpid(pid, &status, 0);
	return (0);
}

/* Extract the unsigned value of "<key>=" from an rctl -u dump (0 if absent). */
static uintmax_t
rctl_field(const char *buf, const char *key)
{
	char pat[48];
	const char *p;

	snprintf(pat, sizeof(pat), "%s=", key);
	for (p = strstr(buf, pat); p != NULL; p = strstr(p + 1, pat)) {
		if (p == buf || p[-1] == '\n')
			return (strtoumax(p + strlen(pat), NULL, 10));
	}
	return (0);
}

/*
 * `ocifbsd stats` — per-container resource usage from RACCT, as JSON. Pretty by
 * default (human-facing); --compact emits one JSON object per line (JSONL) for
 * streaming into a metrics pipeline.
 */
static int
cmd_stats(int argc, char **argv)
{
	struct ocifbsd_container **list;
	int n, i, ch, compact = 0, first = 1;

	static struct option longopts[] = {
		{ "compact",	no_argument,	NULL, 'c' },
		{ "help",	no_argument,	NULL, 'h' },
		{ NULL,		0,		NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "ch", longopts, NULL)) != -1) {
		switch (ch) {
		case 'c':
			compact = 1;
			break;
		case 'h':
			usage("stats [--compact]");
			return (0);
		default:
			usage("stats [--compact]");
			return (1);
		}
	}

	errno = 0;
	list = state_list(&n);
	if (list == NULL) {
		if (errno != 0) {
			fprintf(stderr, "error: failed to list containers: %s\n",
			    strerror(errno));
			return (1);
		}
		if (!compact)
			printf("{\n  \"containers\": []\n}\n");
		return (0);
	}

	if (!compact)
		printf("{\n  \"containers\": [\n");
	for (i = 0; i < n; i++) {
		struct ocifbsd_container *c = list[i];
		char jn[64], buf[8192], eid[128], enm[512];
		uintmax_t cpu = 0, mem = 0, vmem = 0, swap = 0;
		uintmax_t nproc = 0, nthr = 0, ofiles = 0, wall = 0;

		if (c->state == OCIFBSD_STATE_RUNNING) {
			snprintf(jn, sizeof(jn), "ocifbsd-%.12s",
			    c->id ? c->id : "");
			buf[0] = '\0';
			(void)capture_rctl(jn, buf, sizeof(buf));
			cpu = rctl_field(buf, "cputime");
			mem = rctl_field(buf, "memoryuse");
			vmem = rctl_field(buf, "vmemoryuse");
			swap = rctl_field(buf, "swapuse");
			nproc = rctl_field(buf, "maxproc");
			nthr = rctl_field(buf, "nthr");
			ofiles = rctl_field(buf, "openfiles");
			wall = rctl_field(buf, "wallclock");
		}
		ocifbsd_json_escape(c->id ? c->id : "", eid, sizeof(eid));
		ocifbsd_json_escape(c->name ? c->name : "", enm, sizeof(enm));

		if (compact) {
			printf("{\"id\":\"%s\",\"name\":\"%s\",\"state\":\"%s\","
			    "\"cputime_sec\":%ju,\"memory_bytes\":%ju,"
			    "\"vmemory_bytes\":%ju,\"swap_bytes\":%ju,"
			    "\"processes\":%ju,\"threads\":%ju,"
			    "\"open_files\":%ju,\"wallclock_sec\":%ju}\n",
			    eid, enm, ocifbsd_state_to_string(c->state),
			    cpu, mem, vmem, swap, nproc, nthr, ofiles, wall);
		} else {
			printf("%s    {\n"
			    "      \"id\": \"%s\",\n"
			    "      \"name\": \"%s\",\n"
			    "      \"state\": \"%s\",\n"
			    "      \"cputime_sec\": %ju,\n"
			    "      \"memory_bytes\": %ju,\n"
			    "      \"vmemory_bytes\": %ju,\n"
			    "      \"swap_bytes\": %ju,\n"
			    "      \"processes\": %ju,\n"
			    "      \"threads\": %ju,\n"
			    "      \"open_files\": %ju,\n"
			    "      \"wallclock_sec\": %ju\n"
			    "    }",
			    first ? "" : ",\n", eid, enm,
			    ocifbsd_state_to_string(c->state),
			    cpu, mem, vmem, swap, nproc, nthr, ofiles, wall);
			first = 0;
		}
		container_free(c);
	}
	if (!compact)
		printf("\n  ]\n}\n");
	free(list);
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
			usage("list");
			return (0);
		default:
			usage("list");
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
			usage("inspect <container-id>");
			return (0);
		default:
			usage("inspect");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage("inspect");
		return (1);
	}

	id = resolve_cid(argv[0]);

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
			usage(
			    "run [--name name] [--image ref | <bundle>]");
			return (0);
		default:
			usage("run");
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
			usage("run");
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

	/* Serialize the start against other processes acting on the new id
	 * (the same per-container lock every other lifecycle op takes). */
	{
		int lockfd = state_lock_container(c->id);

		if (lockfd < 0) {
			fprintf(stderr, "error: failed to lock container %s: "
			    "%s\n", c->id, strerror(errno));
			container_delete(c);
			container_free(c);
			return (1);
		}

		ret = container_start(c);
		if (ret != 0) {
			/*
			 * `run` is create+start; the start failed, so tear the
			 * created container back down rather than leaving an
			 * orphan on disk that a later run could collide with.
			 * A failed rollback is reported, not swallowed.
			 */
			fprintf(stderr, "error: failed to start container %s: "
			    "%s\n", c->id, strerror(errno));
			if (container_delete(c) != 0)
				fprintf(stderr, "warning: rollback failed; "
				    "container %s left on disk (delete it "
				    "manually)\n", c->id);
			container_free(c);
			state_unlock_container(lockfd);
			return (1);
		}
		state_unlock_container(lockfd);
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
cmd_build(int argc, char **argv)
{
	const char *file = "Containerfile";
	const char *tag = NULL;
	const char *context = ".";
	int ch, verbose = 0;

	static struct option longopts[] = {
		{ "file",	required_argument,	NULL, 'f' },
		{ "tag",	required_argument,	NULL, 't' },
		{ "verbose",	no_argument,		NULL, 'v' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "f:t:vh", longopts, NULL)) != -1) {
		switch (ch) {
		case 'f':
			file = optarg;
			break;
		case 't':
			tag = optarg;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'h':
			usage("build [-f Containerfile] -t name[:tag] [context]");
			return (0);
		default:
			usage("build [-f Containerfile] -t name[:tag] [context]");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc >= 1)
		context = argv[0];
	if (tag == NULL) {
		fprintf(stderr, "error: build requires -t name[:tag]\n");
		usage("build [-f Containerfile] -t name[:tag] [context]");
		return (1);
	}
	return (image_build(file, context, tag, verbose));
}

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
			usage("pull [--dry-run] <reference>");
			return (0);
		default:
			usage("pull");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		usage("pull [--dry-run] <reference>");
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
	/* FTS_COMFOLLOW: follow the root itself if OCIFBSD_DATA_DIR is a
	 * symlink (entries below it are still walked physically). */
	fts = fts_open(paths, FTS_PHYSICAL | FTS_COMFOLLOW | FTS_NOCHDIR, NULL);
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

		/* Is this directory a usable image root? A truncated path
		 * would stat something other than fts_path — skip instead. */
		if ((size_t)snprintf(cfg, sizeof(cfg), "%s/config.json",
		    ent->fts_path) >= sizeof(cfg) ||
		    (size_t)snprintf(rfs, sizeof(rfs), "%s/rootfs",
		    ent->fts_path) >= sizeof(rfs))
			continue;
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
			usage("rmi [--force] <reference>");
			return (0);
		default:
			usage("rmi");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		usage("rmi <reference>");
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
		char inuse[64];
		int found = 0;

		/*
		 * Fail closed: if the container state cannot be read we cannot
		 * prove the image is unused, so refuse rather than delete it.
		 * (state_list returns an empty non-NULL list when the state dir
		 * is simply absent; NULL means a real error.)
		 */
		if (list == NULL) {
			fprintf(stderr, "error: cannot verify whether image %s "
			    "is in use (use --force to override)\n", ref);
			free(store_path);
			return (1);
		}
		inuse[0] = '\0';
		for (i = 0; i < n; i++) {
			if (!found && list[i]->bundle_path != NULL &&
			    strcmp(list[i]->bundle_path, store_path) == 0) {
				strlcpy(inuse, list[i]->id, sizeof(inuse));
				found = 1;
			}
			container_free(list[i]);
		}
		free(list);
		if (found) {
			fprintf(stderr, "error: image %s is in use by container "
			    "%s (use --force to remove anyway)\n", ref, inuse);
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
			usage(
			    "exec [--cwd dir] <container-id> <command> [args...]");
			return (0);
		default:
			usage("exec");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 2) {
		fprintf(stderr, "error: container id and command required\n");
		usage(
		    "exec [--cwd dir] <container-id> <command> [args...]");
		return (1);
	}

	id = resolve_cid(argv[0]);
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
			usage(
			    "stop [--timeout sec] <container-id>");
			return (0);
		default:
			usage("stop");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: container id required\n");
		usage("stop [--timeout sec] <container-id>");
		return (1);
	}

	id = resolve_cid(argv[0]);

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
			usage(do_pause ?
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

	id = resolve_cid(argv[0]);

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
			usage("push <reference>");
			return (0);
		default:
			usage("push");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		usage("push <reference>");
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
			usage("load [--name ref] <oci-archive|dir>");
			return (0);
		default:
			usage("load");
			return (1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		usage("load [--name ref] <oci-archive|dir>");
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
	if (list == NULL) {
		/* A state-read failure is not "no such container" — surface
		 * the real cause before the caller's not-found message. */
		fprintf(stderr, "error: failed to read container state: %s\n",
		    strerror(errno));
		return (NULL);
	}
	if (n <= 0) {
		free(list);
		return (NULL);
	}
	reflen = strlen(ref);
	int nmatch = 0;
	for (i = 0; i < n; i++) {
		struct ocifbsd_container *c = list[i];
		bool exact = false, hit = false;

		if (c->name != NULL && strcmp(c->name, ref) == 0)
			exact = hit = true;
		else if (c->id != NULL && strcmp(c->id, ref) == 0)
			exact = hit = true;
		else if (c->id != NULL && reflen >= 6 &&
		    strncmp(c->id, ref, reflen) == 0)
			hit = true;             /* id prefix (may be ambiguous) */

		/*
		 * An exact name/id match wins outright. Otherwise collect prefix
		 * hits and require exactly one — a prefix that matches several
		 * containers must not silently mutate an arbitrary one.
		 */
		if (exact) {
			if (match != NULL)
				container_free(match);
			match = c;
			nmatch = 1;
			/* keep scanning only to free the rest */
			for (i++; i < n; i++)
				container_free(list[i]);
			break;
		} else if (hit) {
			if (match == NULL)
				match = c;
			else
				container_free(c);
			nmatch++;
		} else {
			container_free(c);
		}
	}
	free(list);
	if (nmatch != 1) {              /* none, or an ambiguous prefix */
		if (match != NULL)
			container_free(match);
		return (NULL);
	}
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
		if (list == NULL) {	/* NULL is a real error, not "empty" */
			fprintf(stderr,
			    "error: failed to list containers: %s\n",
			    strerror(errno));
			return (1);
		}
		if (n <= 0) {
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
		OPT_BRIDGE,
		OPT_CLEAR_IP4, OPT_CLEAR_IP6, OPT_CLEAR_DNS, OPT_CLEAR
	};
	static struct option longopts[] = {
		{ "vnet",	required_argument,	NULL, OPT_VNET },
		{ "ip4",	required_argument,	NULL, OPT_IP4 },
		{ "ip6",	required_argument,	NULL, OPT_IP6 },
		{ "gateway4",	required_argument,	NULL, OPT_GW4 },
		{ "gateway6",	required_argument,	NULL, OPT_GW6 },
		{ "dns",	required_argument,	NULL, OPT_DNS },
		{ "bridge",	required_argument,	NULL, OPT_BRIDGE },
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
		usage("network set <container> [options]");
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
		/*
		 * An existing config that fails to parse must abort, not be
		 * silently replaced: writing a fresh file over it would discard
		 * whatever settings it held.
		 */
		if (netcfg_parse(json, &nc) != 0) {
			fprintf(stderr, "error: existing network config %s is "
			    "unreadable; refusing to overwrite it\n", path);
			free(json);
			netcfg_free(&nc);
			state_unlock_container(lockfd);
			container_free(c);
			return (1);
		}
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
		case OPT_BRIDGE:
			rc = netcfg_set_bridge(&nc, optarg);
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
			usage("network set <container> "
			    "[--vnet on|off] [--ip4 CIDR] [--ip6 CIDR] "
			    "[--gateway4 ADDR] [--gateway6 ADDR] [--dns ADDR] "
			    "[--clear|--clear-ip4|--clear-ip6|--clear-dns]");
			ret = 0;
			goto out;
		default:
			usage("network set");
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
 * Named-network management (`network create|ls|rm|inspect`) is a distinct
 * surface from the per-container `network list|set` above: it drives the
 * network resource library (network_create/list/delete/inspect in
 * network/network.h), which builds an if_bridge + epair and persists the
 * network under /var/run/ocifbsd/networks. The two coexist under `network`
 * the way Docker exposes both container attach config and `network` objects.
 */

/*
 * Generate a filename-safe 32-hex network id. arc4random is already the
 * project's id source elsewhere; a UUID would work equally but needs no
 * library beyond libc here.
 */
static void
net_make_id(char *buf, size_t len)
{
	static const char hex[] = "0123456789abcdef";
	size_t i;

	for (i = 0; i + 1 < len && i < 32; i++)
		buf[i] = hex[arc4random_uniform(16)];
	buf[i] = '\0';
}

/*
 * Ensure the named-network state directory tree exists. The network library
 * persists under /var/run/ocifbsd/networks but its network_init() only does a
 * single mkdir, which fails when the /var/run/ocifbsd parent is absent; use
 * the project's recursive mkdirp so the first `network create` on a fresh host
 * succeeds. Best effort — network_create surfaces any real write error.
 */
static void
net_ensure_store(void)
{
	(void)mkdirp("/var/run/ocifbsd/networks", 0755);
}

/*
 * Resolve a user-supplied network reference (id or name) to a concrete id.
 * Returns a malloc'd id on success (caller frees) or NULL if no match. An
 * exact id match wins; otherwise the first network whose name matches is used.
 */
static char *
net_resolve_id(const char *ref)
{
	struct network_config **list = NULL;
	int n = 0, i;
	char *found = NULL;

	if (ref == NULL || ref[0] == '\0')
		return (NULL);
	if (network_list(&list, &n) != 0 || list == NULL)
		return (NULL);
	/* Exact id match first. */
	for (i = 0; i < n; i++) {
		if (list[i]->id != NULL && strcmp(list[i]->id, ref) == 0) {
			found = strdup(list[i]->id);
			break;
		}
	}
	/* Then name match. */
	if (found == NULL) {
		for (i = 0; i < n; i++) {
			if (list[i]->name != NULL &&
			    strcmp(list[i]->name, ref) == 0 &&
			    list[i]->id != NULL) {
				found = strdup(list[i]->id);
				break;
			}
		}
	}
	free(list);	/* short-lived CLI: entry configs reclaimed at exit */
	return (found);
}

/*
 * network create [--driver bridge] [--subnet CIDR] [--gateway IP]
 *                [--internal] NAME
 * Creates a bridge network and prints its id. Requires modify privilege.
 */
static int
cmd_network_create(int argc, char **argv)
{
	struct network_config cfg;
	char idbuf[33];
	const char *driver = "bridge";
	int ch;
	static struct option longopts[] = {
		{ "driver",	required_argument,	NULL, 'd' },
		{ "subnet",	required_argument,	NULL, 's' },
		{ "gateway",	required_argument,	NULL, 'g' },
		{ "internal",	no_argument,		NULL, 'i' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	if (ocifbsd_require_access(OCIFBSD_OP_MODIFY) != 0) {
		fprintf(stderr, "error: permission denied: creating a network "
		    "requires root or the %s group\n", OCIFBSD_ADMIN_GROUP);
		return (1);
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.type = NETWORK_TYPE_BRIDGE;

	optind = 1;
	while ((ch = getopt_long(argc, argv, "d:s:g:ih", longopts,
	    NULL)) != -1) {
		switch (ch) {
		case 'd': driver = optarg; break;
		case 's': cfg.subnet = optarg; break;
		case 'g': cfg.gateway = optarg; break;
		case 'i': cfg.internal = true; break;
		case 'h':
		default:
			usage(
			    "network create [--driver bridge] [--subnet CIDR] "
			    "[--gateway IP] [--internal] NAME");
			return (ch == 'h' ? 0 : 1);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1) {
		fprintf(stderr, "error: network create requires a NAME\n");
		return (1);
	}
	if (strcmp(driver, "bridge") != 0) {
		fprintf(stderr, "error: unsupported driver '%s' "
		    "(only 'bridge' is supported)\n", driver);
		return (1);
	}

	net_ensure_store();
	net_make_id(idbuf, sizeof(idbuf));
	cfg.id = idbuf;
	cfg.name = argv[0];
	cfg.driver = __DECONST(char *, driver);

	if (network_create(&cfg) != 0) {
		fprintf(stderr, "error: failed to create network '%s': %s\n",
		    cfg.name, strerror(errno));
		return (1);
	}
	printf("%s\n", idbuf);
	return (0);
}

/*
 * network ls — list named networks. Human-readable table by default;
 * with --pretty JSON (the default) still prints the table, since `inspect`
 * is the JSON surface. Requires view privilege.
 */
static int
cmd_network_ls(int argc __unused, char **argv __unused)
{
	struct network_config **list = NULL;
	int n = 0, i;

	if (ocifbsd_require_access(OCIFBSD_OP_VIEW) != 0) {
		fprintf(stderr, "error: permission denied: listing networks "
		    "requires root or the %s group\n", OCIFBSD_ADMIN_GROUP);
		return (1);
	}
	if (network_list(&list, &n) != 0) {
		/* No store yet simply means no networks. */
		if (errno == ENOENT)
			return (0);
		fprintf(stderr, "error: failed to list networks: %s\n",
		    strerror(errno));
		return (1);
	}
	printf("%-34s %-16s %-10s %s\n", "NETWORK ID", "NAME", "DRIVER",
	    "BRIDGE");
	for (i = 0; i < n; i++) {
		printf("%-34s %-16s %-10s %s\n",
		    list[i]->id != NULL ? list[i]->id : "-",
		    list[i]->name != NULL ? list[i]->name : "-",
		    list[i]->driver != NULL ? list[i]->driver : "bridge",
		    list[i]->bridge != NULL ? list[i]->bridge : "-");
	}
	free(list);	/* entry configs reclaimed at process exit */
	return (0);
}

/*
 * network rm ID|NAME [...] — delete one or more named networks. Requires
 * modify privilege. Reports and continues on individual failures.
 */
static int
cmd_network_rm(int argc, char **argv)
{
	int i, ret = 0;

	if (ocifbsd_require_access(OCIFBSD_OP_MODIFY) != 0) {
		fprintf(stderr, "error: permission denied: removing a network "
		    "requires root or the %s group\n", OCIFBSD_ADMIN_GROUP);
		return (1);
	}
	if (argc < 1) {
		fprintf(stderr, "error: network rm requires a network ID or "
		    "name\n");
		return (1);
	}
	for (i = 0; i < argc; i++) {
		char *id = net_resolve_id(argv[i]);

		if (id == NULL) {
			fprintf(stderr, "error: no such network: %s\n",
			    argv[i]);
			ret = 1;
			continue;
		}
		if (network_delete(id) != 0) {
			fprintf(stderr, "error: failed to remove network %s: "
			    "%s\n", argv[i], strerror(errno));
			ret = 1;
		} else {
			printf("%s\n", argv[i]);
		}
		free(id);
	}
	return (ret);
}

/*
 * network inspect ID|NAME — emit the network's JSON (pretty by default).
 * Requires view privilege.
 */
static int
cmd_network_inspect(int argc, char **argv)
{
	char *id, *json = NULL;
	int rc;

	if (ocifbsd_require_access(OCIFBSD_OP_VIEW) != 0) {
		fprintf(stderr, "error: permission denied: inspecting a network "
		    "requires root or the %s group\n", OCIFBSD_ADMIN_GROUP);
		return (1);
	}
	if (argc < 1) {
		fprintf(stderr, "error: network inspect requires a network ID "
		    "or name\n");
		return (1);
	}
	id = net_resolve_id(argv[0]);
	if (id == NULL) {
		fprintf(stderr, "error: no such network: %s\n", argv[0]);
		return (1);
	}
	rc = network_inspect(id, &json);
	free(id);
	if (rc != 0 || json == NULL) {
		fprintf(stderr, "error: failed to inspect network %s\n",
		    argv[0]);
		return (1);
	}
	emit_json(json);
	free(json);
	return (0);
}

/*
 * network — per-container network config (list|set) plus named-network
 * management (create|ls|rm|inspect).
 */
static int
cmd_network(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: ocifbsd network "
		    "<list|set|create|ls|rm|inspect> [args]\n");
		return (1);
	}
	if (strcmp(argv[1], "list") == 0)
		return (cmd_network_list(argc - 2, argv + 2));
	if (strcmp(argv[1], "set") == 0)
		return (cmd_network_set(argc - 2, argv + 2));
	if (strcmp(argv[1], "create") == 0)
		return (cmd_network_create(argc - 1, argv + 1));
	if (strcmp(argv[1], "ls") == 0)
		return (cmd_network_ls(argc - 2, argv + 2));
	if (strcmp(argv[1], "rm") == 0)
		return (cmd_network_rm(argc - 2, argv + 2));
	if (strcmp(argv[1], "inspect") == 0)
		return (cmd_network_inspect(argc - 2, argv + 2));
	fprintf(stderr, "error: unknown network subcommand: %s\n", argv[1]);
	fprintf(stderr, "usage: ocifbsd network "
	    "<list|set|create|ls|rm|inspect> [args]\n");
	return (1);
}

/* Command table */
struct command {
	const char *name;
	int (*func)(int argc, char **argv);
	const char *help;
};

/*
 * Orchestration CLI bridge. The orchestration library exposes a dispatcher
 * that expects argv[1] to be the object (pod|service|stack); our command
 * table hands the handler argv[0] as that object, so prepend a program-name
 * slot before delegating. orch_init() prepares orchestration state (best
 * effort). Declared here to avoid pulling the orchestration headers into the
 * whole CLI translation unit.
 */
extern int orch_cli_dispatch(int argc, char **argv);
extern int orch_init(void);

static int
cmd_orch(int argc, char **argv)
{
	char **nargv;
	int i, ret;

	nargv = calloc((size_t)argc + 2, sizeof(char *));
	if (nargv == NULL)
		return (1);
	nargv[0] = __DECONST(char *, "ocifbsd");
	for (i = 0; i < argc; i++)
		nargv[i + 1] = argv[i];
	nargv[argc + 1] = NULL;

	(void)orch_init();
	ret = orch_cli_dispatch(argc + 1, nargv);
	free(nargv);
	return (ret);
}

static struct command commands[] = {
	{ "create",	cmd_create,	"Create a container from OCI bundle" },
	{ "start",	cmd_start,	"Start a created container" },
	{ "kill",	cmd_kill,	"Send signal to container" },
	{ "delete",	cmd_delete,	"Delete a container" },
	{ "state",	cmd_state,	"Show container state" },
	{ "list",	cmd_list,	"List containers" },
	{ "stats",	cmd_stats,	"Per-container resource stats (JSON)" },
	{ "proxy",	cmd_proxy,	"L4 (protocol-agnostic) load balancer across backends" },
	{ "inspect",	cmd_inspect,	"Show container details" },
	{ "run",	cmd_run,	"Create and start container" },
	{ "exec",	cmd_exec,	"Run a command in a running container" },
	{ "stop",	cmd_stop,	"Gracefully stop a container" },
	{ "pause",	cmd_pause,	"Pause a running container" },
	{ "resume",	cmd_resume,	"Resume a paused container" },
	{ "build",	cmd_build,	"Build an image from a Containerfile" },
	{ "pull",	cmd_pull,	"Resolve or pull an OCI image" },
	{ "push",	cmd_push,	"Push a local image to a registry" },
	{ "load",	cmd_load,	"Import a local OCI image archive" },
	{ "images",	cmd_images,	"List local image store" },
	{ "rmi",	cmd_rmi,	"Remove a local image" },
	{ "network",	cmd_network,	"Manage networks (create/ls/rm/inspect) and container net config (list/set)" },
	{ "pod",	cmd_orch,	"Manage pods (orchestration)" },
	{ "service",	cmd_orch,	"Manage services (orchestration)" },
	{ "stack",	cmd_orch,	"Manage stacks (orchestration)" },
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
			usage(NULL);
			return (0);
		case 'V':
			opt.version = 1;
			version();
			return (0);
		default:
			usage(NULL);
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		usage(NULL);
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
	usage(NULL);

	return (1);
}
