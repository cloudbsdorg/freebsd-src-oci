/*-
 * Copyright (c) 2024 The FreeBSD Foundation
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
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "include/ocifbsd.h"
#include "image/pull.h"
#include "image/zfs_store.h"

/* Global verbosity flag */
static bool verbose = false;

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
		fprintf(stderr, "  -h, --help       Show this help message\n");
		fprintf(stderr, "  -V, --version    Show version information\n");
		fprintf(stderr, "\nCommands:\n");
		fprintf(stderr, "  create <bundle> [--name <name>]  Create a container\n");
		fprintf(stderr, "  start <container-id>              Start a created container\n");
		fprintf(stderr, "  kill <container-id> [signal]      Send signal to container\n");
		fprintf(stderr, "  delete <container-id> [--force]   Delete a container\n");
		fprintf(stderr, "  state <container-id>              Show container state\n");
		fprintf(stderr, "  list                               List containers\n");
		fprintf(stderr, "  inspect <container-id>            Show container details\n");
		fprintf(stderr, "  run <bundle> [--name <name>]      Create and start in one command\n");
		fprintf(stderr, "  pull <reference> [--dry-run]      Resolve/pull OCI image reference\n");
		fprintf(stderr, "  images                            List local image store paths\n");
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

/* Command handlers */
static int
cmd_create(int argc, char **argv)
{
	const char *bundle = NULL;
	const char *name = NULL;
	struct ocifbsd_container *c;
	char *bundle_path;
	char *cname;
	int ret;

	/* Parse create-specific options */
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "+n:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'h':
			usage(argv[0], "create [--name name] <bundle>");
			return (0);
		default:
			usage(argv[0], "create");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: bundle path required\n");
		usage("ocifbsd", "create");
		return (1);
	}

	bundle = argv[0];

	/* Resolve bundle path */
	bundle_path = resolve_bundle_path(bundle);
	if (bundle_path == NULL) {
		fprintf(stderr, "error: invalid bundle path: %s\n", bundle);
		return (1);
	}

	/* Canonicalize name if provided */
	if (name != NULL) {
		cname = canonical_name(name);
		if (cname == NULL) {
			fprintf(stderr, "error: invalid container name: %s\n", name);
			free(bundle_path);
			return (1);
		}
	} else {
		cname = NULL;
	}

	/* Create container */
	if (verbose) {
		fprintf(stderr, "Creating container from bundle: %s\n", bundle_path);
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

	/* Parse start-specific options */
	static struct option longopts[] = {
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "+h", longopts, NULL)) != -1) {
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

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
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
		return (1);
	}

	printf("%s\n", c->id);

	container_free(c);
	return (0);
}

static int
cmd_kill(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;
	int sig = SIGTERM;
	int ret;

	/* Parse kill-specific options */
	static struct option longopts[] = {
		{ "signal", required_argument, NULL, 's' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "+s:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 's':
			/* Convert signal name to number */
			if (isdigit(optarg[0])) {
				sig = atoi(optarg);
			} else if (strncasecmp(optarg, "SIG", 3) == 0) {
				sig = atoi(optarg + 3);
			} else {
				/* Try signal name without SIG prefix */
				sig = atoi(optarg);
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

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
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
		return (1);
	}

	printf("%s\n", c->id);

	container_free(c);
	return (0);
}

static int
cmd_delete(int argc, char **argv)
{
	struct ocifbsd_container *c;
	const char *id;
	bool force = false;
	int ret;

	/* Parse delete-specific options */
	static struct option longopts[] = {
		{ "force", no_argument, NULL, 'f' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "+fh", longopts, NULL)) != -1) {
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

	/* Get container */
	c = container_get_by_id(id);
	if (c == NULL) {
		fprintf(stderr, "error: container not found: %s\n", id);
		return (1);
	}

	/* Stop if running and not forced */
	if (c->state == OCIFBSD_STATE_RUNNING && !force) {
		fprintf(stderr, "error: container is running, use --force to stop and delete\n");
		container_free(c);
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
		return (1);
	}

	printf("%s\n", id);

	container_free(c);
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
	while ((ch = getopt_long(argc, argv, "+h", longopts, NULL)) != -1) {
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

	/* Print state as JSON */
	printf("{\"id\":\"%s\",\"status\":\"%s\"}\n",
	    c->id, ocifbsd_state_to_string(c->state));

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
	while ((ch = getopt_long(argc, argv, "+h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'h':
			usage(argv[0], "list");
			return (0);
		default:
			usage(argv[0], "list");
			return (1);
		}
	}

	/* Get all containers */
	list = state_list(&n);
	if (list == NULL) {
		if (errno != 0)
			fprintf(stderr, "error: failed to list containers: %s\n",
			    strerror(errno));
		return (0);
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
	while ((ch = getopt_long(argc, argv, "+h", longopts, NULL)) != -1) {
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

	printf("%s\n", json);
	free(json);

	container_free(c);
	return (0);
}

static int
cmd_run(int argc, char **argv)
{
	const char *bundle = NULL;
	const char *name = NULL;
	struct ocifbsd_container *c;
	char *bundle_path;
	char *cname;
	int ret;

	/* Parse run-specific options */
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "+n:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'h':
			usage(argv[0], "run <bundle> [--name <name>]");
			return (0);
		default:
			usage(argv[0], "run");
			return (1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		fprintf(stderr, "error: bundle path required\n");
		usage(argv[-optind], "run");
		return (1);
	}

	bundle = argv[0];

	/* Resolve bundle path */
	bundle_path = resolve_bundle_path(bundle);
	if (bundle_path == NULL) {
		fprintf(stderr, "error: invalid bundle path: %s\n", bundle);
		return (1);
	}

	/* Canonicalize name if provided */
	if (name != NULL) {
		cname = canonical_name(name);
		if (cname == NULL) {
			fprintf(stderr, "error: invalid container name: %s\n", name);
			free(bundle_path);
			return (1);
		}
	} else {
		cname = NULL;
	}

	/* Create container */
	if (verbose) {
		fprintf(stderr, "Creating container from bundle: %s\n", bundle_path);
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
		fprintf(stderr, "error: failed to start container: %s\n",
		    strerror(errno));
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
	while ((ch = getopt_long(argc, argv, "+nh", longopts, NULL)) != -1) {
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
 * images — list known image store roots (filesystem scan of OCIFBSD_DATA_DIR).
 */
static int
cmd_images(int argc, char **argv)
{
	DIR *dir;
	struct dirent *ent;
	const char *base = OCIFBSD_DATA_DIR;
	char path[PATH_MAX];
	int found = 0;

	(void)argc;
	(void)argv;

	/*
	 * Image store layout (paths.c): /var/lib/ocifbsd/<registry>/...
	 * Until a catalog DB exists, list top-level registry directories.
	 */
	dir = opendir(base);
	if (dir == NULL) {
		if (errno == ENOENT) {
			/* empty store is success */
			return (0);
		}
		fprintf(stderr, "error: cannot open %s: %s\n", base,
		    strerror(errno));
		return (1);
	}

	printf("REPOSITORY\tTAG\tPATH\n");
	while ((ent = readdir(dir)) != NULL) {
		struct stat st;

		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", base, ent->d_name);
		if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
			continue;
		/* One row per registry root; finer listing comes with ZFS catalog */
		printf("%s\t-\t%s\n", ent->d_name, path);
		found++;
	}
	closedir(dir);
	(void)found;
	return (0);
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
	{ "pull",	cmd_pull,	"Resolve or pull an OCI image" },
	{ "images",	cmd_images,	"List local image store" },
	{ NULL,		NULL,		NULL },
};

int
main(int argc, char **argv)
{
	struct command *cmd;

	/* Parse global options */
	static struct option longopts[] = {
		{ "verbose",	no_argument,		NULL, 'v' },
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
	while ((ch = getopt_long(argc, argv, "+vhV", longopts, NULL)) != -1) {
		switch (ch) {
		case 'v':
			verbose = true;
			ocifbsd_set_verbose(true);
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
