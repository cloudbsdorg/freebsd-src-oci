/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * ocifbsd build: assemble an image from a Containerfile/Dockerfile.
 *
 * A build resolves its FROM base in the local image store (pulling it if
 * absent), copies that base rootfs into the target image directory, then
 * applies each instruction in order. RUN steps execute inside the assembled
 * rootfs via chroot(2) with devfs mounted and the host resolver copied in, so
 * pkg(8) installs reach the network through the host. The result is written to
 * the store as <base>/<registry>/<repo>/<tag>/{rootfs,config.json,
 * image-config.json}, exactly the layout pull(1) produces, so build output is
 * immediately runnable by create/run.
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pull.h"
#include "zfs_store.h"
#include "build.h"
#include "../include/ocifbsd.h"	/* ocifbsd_json_escape */

/* Accumulated image configuration as instructions are applied. */
struct build_config {
	char	**env;
	int	  nenv;
	char	**cmd;		/* exec-form argv, NULL-terminated conceptually */
	char	**entrypoint;
	char	 *workdir;
	char	 *user;
	char	**exposed;	/* e.g. "80/tcp" */
	int	  nexposed;
	char	**label_k;
	char	**label_v;
	int	  nlabel;
	char	 *arch;
	char	 *os;
};

/* One parsed Containerfile instruction. */
struct build_step {
	char	*op;		/* upper-cased opcode, e.g. "RUN" */
	char	*arg;		/* remainder of the line (trimmed) */
};

static void
strvec_free(char **v, int n)
{
	int i;

	if (v == NULL)
		return;
	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
}

/* Free a NULL-terminated argv (frees every element and the array). */
static void
argv_free(char **v)
{
	int i;

	if (v == NULL)
		return;
	for (i = 0; v[i] != NULL; i++)
		free(v[i]);
	free(v);
}

static int
strvec_push(char ***v, int *n, const char *s)
{
	char **nv = realloc(*v, (size_t)(*n + 1) * sizeof(char *));

	if (nv == NULL)
		return (-1);
	nv[*n] = strdup(s);
	if (nv[*n] == NULL) {
		*v = nv;
		return (-1);
	}
	*v = nv;
	(*n)++;
	return (0);
}

/* Read an entire file into a NUL-terminated buffer (caller frees). */
static char *
read_whole_file(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	long sz;
	size_t rd;

	if (f == NULL)
		return (NULL);
	if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
		fclose(f);
		return (NULL);
	}
	rewind(f);
	buf = malloc((size_t)sz + 1);
	if (buf == NULL) {
		fclose(f);
		return (NULL);
	}
	rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = '\0';
	return (buf);
}

static char *
trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '\0')
		return (s);
	e = s + strlen(s) - 1;
	while (e > s && (*e == ' ' || *e == '\t' || *e == '\r' || *e == '\n'))
		*e-- = '\0';
	return (s);
}

/*
 * Parse the Containerfile text into a list of steps. Blank lines and #comments
 * are dropped; a trailing backslash continues the logical line. Returns the
 * step count, or -1 on error; *out receives a malloc'd array (caller frees
 * each op/arg and the array).
 */
static int
parse_containerfile(const char *text, struct build_step **out)
{
	struct build_step *steps = NULL;
	int nsteps = 0;
	char *copy = strdup(text);
	char *acc = NULL;
	char *line, *save;

	if (copy == NULL)
		return (-1);

	for (line = strtok_r(copy, "\n", &save); line != NULL;
	    line = strtok_r(NULL, "\n", &save)) {
		char *t = trim(line);
		size_t len;
		bool cont;

		if (acc == NULL && (t[0] == '#' || t[0] == '\0'))
			continue;

		len = strlen(t);
		cont = (len > 0 && t[len - 1] == '\\');
		if (cont)
			t[len - 1] = '\0';	/* drop the backslash */

		if (acc == NULL) {
			acc = strdup(t);
		} else {
			char *na;

			if (asprintf(&na, "%s %s", acc, t) < 0)
				na = NULL;
			free(acc);
			acc = na;
		}
		if (acc == NULL)
			goto fail;
		if (cont)
			continue;	/* keep accumulating */

		/* Complete logical line in acc: split opcode + argument. */
		{
			char *sp = acc;
			struct build_step *ns;
			char *op, *arg;

			while (*sp != '\0' && *sp != ' ' && *sp != '\t')
				sp++;
			if (*sp != '\0') {
				*sp = '\0';
				arg = trim(sp + 1);
			} else {
				arg = sp;	/* empty */
			}
			op = acc;
			for (char *p = op; *p != '\0'; p++)
				*p = (char)toupper((unsigned char)*p);

			ns = realloc(steps,
			    (size_t)(nsteps + 1) * sizeof(*steps));
			if (ns == NULL)
				goto fail;
			steps = ns;
			steps[nsteps].op = strdup(op);
			steps[nsteps].arg = strdup(arg);
			if (steps[nsteps].op == NULL ||
			    steps[nsteps].arg == NULL)
				goto fail;
			nsteps++;
		}
		free(acc);
		acc = NULL;
	}

	free(acc);
	free(copy);
	*out = steps;
	return (nsteps);
fail:
	free(acc);
	free(copy);
	for (int i = 0; i < nsteps; i++) {
		free(steps[i].op);
		free(steps[i].arg);
	}
	free(steps);
	return (-1);
}

/*
 * If `arg` is JSON exec form (["a","b"]), return a NULL-terminated argv of the
 * elements (caller frees with argv_free). If it is shell form, return NULL —
 * the caller wraps the raw string as /bin/sh -c itself.
 */
static char **
parse_exec_array(const char *arg)
{
	char **v;
	int n = 0;
	const char *p;

	while (*arg == ' ' || *arg == '\t')
		arg++;
	if (arg[0] != '[')
		return (NULL);		/* shell form */

	v = calloc(1, sizeof(char *));	/* NULL-terminated empty vector */
	if (v == NULL)
		return (NULL);
	for (p = arg + 1; *p != '\0' && *p != ']';) {
		char item[PATH_MAX];
		size_t o = 0;
		char **nv;

		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;
		if (*p != '"')
			break;
		p++;
		while (*p != '\0' && *p != '"' && o < sizeof(item) - 1) {
			if (*p == '\\' && p[1] != '\0')
				p++;
			item[o++] = *p++;
		}
		item[o] = '\0';
		if (*p == '"')
			p++;
		nv = realloc(v, (size_t)(n + 2) * sizeof(char *));
		if (nv == NULL) {
			argv_free(v);
			return (NULL);
		}
		v = nv;
		v[n] = strdup(item);
		v[n + 1] = NULL;
		if (v[n] == NULL) {
			argv_free(v);
			return (NULL);
		}
		n++;
	}
	return (v);
}

/* Build a ["/bin/sh","-c",cmd] NULL-terminated argv. */
static char **
sh_wrap(const char *cmd)
{
	char **v = calloc(4, sizeof(char *));

	if (v == NULL)
		return (NULL);
	v[0] = strdup("/bin/sh");
	v[1] = strdup("-c");
	v[2] = strdup(cmd);
	if (v[0] == NULL || v[1] == NULL || v[2] == NULL) {
		argv_free(v);
		return (NULL);
	}
	return (v);
}

/* fork/exec argv; if chroot_dir != NULL the child chroots into it first and
 * applies env/workdir. Returns child exit status (0 == success) or -1. */
static int
run_cmd(const char *chroot_dir, const char *workdir, char *const env[],
    char *const argv[])
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return (-1);
	if (pid == 0) {
		if (chroot_dir != NULL) {
			if (chdir(chroot_dir) != 0 || chroot(chroot_dir) != 0)
				_exit(126);
			if (chdir(workdir != NULL ? workdir : "/") != 0)
				(void)chdir("/");
			if (env != NULL) {
				extern char **environ;
				environ = (char **)env;
			}
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return (-1);
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	return (-1);
}

/* Recursively copy src tree to dst using cp -a (system integration tool). */
static int
copy_tree(const char *src, const char *dst)
{
	char *const argv[] = { "cp", "-a", (char *)src, (char *)dst, NULL };

	return (run_cmd(NULL, NULL, NULL, argv));
}

static int
mkdirp_local(const char *path)
{
	char buf[PATH_MAX];
	char *p;

	if (strlcpy(buf, path, sizeof(buf)) >= sizeof(buf))
		return (-1);
	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, 0755) != 0 && errno != EEXIST)
			return (-1);
		*p = '/';
	}
	if (mkdir(buf, 0755) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

/* Resolve a reference to its store rootfs dir, pulling the image if absent. */
static char *
ensure_base_rootfs(const char *ref, int verbose)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;
	char *dir = NULL, *rootfs = NULL;
	struct stat st;

	if (parse_reference(ref, &registry, &repo, &tag, &digest) != 0)
		return (NULL);
	dir = zfs_image_path(registry, repo, tag);
	if (dir == NULL)
		goto out;
	if (asprintf(&rootfs, "%s/rootfs", dir) < 0) {
		rootfs = NULL;
		goto out;
	}
	if (stat(rootfs, &st) == 0 && S_ISDIR(st.st_mode))
		goto out;	/* already present */

	/* Not present: pull it. */
	if (verbose)
		printf("build: pulling base image %s\n", ref);
	{
		struct registry reg;

		if (registry_init(&reg, ref) != 0 ||
		    registry_pull(&reg, ref, dir, NULL, NULL) != 0) {
			free(rootfs);
			rootfs = NULL;
			goto out;
		}
	}
	if (stat(rootfs, &st) != 0 || !S_ISDIR(st.st_mode)) {
		free(rootfs);
		rootfs = NULL;
	}
out:
	free(registry);
	free(repo);
	free(tag);
	free(digest);
	free(dir);
	return (rootfs);
}

/* Emit a JSON array of quoted strings from a NULL-terminated argv. */
static void
json_str_array(FILE *f, char *const *v)
{
	char esc[4096];
	int i;

	fputc('[', f);
	for (i = 0; v != NULL && v[i] != NULL; i++) {
		if (i > 0)
			fputs(", ", f);
		fprintf(f, "\"%s\"", ocifbsd_json_escape(v[i], esc, sizeof(esc)));
	}
	fputc(']', f);
}

/* Write image-config.json (OCI image config) into destdir. */
static int
write_image_config(const char *destdir, const struct build_config *c)
{
	char path[PATH_MAX];
	char esc[4096];
	FILE *f;
	int i;

	snprintf(path, sizeof(path), "%s/image-config.json", destdir);
	f = fopen(path, "w");
	if (f == NULL)
		return (-1);
	fprintf(f, "{\n  \"architecture\": \"%s\",\n  \"os\": \"%s\",\n",
	    c->arch ? c->arch : "amd64", c->os ? c->os : "freebsd");
	fprintf(f, "  \"config\": {\n");
	fprintf(f, "    \"Env\": [");
	for (i = 0; i < c->nenv; i++)
		fprintf(f, "%s\"%s\"", i ? ", " : "",
		    ocifbsd_json_escape(c->env[i], esc, sizeof(esc)));
	fprintf(f, "],\n    \"Cmd\": ");
	json_str_array(f, c->cmd);
	fprintf(f, ",\n    \"Entrypoint\": ");
	json_str_array(f, c->entrypoint);
	fprintf(f, ",\n    \"WorkingDir\": \"%s\",\n",
	    ocifbsd_json_escape(c->workdir ? c->workdir : "/", esc, sizeof(esc)));
	fprintf(f, "    \"User\": \"%s\",\n",
	    ocifbsd_json_escape(c->user ? c->user : "", esc, sizeof(esc)));
	fprintf(f, "    \"ExposedPorts\": {");
	for (i = 0; i < c->nexposed; i++)
		fprintf(f, "%s\"%s\": {}", i ? ", " : "",
		    ocifbsd_json_escape(c->exposed[i], esc, sizeof(esc)));
	fprintf(f, "},\n    \"Labels\": {");
	for (i = 0; i < c->nlabel; i++) {
		char ek[1024], ev[2048];

		fprintf(f, "%s\"%s\": \"%s\"", i ? ", " : "",
		    ocifbsd_json_escape(c->label_k[i], ek, sizeof(ek)),
		    ocifbsd_json_escape(c->label_v[i], ev, sizeof(ev)));
	}
	fprintf(f, "}\n  }\n}\n");
	fclose(f);
	return (0);
}

/* Write config.json (OCI runtime spec) into destdir. */
static int
write_runtime_config(const char *destdir, const struct build_config *c)
{
	char path[PATH_MAX];
	char esc[4096];
	FILE *f;
	int i;
	char **args;

	snprintf(path, sizeof(path), "%s/config.json", destdir);
	f = fopen(path, "w");
	if (f == NULL)
		return (-1);

	/* Runtime process.args = entrypoint + cmd (or a default shell). */
	fprintf(f, "{\n  \"ociVersion\": \"1.0.2\",\n");
	fprintf(f, "  \"hostname\": \"ocifbsd\",\n");
	fprintf(f, "  \"process\": {\n    \"terminal\": false,\n");
	fprintf(f, "    \"user\": { \"uid\": 0, \"gid\": 0 },\n");
	fprintf(f, "    \"cwd\": \"%s\",\n",
	    ocifbsd_json_escape(c->workdir ? c->workdir : "/", esc, sizeof(esc)));
	fprintf(f, "    \"args\": [");
	{
		bool first = true;

		args = c->entrypoint;
		for (i = 0; args != NULL && args[i] != NULL; i++) {
			fprintf(f, "%s\"%s\"", first ? "" : ", ",
			    ocifbsd_json_escape(args[i], esc, sizeof(esc)));
			first = false;
		}
		args = c->cmd;
		for (i = 0; args != NULL && args[i] != NULL; i++) {
			fprintf(f, "%s\"%s\"", first ? "" : ", ",
			    ocifbsd_json_escape(args[i], esc, sizeof(esc)));
			first = false;
		}
		if (first)
			fprintf(f, "\"/bin/sh\"");
	}
	fprintf(f, "],\n    \"env\": [");
	for (i = 0; i < c->nenv; i++)
		fprintf(f, "%s\"%s\"", i ? ", " : "",
		    ocifbsd_json_escape(c->env[i], esc, sizeof(esc)));
	if (c->nenv == 0)
		fprintf(f, "\"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:"
		    "/usr/bin:/sbin:/bin\"");
	fprintf(f, "]\n  },\n");
	fprintf(f, "  \"root\": { \"path\": \"rootfs\", \"readonly\": false }\n");
	fprintf(f, "}\n");
	fclose(f);
	return (0);
}

/* Set/replace an "K=V" entry in the env list. */
static int
env_set(struct build_config *c, const char *kv)
{
	const char *eq = strchr(kv, '=');
	size_t klen;
	int i;

	if (eq == NULL)
		return (strvec_push(&c->env, &c->nenv, kv));
	klen = (size_t)(eq - kv);
	for (i = 0; i < c->nenv; i++) {
		if (strncmp(c->env[i], kv, klen) == 0 && c->env[i][klen] == '=') {
			free(c->env[i]);
			c->env[i] = strdup(kv);
			return (c->env[i] == NULL ? -1 : 0);
		}
	}
	return (strvec_push(&c->env, &c->nenv, kv));
}

int
image_build(const char *containerfile, const char *context, const char *tag,
    int verbose)
{
	char *text = NULL;
	struct build_step *steps = NULL;
	int nsteps, i, rc = 1;
	struct build_config cfg;
	char *destdir = NULL, *destrootfs = NULL, *baserootfs = NULL;
	char *dregistry = NULL, *drepo = NULL, *dtag = NULL, *ddigest = NULL;
	bool devfs_mounted = false;

	memset(&cfg, 0, sizeof(cfg));
	cfg.arch = strdup("amd64");
	cfg.os = strdup("freebsd");

	text = read_whole_file(containerfile);
	if (text == NULL) {
		fprintf(stderr, "build: cannot read %s: %s\n", containerfile,
		    strerror(errno));
		return (1);
	}
	nsteps = parse_containerfile(text, &steps);
	if (nsteps <= 0) {
		fprintf(stderr, "build: empty or unparseable Containerfile\n");
		goto out;
	}
	if (strcmp(steps[0].op, "FROM") != 0) {
		fprintf(stderr, "build: first instruction must be FROM\n");
		goto out;
	}

	/* Resolve the destination image dir from the tag. */
	if (parse_reference(tag, &dregistry, &drepo, &dtag, &ddigest) != 0) {
		fprintf(stderr, "build: invalid tag: %s\n", tag);
		goto out;
	}
	destdir = zfs_image_path(dregistry, drepo, dtag);
	if (destdir == NULL || asprintf(&destrootfs, "%s/rootfs", destdir) < 0) {
		fprintf(stderr, "build: cannot compute destination path\n");
		goto out;
	}

	/* FROM: get the base rootfs and copy it into the destination. */
	baserootfs = ensure_base_rootfs(steps[0].arg, verbose);
	if (baserootfs == NULL) {
		fprintf(stderr, "build: cannot resolve base image %s\n",
		    steps[0].arg);
		goto out;
	}
	if (mkdirp_local(destdir) != 0) {
		fprintf(stderr, "build: cannot create %s\n", destdir);
		goto out;
	}
	/* Fresh rootfs: remove any stale one, then copy the base in. */
	{
		char *const rm[] = { "rm", "-rf", destrootfs, NULL };

		(void)run_cmd(NULL, NULL, NULL, rm);
	}
	if (verbose)
		printf("build: FROM %s -> copying base rootfs\n", steps[0].arg);
	if (copy_tree(baserootfs, destrootfs) != 0) {
		fprintf(stderr, "build: failed to copy base rootfs\n");
		goto out;
	}

	/* Apply the remaining instructions in order. */
	for (i = 1; i < nsteps; i++) {
		const char *op = steps[i].op;
		char *arg = steps[i].arg;

		if (verbose)
			printf("build: step %d/%d: %s %s\n", i, nsteps - 1, op,
			    arg);

		if (strcmp(op, "RUN") == 0) {
			char *const shargv[] = { "/bin/sh", "-c", arg, NULL };
			char **envp;
			int st;

			/* Lazily set up the rootfs for chrooted execution:
			 * mount devfs so pkg(8) has /dev, and copy the host
			 * resolver so it can reach the network via NAT. */
			if (!devfs_mounted) {
				char devdir[PATH_MAX], rbuf[PATH_MAX];
				char *marg[6];
				char *cpr[4];

				snprintf(devdir, sizeof(devdir), "%s/dev",
				    destrootfs);
				(void)mkdirp_local(devdir);
				marg[0] = "mount"; marg[1] = "-t";
				marg[2] = "devfs"; marg[3] = "devfs";
				marg[4] = devdir; marg[5] = NULL;
				(void)run_cmd(NULL, NULL, NULL, marg);
				devfs_mounted = true;

				snprintf(rbuf, sizeof(rbuf), "%s/etc/resolv.conf",
				    destrootfs);
				cpr[0] = "cp"; cpr[1] = "/etc/resolv.conf";
				cpr[2] = rbuf; cpr[3] = NULL;
				(void)run_cmd(NULL, NULL, NULL, cpr);
			}

			/* env vector: cfg.env + NULL. */
			envp = calloc((size_t)cfg.nenv + 1, sizeof(char *));
			if (envp != NULL) {
				int e;

				for (e = 0; e < cfg.nenv; e++)
					envp[e] = cfg.env[e];
			}
			st = run_cmd(destrootfs, cfg.workdir, envp, shargv);
			free(envp);
			if (st != 0) {
				fprintf(stderr, "build: RUN failed (exit %d): "
				    "%s\n", st, arg);
				goto out;
			}
		} else if (strcmp(op, "COPY") == 0 || strcmp(op, "ADD") == 0) {
			/* COPY <src...> <dst> — split on whitespace. */
			char *fields[64];
			int nf = 0;
			char *tok, *sv;
			char srcpath[PATH_MAX], dstpath[PATH_MAX];
			char *argcopy = strdup(arg);

			if (argcopy == NULL)
				goto out;
			for (tok = strtok_r(argcopy, " \t", &sv);
			    tok != NULL && nf < 63;
			    tok = strtok_r(NULL, " \t", &sv))
				fields[nf++] = tok;
			if (nf < 2) {
				fprintf(stderr, "build: %s needs src and dst\n",
				    op);
				free(argcopy);
				goto out;
			}
			snprintf(dstpath, sizeof(dstpath), "%s%s%s", destrootfs,
			    fields[nf - 1][0] == '/' ? "" : "/",
			    fields[nf - 1]);
			for (int s = 0; s < nf - 1; s++) {
				char *const cp[] = { "cp", "-R", srcpath,
				    dstpath, NULL };

				snprintf(srcpath, sizeof(srcpath), "%s/%s",
				    context, fields[s]);
				if (run_cmd(NULL, NULL, NULL, cp) != 0) {
					fprintf(stderr, "build: %s %s failed\n",
					    op, fields[s]);
					free(argcopy);
					goto out;
				}
			}
			free(argcopy);
		} else if (strcmp(op, "ENV") == 0) {
			/* "K=V" or "K V" (single pair). */
			char *eq = strchr(arg, '=');
			char kv[4096];

			if (eq != NULL) {
				if (env_set(&cfg, arg) != 0)
					goto out;
			} else {
				char *sp = strpbrk(arg, " \t");

				if (sp != NULL) {
					*sp = '\0';
					snprintf(kv, sizeof(kv), "%s=%s", arg,
					    trim(sp + 1));
					if (env_set(&cfg, kv) != 0)
						goto out;
				}
			}
		} else if (strcmp(op, "WORKDIR") == 0) {
			char wd[PATH_MAX];

			free(cfg.workdir);
			cfg.workdir = strdup(arg);
			/* Create it in the rootfs so a later RUN can cd there. */
			snprintf(wd, sizeof(wd), "%s%s%s", destrootfs,
			    arg[0] == '/' ? "" : "/", arg);
			(void)mkdirp_local(wd);
		} else if (strcmp(op, "USER") == 0) {
			free(cfg.user);
			cfg.user = strdup(arg);
		} else if (strcmp(op, "CMD") == 0) {
			char **v = parse_exec_array(arg);

			argv_free(cfg.cmd);
			cfg.cmd = (v != NULL) ? v : sh_wrap(arg);
		} else if (strcmp(op, "ENTRYPOINT") == 0) {
			char **v = parse_exec_array(arg);

			argv_free(cfg.entrypoint);
			cfg.entrypoint = (v != NULL) ? v : sh_wrap(arg);
		} else if (strcmp(op, "EXPOSE") == 0) {
			char *tok, *sv;
			char *ac = strdup(arg);

			for (tok = strtok_r(ac, " \t", &sv); tok != NULL;
			    tok = strtok_r(NULL, " \t", &sv)) {
				char port[64];

				if (strchr(tok, '/') != NULL)
					snprintf(port, sizeof(port), "%s", tok);
				else
					snprintf(port, sizeof(port), "%s/tcp",
					    tok);
				(void)strvec_push(&cfg.exposed, &cfg.nexposed,
				    port);
			}
			free(ac);
		} else if (strcmp(op, "LABEL") == 0) {
			/* LABEL key=value (single pair per line). */
			char *eq = strchr(arg, '=');

			if (eq != NULL) {
				int nk = cfg.nlabel, nv = cfg.nlabel;

				*eq = '\0';
				if (strvec_push(&cfg.label_k, &nk,
				    trim(arg)) == 0 &&
				    strvec_push(&cfg.label_v, &nv,
				    trim(eq + 1)) == 0)
					cfg.nlabel = nk;	/* == old+1 */
			}
		} else if (strcmp(op, "FROM") == 0) {
			fprintf(stderr, "build: multi-stage FROM not supported "
			    "yet\n");
			goto out;
		} else if (strcmp(op, "MAINTAINER") == 0 ||
		    strcmp(op, "ARG") == 0) {
			/* Accepted and ignored for now. */
		} else {
			fprintf(stderr, "build: unknown instruction %s\n", op);
			goto out;
		}
	}

	/* Unmount devfs before finalizing (best effort). */
	if (devfs_mounted) {
		char devdir[PATH_MAX];
		char *const um[] = { "umount", "-f", devdir, NULL };

		snprintf(devdir, sizeof(devdir), "%s/dev", destrootfs);
		(void)run_cmd(NULL, NULL, NULL, um);
		devfs_mounted = false;
	}

	if (write_image_config(destdir, &cfg) != 0 ||
	    write_runtime_config(destdir, &cfg) != 0) {
		fprintf(stderr, "build: failed to write image config\n");
		goto out;
	}

	printf("build: successfully built %s\n", tag);
	printf("  image dir: %s\n", destdir);
	rc = 0;
out:
	if (devfs_mounted) {
		char devdir[PATH_MAX];
		char *const um[] = { "umount", "-f", devdir, NULL };

		snprintf(devdir, sizeof(devdir), "%s/dev", destrootfs);
		(void)run_cmd(NULL, NULL, NULL, um);
	}
	free(text);
	if (steps != NULL) {
		for (i = 0; i < nsteps; i++) {
			free(steps[i].op);
			free(steps[i].arg);
		}
		free(steps);
	}
	strvec_free(cfg.env, cfg.nenv);
	argv_free(cfg.cmd);
	argv_free(cfg.entrypoint);
	free(cfg.workdir);
	free(cfg.user);
	strvec_free(cfg.exposed, cfg.nexposed);
	strvec_free(cfg.label_k, cfg.nlabel);
	strvec_free(cfg.label_v, cfg.nlabel);
	free(cfg.arch);
	free(cfg.os);
	free(destdir);
	free(destrootfs);
	free(baserootfs);
	free(dregistry);
	free(drepo);
	free(dtag);
	free(ddigest);
	return (rc);
}
