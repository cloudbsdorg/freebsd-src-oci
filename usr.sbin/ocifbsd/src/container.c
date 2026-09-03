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
 * Container lifecycle management
 */

#include <sys/param.h>
#include <sys/jail.h>
#include <sys/mac.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syslimits.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <jail.h>
#include <libutil.h>
#include <limits.h>
#include <paths.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ocifbsd.h"
#include "network/netcfg.h"
#include "network/network.h"
#include "security/rctl.h"

/* setproctitle(3) is in <libutil.h> on FreeBSD but the declaration is
 * not visible with the strict feature-test macros used here. Declare
 * it locally to avoid the implicit-function-declaration -Werror. */
extern void setproctitle(const char *fmt, ...);

/* putenv(3) is in <stdlib.h> but on FreeBSD 16 it is gated on
 * __XSI_VISIBLE, which we don't enable (we use __POSIX_VISIBLE=200809
 * via _POSIX_C_SOURCE). Declare it locally. */
extern int putenv(char *string);

/*
 * Return true if process `pid` currently exists and, when jid > 0, belongs
 * to jail `jid`. Used to avoid signaling a recycled PID after a container's
 * init has exited. Implemented in procutil.c (which needs <sys/user.h>,
 * incompatible with this file's strict feature-test macros).
 */
extern bool pid_in_jail(pid_t pid, int jid);

/* Global container registry */
static struct ocifbsd_container **container_registry = NULL;
static int container_registry_size = 0;
static int container_registry_capacity = 0;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Add container to registry
 */
static int
container_register(struct ocifbsd_container *c)
{
	int i;

	pthread_mutex_lock(&registry_lock);

	/* Check if already exists */
	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    strcmp(container_registry[i]->id, c->id) == 0) {
			pthread_mutex_unlock(&registry_lock);
			errno = EEXIST;
			return (-1);
		}
	}

	/* Expand capacity if needed */
	if (container_registry_size >= container_registry_capacity) {
		int newcap = container_registry_capacity ?
		    container_registry_capacity * 2 : 16;
		void *_t = realloc(container_registry,
		    newcap * sizeof(*container_registry));
		if (_t == NULL) {
			/*
			 * Keep the old registry (pointer AND capacity) intact on
			 * failure: publishing the doubled capacity before the
			 * realloc actually grew the buffer would let the next
			 * insert write past the still-old-sized array.
			 */
			pthread_mutex_unlock(&registry_lock);
			errno = ENOMEM;
			return (-1);
		}
		container_registry = _t;
		container_registry_capacity = newcap;
	}

	container_registry[container_registry_size++] = c;

	pthread_mutex_unlock(&registry_lock);
	return (0);
}

/*
 * Remove container from registry
 */
static int
container_unregister(const char *id)
{
	int i;

	pthread_mutex_lock(&registry_lock);

	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    strcmp(container_registry[i]->id, id) == 0) {
			container_registry[i] = NULL;
			pthread_mutex_unlock(&registry_lock);
			return (0);
		}
	}

	pthread_mutex_unlock(&registry_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Get container from registry by ID
 */
struct ocifbsd_container *
container_get_by_id(const char *id)
{
	int i;
	struct ocifbsd_container *found = NULL;

	if (id == NULL)
		return (NULL);

	pthread_mutex_lock(&registry_lock);

	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    strcmp(container_registry[i]->id, id) == 0) {
			found = container_registry[i];
			break;
		}
	}

	pthread_mutex_unlock(&registry_lock);

	/* Try loading from state if not in memory */
	if (found == NULL) {
		found = state_load(id);
		if (found != NULL) {
			container_register(found);
		}
	}

	return (found);
}

/*
 * Get container by name
 */
struct ocifbsd_container *
container_get_by_name(const char *name)
{
	int i;
	struct ocifbsd_container *found = NULL;

	if (name == NULL)
		return (NULL);

	pthread_mutex_lock(&registry_lock);

	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    container_registry[i]->name != NULL &&
		    strcmp(container_registry[i]->name, name) == 0) {
			found = container_registry[i];
			break;
		}
	}

	pthread_mutex_unlock(&registry_lock);

	return (found);
}

/*
 * Get container by jail ID
 */
struct ocifbsd_container *
container_get_by_jid(int jid)
{
	int i;
	struct ocifbsd_container *found = NULL;

	pthread_mutex_lock(&registry_lock);

	for (i = 0; i < container_registry_size; i++) {
		if (container_registry[i] != NULL &&
		    container_registry[i]->jid == jid) {
			found = container_registry[i];
			break;
		}
	}

	pthread_mutex_unlock(&registry_lock);

	return (found);
}

/*
 * Free container structure
 */
void
container_free(struct ocifbsd_container *c)
{
	int i;

	if (c == NULL)
		return;

	/*
	 * Detach from the registry before freeing. container_get_by_id()
	 * registers any container it loads from disk, so a caller that looks
	 * one up and then frees it would otherwise leave a dangling pointer in
	 * container_registry; the next lookup of the same id would return freed
	 * memory. Unregister first (it matches on c->id, so it must run before
	 * c->id is freed). A container that was never registered simply matches
	 * nothing, which is harmless.
	 */
	if (c->id != NULL)
		(void)container_unregister(c->id);

	free(c->id);
	free(c->name);
	free(c->rootfs);
	free(c->bundle_path);
	free(c->config_path);
	free(c->log_path);

	if (c->applied_mounts != NULL) {
		for (i = 0; i < c->n_applied_mounts; i++)
			free(c->applied_mounts[i]);
		free(c->applied_mounts);
	}

	if (c->spec != NULL)
		oci_free_spec(c->spec);

	free(c);
}

/*
 * Run /sbin/mount or /sbin/umount and wait. Returns 0 on success.
 */
static int
run_mount_cmd(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return (-1);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		errno = EIO;
		return (-1);
	}
	return (0);
}

/*
 * Verify that a mount/umount target really resolves to a location inside the
 * container rootfs, defeating symlink escapes. A pulled image can legitimately
 * contain in-rootfs symlinks with absolute targets (e.g. rootfs/dev -> /etc);
 * mount(8)/umount(8) follow them, so without this a crafted image could mount
 * over — or force-unmount — an arbitrary HOST path as root.
 *
 * realpath() resolves every symlink in the path (leaf and parents); the result
 * must be prefixed by the real rootfs. If the target does not exist yet (a
 * mount point about to be created), fall back to resolving its parent so a
 * symlinked parent is still caught. Returns true only when the resolved path is
 * provably within the rootfs.
 */
static bool
mount_target_within_rootfs(const char *rootfs, const char *target)
{
	char real_root[PATH_MAX];
	char real_target[PATH_MAX];
	size_t rl;

	if (rootfs == NULL || target == NULL)
		return (false);
	if (realpath(rootfs, real_root) == NULL)
		return (false);
	rl = strlen(real_root);

	if (realpath(target, real_target) == NULL) {
		/* Not present yet: resolve the parent directory instead. */
		char tmp[PATH_MAX];
		char *slash;

		if (strlcpy(tmp, target, sizeof(tmp)) >= sizeof(tmp))
			return (false);
		slash = strrchr(tmp, '/');
		if (slash == NULL || slash == tmp)
			return (false);
		*slash = '\0';
		if (realpath(tmp, real_target) == NULL)
			return (false);
	}

	/* real_target must equal real_root or sit strictly beneath it. */
	return (strncmp(real_target, real_root, rl) == 0 &&
	    (real_target[rl] == '\0' || real_target[rl] == '/'));
}

/*
 * Map OCI mount type names to FreeBSD vfs types. Returns NULL to skip
 * Linux-only filesystems we do not implement.
 */
static const char *
oci_mount_type_to_fbsd(const char *type)
{
	if (type == NULL)
		return (NULL);
	if (strcmp(type, "nullfs") == 0 || strcmp(type, "bind") == 0)
		return ("nullfs");
	if (strcmp(type, "devfs") == 0 || strcmp(type, "dev") == 0)
		return ("devfs");
	if (strcmp(type, "procfs") == 0 || strcmp(type, "proc") == 0)
		return ("procfs");
	if (strcmp(type, "fdescfs") == 0)
		return ("fdescfs");
	if (strcmp(type, "tmpfs") == 0)
		return ("tmpfs");
	/* Explicit FreeBSD alias used in some configs */
	if (strcmp(type, "linprocfs") == 0)
		return ("linprocfs");
	return (NULL);
}

static int
record_applied_mount(struct ocifbsd_container *c, const char *target)
{
	char **nm;
	char *copy;

	copy = strdup(target);
	if (copy == NULL)
		return (-1);
	nm = realloc(c->applied_mounts,
	    (size_t)(c->n_applied_mounts + 1) * sizeof(char *));
	if (nm == NULL) {
		free(copy);
		return (-1);
	}
	c->applied_mounts = nm;
	c->applied_mounts[c->n_applied_mounts++] = copy;
	return (0);
}

/*
 * Report whether the MAC labeling policy named by a label's prefix is loaded.
 * A label is "<policy>/<detail>" (e.g. "biba/high"); a loaded policy registers
 * a "security.mac.<policy>.enabled" sysctl. mac_from_text() is not a usable
 * probe here — it converts text to an internal form and succeeds even with no
 * policy loaded. Returns false for a NULL/blank/malformed label.
 */
static bool
mac_policy_for_label_loaded(const char *label)
{
	char name[64];
	char oid[128];
	const char *slash;
	size_t nlen;
	int enabled;
	size_t sz = sizeof(enabled);

	if (label == NULL || label[0] == '\0')
		return (false);
	slash = strchr(label, '/');
	nlen = (slash != NULL) ? (size_t)(slash - label) : strlen(label);
	if (nlen == 0 || nlen >= sizeof(name))
		return (false);
	memcpy(name, label, nlen);
	name[nlen] = '\0';

	snprintf(oid, sizeof(oid), "security.mac.%s.enabled", name);
	if (sysctlbyname(oid, &enabled, &sz, NULL, 0) != 0)
		return (false);
	return (enabled != 0);
}

/*
 * Warn about OCI security-context restrictions this bundle requests but that
 * ocifbsd does not enforce yet, so an operator is never misled into believing a
 * container is hardened when it is not. root.readonly, process.noNewPrivileges,
 * process.rlimits, freebsd.rctl_rules and freebsd.vnet ARE enforced and are not
 * warned about; only the per-path linux.readonlyPaths / linux.maskedPaths remain
 * parsed-only (see docs/security-restrictions.md).
 */
static void
warn_unenforced_security(const struct oci_runtime_spec *spec)
{
	if (spec == NULL)
		return;
	/*
	 * root.readonly and process.noNewPrivileges ARE enforced (the root is
	 * established as a read-only / nosuid nullfs mount; see
	 * establish_secure_rootfs), so they are not warned about. Per-path
	 * readonlyPaths and maskedPaths are still parsed-only.
	 */
	if (spec->n_readonly_paths > 0)
		fprintf(stderr, "warning: linux.readonlyPaths (%d) recorded but "
		    "NOT yet enforced; those paths remain writable\n",
		    spec->n_readonly_paths);
	if (spec->n_masked_paths > 0)
		fprintf(stderr, "warning: linux.maskedPaths (%d) recorded but "
		    "NOT yet enforced; those paths are not masked\n",
		    spec->n_masked_paths);
	if (spec->freebsd != NULL && spec->freebsd->mac_label != NULL &&
	    !mac_policy_for_label_loaded(spec->freebsd->mac_label)) {
		/*
		 * The label IS applied to the container's init process (see
		 * apply_process_mac_label in container_start), but only takes
		 * effect when the matching MAC labeling policy is loaded. Warn
		 * only when it is absent, so the operator is not misled into
		 * believing an unenforceable label is active.
		 */
		fprintf(stderr, "warning: freebsd.macLabel '%s' set but its MAC "
		    "policy is not loaded (labeling policies load at boot, e.g. "
		    "mac_biba_load=YES); it will not be enforced\n",
		    spec->freebsd->mac_label);
	}
}

/*
 * Apply a MAC label to the current (container init) process via mac_set_proc,
 * best-effort. Called in the start child after jail_attach so the label lands
 * on the container's init. Returns 0 on success or when there is nothing to
 * do, -1 (with a warning already emitted) when a label was requested but could
 * not be applied — the caller proceeds regardless, matching the runtime's
 * "map honestly, do not fail closed on advisory hardening" behavior.
 */
static int
apply_process_mac_label(const char *label)
{
	mac_t mac;

	if (label == NULL || label[0] == '\0')
		return (0);

	if (mac_from_text(&mac, label) != 0) {
		fprintf(stderr, "warning: cannot parse MAC label '%s': %s\n",
		    label, strerror(errno));
		return (-1);
	}
	if (mac_set_proc(mac) != 0) {
		fprintf(stderr, "warning: cannot apply MAC label '%s' (is a "
		    "labeling policy loaded?): %s\n", label, strerror(errno));
		mac_free(mac);
		return (-1);
	}
	mac_free(mac);
	return (0);
}

/*
 * Compose the kernel jail name for a container. Must match the name used in
 * create_jail_from_spec ("ocifbsd-<first 12 chars of id>") so RCTL rules and
 * teardown address the same jail subject.
 */
static void
container_jail_name(const struct ocifbsd_container *c, char *buf, size_t len)
{
	snprintf(buf, len, "ocifbsd-%.12s", c->id != NULL ? c->id : "");
}

/*
 * Path of the sidecar that records the host-side epair name for a VNET
 * container, so container_delete can destroy it (the kernel does not reclaim
 * the host side when the jail is removed). Lives next to the other per-id
 * state under OCIFBSD_STATE_DIR.
 */
static int
container_epair_sidecar(const struct ocifbsd_container *c, char *buf,
	size_t len)
{
	if (c == NULL || c->id == NULL)
		return (-1);
	if ((size_t)snprintf(buf, len, "%s/%s.epair", state_base_dir(),
	    c->id) >= len)
		return (-1);
	return (0);
}

/*
 * Set up VNET connectivity for a container: when the bundle requests
 * freebsd.vnet, create an epair, move its peer into the jail, and configure
 * the container's first IPv4 address and default gateway inside the jail. The
 * host-side interface name is persisted so it can be destroyed on delete.
 * Best-effort: a non-VNET container is a no-op, and a setup failure warns
 * rather than aborting the start.
 */
static void
setup_container_network(const struct ocifbsd_container *c)
{
	const struct oci_freebsd *fb;
	const char *ip = NULL, *gw = NULL, *bridge = NULL;
	char *side_a = NULL;
	char path[PATH_MAX];
	FILE *f;

	if (c == NULL || c->spec == NULL || c->spec->freebsd == NULL)
		return;
	fb = c->spec->freebsd;
	if (!fb->vnet)
		return;
	if (fb->n_ip4 > 0)
		ip = fb->ip4[0];
	if (fb->n_default_gateway4 > 0)
		gw = fb->default_gateway4[0];
	bridge = fb->bridge;

	if (vnet_wire_jail(c->jid, ip, gw, bridge, &side_a) != 0) {
		fprintf(stderr, "warning: VNET setup failed for %s; the "
		    "container has an isolated network stack\n", c->id);
		return;
	}

	f = NULL;
	if (container_epair_sidecar(c, path, sizeof(path)) == 0)
		f = fopen(path, "w");
	if (f != NULL) {
		fprintf(f, "%s\n", side_a);
		fclose(f);
	} else if (side_a != NULL) {
		/*
		 * Without the persisted sidecar, teardown at delete time cannot
		 * learn the host-side epair name, so the interface would leak
		 * until reboot. Destroy it now rather than orphan it.
		 */
		fprintf(stderr, "warning: cannot record epair sidecar for %s; "
		    "removing host interface %s to avoid a leak\n", c->id,
		    side_a);
		(void)epair_delete(side_a);
	}
	free(side_a);
}

/*
 * Tear down VNET connectivity: destroy the host-side epair recorded at start.
 * Destroying either side removes the pair (and any bridge membership). A
 * missing sidecar (non-VNET container, or one started before this was wired)
 * is a no-op.
 */
static void
teardown_container_network(const struct ocifbsd_container *c)
{
	char path[PATH_MAX];
	char line[64];
	FILE *f;

	if (container_epair_sidecar(c, path, sizeof(path)) != 0)
		return;
	f = fopen(path, "r");
	if (f == NULL)
		return;
	if (fgets(line, sizeof(line), f) != NULL) {
		line[strcspn(line, " \t\r\n")] = '\0';
		if (line[0] != '\0')
			(void)epair_delete(line);
	}
	fclose(f);
	(void)unlink(path);
}

/*
 * Apply RCTL resource limits from the OCI linux.resources object, if the
 * bundle declares one. Opt-in and best-effort: a bundle with no resources is
 * a no-op, and when the kernel has RACCT/RCTL disabled (kern.racct.enable=0)
 * we warn once rather than failing the start, so containers still run on hosts
 * without accounting compiled in or enabled.
 */
static void
apply_resource_limits(const struct ocifbsd_container *c)
{
	struct rctl_limits limits;
	char jname[MAXHOSTNAMELEN];

	if (c == NULL || c->spec == NULL ||
	    c->spec->linux_resources_json == NULL)
		return;

	if (!rctl_check_available()) {
		fprintf(stderr, "warning: container %s declares resource limits "
		    "but RACCT/RCTL is unavailable (enable kern.racct.enable=1); "
		    "limits not applied\n", c->id);
		return;
	}

	if (rctl_parse_oci_resources(&limits, c->spec->linux_resources_json)
	    != 0)
		return;

	container_jail_name(c, jname, sizeof(jname));
	if (rctl_apply_rules(jname, &limits) != 0)
		fprintf(stderr, "warning: failed to apply some RCTL limits for "
		    "%s\n", c->id);
}

/*
 * Apply OCI mounts into the container rootfs (host-side, before start).
 * Unsupported Linux types are skipped with a warning.
 */
int
container_apply_mounts(struct ocifbsd_container *c)
{
	struct oci_runtime_spec *spec;
	int i;

	if (c == NULL || c->rootfs == NULL) {
		errno = EINVAL;
		return (-1);
	}
	spec = c->spec;

	/*
	 * Always provide /dev. A container with no devfs mount has no
	 * /dev/null, /dev/random, etc., and most services (php-fpm, sshd, and
	 * many daemons) fail to initialize ("open(\"/dev/null\"): No such file
	 * or directory"). Built/pulled images rarely carry a mounts array, so
	 * mount devfs at <rootfs>/dev — ruleset 4, the jail-safe device subset —
	 * unless the spec already mounts something there.
	 */
	{
		bool have_dev = false;
		char dest[PATH_MAX];
		char *argv[8];
		int argc = 0, k;

		for (k = 0; spec != NULL && spec->mounts != NULL &&
		    k < spec->n_mounts; k++) {
			const char *d = spec->mounts[k].destination;

			if (d != NULL && strcmp(d, "/dev") == 0) {
				have_dev = true;
				break;
			}
		}
		if (!have_dev) {
			snprintf(dest, sizeof(dest), "%s/dev", c->rootfs);
			(void)ensure_directory(dest, 0755);
			if (!mount_target_within_rootfs(c->rootfs, dest)) {
				fprintf(stderr, "warning: refusing devfs mount: "
				    "%s escapes the container rootfs\n", dest);
				goto spec_mounts;
			}
			argv[argc++] = __DECONST(char *, "/sbin/mount");
			argv[argc++] = __DECONST(char *, "-t");
			argv[argc++] = __DECONST(char *, "devfs");
			argv[argc++] = __DECONST(char *, "-o");
			argv[argc++] = __DECONST(char *, "ruleset=4");
			argv[argc++] = __DECONST(char *, "devfs");
			argv[argc++] = dest;
			argv[argc] = NULL;
			if (run_mount_cmd(argv) == 0)
				(void)record_applied_mount(c, dest);
			else
				fprintf(stderr, "warning: default devfs mount at "
				    "%s failed: %s\n", dest, strerror(errno));
		}
	}

spec_mounts:
	if (spec == NULL || spec->n_mounts <= 0 || spec->mounts == NULL)
		return (0);

	for (i = 0; i < spec->n_mounts; i++) {
		struct oci_mount *m = &spec->mounts[i];
		const char *fstype;
		char dest[PATH_MAX];
		char *argv[10];
		int argc = 0;
		const char *source;
		char opts[256];
		struct stat sb;

		if (m->destination == NULL || m->destination[0] == '\0')
			continue;

		/*
		 * Defense in depth: oci_validate_spec already rejects a config
		 * whose destination escapes the root, but a reloaded spec reaches
		 * here without re-validation, so refuse any ".." traversal rather
		 * than mount outside the rootfs.
		 */
		if (!oci_path_is_safe(m->destination)) {
			fprintf(stderr, "warning: skipping unsafe mount "
			    "destination (path traversal): %s\n",
			    m->destination);
			continue;
		}

		fstype = oci_mount_type_to_fbsd(m->type);
		if (fstype == NULL) {
			fprintf(stderr,
			    "warning: skipping unsupported mount type '%s' -> %s\n",
			    m->type ? m->type : "(null)", m->destination);
			continue;
		}

		if ((size_t)snprintf(dest, sizeof(dest),
		    m->destination[0] == '/' ? "%s%s" : "%s/%s",
		    c->rootfs, m->destination) >= sizeof(dest)) {
			/*
			 * A truncated join can name a host path above the
			 * container root (mount/umount then act on it as root);
			 * refuse rather than operate on a shortened path.
			 */
			fprintf(stderr, "warning: skipping mount, path too long: "
			    "%s%s\n", c->rootfs, m->destination);
			continue;
		}

		if (stat(dest, &sb) != 0) {
			if (ensure_directory(dest, 0755) != 0) {
				fprintf(stderr,
				    "warning: cannot create mount point %s: %s\n",
				    dest, strerror(errno));
				continue;
			}
		}

		/*
		 * Symlink-escape guard: oci_path_is_safe rejects ".." components
		 * but not symlinks planted in the image rootfs. Confirm the join
		 * really resolves inside the rootfs before mounting over it.
		 */
		if (!mount_target_within_rootfs(c->rootfs, dest)) {
			fprintf(stderr, "warning: skipping mount, destination "
			    "escapes the container rootfs (symlink?): %s\n", dest);
			continue;
		}

		/* Build -o options: readonly flag + any options string */
		opts[0] = '\0';
		if (m->readonly)
			strlcpy(opts, "ro", sizeof(opts));
		if (m->options != NULL && m->options[0] != '\0') {
			if (opts[0] != '\0')
				strlcat(opts, ",", sizeof(opts));
			strlcat(opts, m->options, sizeof(opts));
		}
		/* jail-friendly default for devfs */
		if (strcmp(fstype, "devfs") == 0 && opts[0] == '\0')
			strlcpy(opts, "ruleset=4", sizeof(opts));

		if (strcmp(fstype, "nullfs") == 0) {
			source = m->source != NULL ? m->source : "";
			if (source[0] == '\0') {
				fprintf(stderr,
				    "warning: nullfs mount missing source for %s\n",
				    dest);
				continue;
			}
		} else if (strcmp(fstype, "tmpfs") == 0) {
			source = m->source != NULL && m->source[0] != '\0' ?
			    m->source : "tmpfs";
		} else {
			/* pseudo-fs: source is conventionally the type name */
			source = m->source != NULL && m->source[0] != '\0' ?
			    m->source : fstype;
		}

		argv[argc++] = __DECONST(char *, "/sbin/mount");
		argv[argc++] = __DECONST(char *, "-t");
		argv[argc++] = __DECONST(char *, fstype);
		if (opts[0] != '\0') {
			argv[argc++] = __DECONST(char *, "-o");
			argv[argc++] = opts;
		}
		argv[argc++] = __DECONST(char *, source);
		argv[argc++] = dest;
		argv[argc] = NULL;

		if (run_mount_cmd(argv) != 0) {
			fprintf(stderr,
			    "warning: mount -t %s %s %s failed: %s\n",
			    fstype, source, dest, strerror(errno));
			continue;
		}
		if (record_applied_mount(c, dest) != 0) {
			/* best-effort unmount if we cannot track */
			char *uargv[3];

			uargv[0] = __DECONST(char *, "/sbin/umount");
			uargv[1] = dest;
			uargv[2] = NULL;
			(void)run_mount_cmd(uargv);
			errno = ENOMEM;
			return (-1);
		}
	}

	return (0);
}

/*
 * Unmount a single absolute path (best-effort).
 */
static void
umount_path(const char *target)
{
	char *argv[4];

	if (target == NULL || target[0] == '\0')
		return;
	argv[0] = __DECONST(char *, "/sbin/umount");
	argv[1] = __DECONST(char *, "-f");
	argv[2] = __DECONST(char *, target);
	argv[3] = NULL;
	if (run_mount_cmd(argv) != 0) {
		/*
		 * EINVAL/ENOENT mean nothing was mounted — ignore.
		 * Other errors: warn and continue.
		 */
		if (errno != EINVAL && errno != ENOENT)
			fprintf(stderr, "warning: umount %s failed: %s\n",
			    target, strerror(errno));
	}
}

/*
 * Unmount previously applied mounts in reverse order.
 *
 * applied_mounts is process-local (not in state.json). After a separate
 * CLI process reloads the container, rebuild targets from the OCI spec
 * so delete still tears mounts down.
 */
int
container_unmount_all(struct ocifbsd_container *c)
{
	int i;

	if (c == NULL)
		return (0);

	if (c->n_applied_mounts > 0 && c->applied_mounts != NULL) {
		for (i = c->n_applied_mounts - 1; i >= 0; i--) {
			umount_path(c->applied_mounts[i]);
			free(c->applied_mounts[i]);
			c->applied_mounts[i] = NULL;
		}
		free(c->applied_mounts);
		c->applied_mounts = NULL;
		c->n_applied_mounts = 0;
		return (0);
	}

	/* Derive from spec when this process did not apply the mounts. */
	if (c->spec == NULL && c->config_path != NULL)
		c->spec = oci_parse_config(c->config_path);
	if (c->spec != NULL && c->rootfs != NULL &&
	    c->spec->mounts != NULL && c->spec->n_mounts > 0) {
		for (i = c->spec->n_mounts - 1; i >= 0; i--) {
			char dest[PATH_MAX];
			struct oci_mount *m = &c->spec->mounts[i];
			const char *fstype;

			if (m->destination == NULL || m->destination[0] == '\0')
				continue;
			fstype = oci_mount_type_to_fbsd(m->type);
			if (fstype == NULL)
				continue;
			/*
			 * This spec was reloaded from config.json without going
			 * back through oci_validate_spec, so re-check it here: a
			 * '..' destination or a truncated join would otherwise
			 * force-unmount a host path (up to and including / or
			 * /usr) as root. Mirror the guards in the apply path.
			 */
			if (!oci_path_is_safe(m->destination))
				continue;
			if ((size_t)snprintf(dest, sizeof(dest),
			    m->destination[0] == '/' ? "%s%s" : "%s/%s",
			    c->rootfs, m->destination) >= sizeof(dest))
				continue;
			/* Never force-unmount a path that resolves outside root. */
			if (!mount_target_within_rootfs(c->rootfs, dest))
				continue;
			umount_path(dest);
		}
	}

	/*
	 * The default devfs at <rootfs>/dev is mounted at start but is not part
	 * of the spec, so a delete running in a fresh process (n_applied_mounts
	 * == 0) never sees it in the loops above. Tear it down explicitly so it
	 * does not leak and keep the image rootfs busy (blocking rmi).
	 */
	if (c->rootfs != NULL) {
		char devp[PATH_MAX];

		if ((size_t)snprintf(devp, sizeof(devp), "%s/dev", c->rootfs) <
		    sizeof(devp) && mount_target_within_rootfs(c->rootfs, devp))
			umount_path(devp);
	}

	return (0);
}

/*
 * Set up process environment inside container
 */
static int
setup_process_env(struct ocifbsd_container *c)
{
	extern char **environ;
	static const char defpath[] =
	    "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	struct oci_runtime_spec *spec = c->spec;
	char **src = (spec != NULL) ? spec->process.env : NULL;
	char **envp;
	int n = 0, i, k = 0;
	bool have_path = false;

	/*
	 * Replace the inherited environment ENTIRELY rather than overlaying the
	 * spec on top of it. The old putenv()-onto-environ approach leaked the
	 * ocifbsd invoker's whole environment (registry credentials, API tokens,
	 * proxy vars) into the container, readable by anything running inside it;
	 * OCI requires the process env to be exactly process.env. We run in the
	 * forked child just before exec, so building a fresh environ is safe.
	 * A default PATH is supplied when the spec omits one so that execvp of a
	 * bare program name still resolves.
	 */
	if (src != NULL) {
		for (i = 0; src[i] != NULL; i++) {
			n++;
			if (strncmp(src[i], "PATH=", 5) == 0)
				have_path = true;
		}
	}

	envp = calloc((size_t)n + 2, sizeof(char *));	/* +default PATH, +NULL */
	if (envp == NULL)
		return (-1);
	if (src != NULL) {
		for (i = 0; src[i] != NULL; i++)
			envp[k++] = src[i];
	}
	if (!have_path)
		envp[k++] = __DECONST(char *, defpath);
	envp[k] = NULL;

	environ = envp;
	return (0);
}

/*
 * Overlay a container's persisted network configuration (set via
 * `ocifbsd network set`) onto its spec. The file lives under
 * <DATA_DIR>/networks/<id>.json and is created 0640, root:ocifbsd; an
 * absent or unreadable file leaves the spec's own network settings in place.
 */
static void
container_overlay_netcfg(struct ocifbsd_container *c,
	struct oci_runtime_spec *spec)
{
	const char *ddir;
	char ncpath[PATH_MAX];
	char *ncjson;
	size_t nclen;

	ddir = getenv("OCIFBSD_DATA_DIR");
	if (ddir == NULL || ddir[0] == '\0')
		ddir = OCIFBSD_DATA_DIR;
	if ((size_t)snprintf(ncpath, sizeof(ncpath), "%s/networks/%s.json",
	    ddir, c->id) >= sizeof(ncpath))
		return;
	ncjson = read_file(ncpath, &nclen);
	if (ncjson == NULL)
		return;
	{
		struct netcfg nc;

		if (netcfg_parse(ncjson, &nc) == 0)
			netcfg_apply_to_spec(&nc, spec);
		netcfg_free(&nc);
	}
	free(ncjson);
}

/*
 * Build jail parameters from the (hostname-defaulted, netcfg-overlaid) spec
 * and create the persistent jail, setting c->jid. Shared by container_create
 * and container_reconfigure_network so both derive the jail the same way.
 * Returns 0 on success or -1 with errno set.
 */
static int
create_jail_from_spec(struct ocifbsd_container *c,
	struct oci_runtime_spec *spec)
{
	struct jailparam *params;
	size_t nparams, pi;
	char jname[64];

	oci_spec_default_hostname(spec, c->name);
	container_overlay_netcfg(c, spec);

	params = oci_spec_to_jail_params(spec, &nparams);
	if (params == NULL)
		return (-1);

	/*
	 * Give the jail a unique name (container id prefix). Replace the
	 * placeholder value in place: free the existing heap value first and
	 * re-import so we neither leak it nor store a pointer to the on-stack
	 * jname buffer. If the re-import fails we must not submit a jail with
	 * an empty/duplicate name, so fail closed.
	 */
	snprintf(jname, sizeof(jname), "ocifbsd-%.12s", c->id);
	for (pi = 0; pi < nparams; pi++) {
		if (params[pi].jp_name != NULL &&
		    strcmp(params[pi].jp_name, "name") == 0) {
			free(params[pi].jp_value);
			params[pi].jp_value = NULL;
			if (jailparam_import(&params[pi], jname) != 0) {
				jailparam_free(params, nparams);
				free(params);
				return (-1);
			}
			break;
		}
	}

	c->jid = jailparam_set(params, nparams, JAIL_CREATE);
	jailparam_free(params, nparams);
	free(params);		/* jailparam_free frees slots, not the array */
	if (c->jid < 0)
		return (-1);
	return (0);
}

/* Defined below; the rebuild path re-establishes the secure rootfs overlay. */
static int establish_secure_rootfs(struct ocifbsd_container *c,
    struct oci_runtime_spec *spec);

/*
 * Reapply a container's network configuration by rebuilding its jail from
 * the current spec + persisted netcfg. Only permitted while the container is
 * in the created (not yet started) state: its jail holds no processes, so
 * destroying and recreating it is safe. A running or paused container is
 * refused with EBUSY — the caller should report that a restart is required.
 * Returns 0 on success, -1 with errno set otherwise.
 */
int
container_reconfigure_network(struct ocifbsd_container *c)
{
	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (c->state != OCIFBSD_STATE_CREATED) {
		errno = EBUSY;
		return (-1);
	}
	if (c->spec == NULL) {
		if (c->config_path == NULL) {
			errno = EINVAL;
			return (-1);
		}
		c->spec = oci_parse_config(c->config_path);
		if (c->spec == NULL)
			return (-1);	/* keep oci_parse_config's errno */
	}
	/*
	 * Destroy the existing (process-less) jail before recreating it, but
	 * resolve it by name rather than trusting the stored jid: a jid can be
	 * reused by an unrelated jail after the original is gone, and removing
	 * it by number would destroy the wrong jail. jail_getid on the
	 * ocifbsd-<id> name returns our current jail if it still exists (and
	 * sidesteps jail_remove(2)'s EINVAL-for-missing-jail semantics). If the
	 * name does not resolve, the jail is already gone and there is nothing
	 * to remove; if it resolves but cannot be removed, abort with the old
	 * state intact.
	 */
	{
		char jname[64];
		int real;

		snprintf(jname, sizeof(jname), "ocifbsd-%.12s", c->id);
		real = jail_getid(jname);
		if (real >= 0 && jail_remove(real) != 0)
			return (-1);
	}
	c->jid = -1;
	/*
	 * Re-establish the read-only / nosuid nullfs overlay before rebuilding,
	 * exactly as container_create does. The freshly parsed spec->root.path
	 * points at the writable image rootfs, so without this a restart would
	 * silently rebuild a readonly/noNewPrivileges container as writable and
	 * apply mounts to the stale overlay. establish_secure_rootfs is a no-op
	 * for ordinary containers and idempotent for secured ones.
	 */
	if (establish_secure_rootfs(c, c->spec) != 0) {
		int saved = errno;

		c->state = OCIFBSD_STATE_STOPPED;
		state_save(c);
		errno = saved;
		return (-1);
	}
	if (create_jail_from_spec(c, c->spec) != 0) {
		/*
		 * The old jail is gone and the new one could not be built.
		 * Persist that the container no longer has a jail (jid -1,
		 * stopped) so a later start cannot attach a stale — possibly
		 * reused — jid. The caller reports the failure.
		 */
		int saved = errno;

		c->state = OCIFBSD_STATE_STOPPED;
		state_save(c);
		errno = saved;
		return (-1);
	}
	/*
	 * If we cannot persist the new jid, on-disk state would still name the
	 * removed jail; tear the fresh jail back down so in-memory and on-disk
	 * identity stay consistent, and fail.
	 */
	if (state_save(c) != 0) {
		int saved = errno;

		(void)jail_remove(c->jid);
		c->jid = -1;
		c->state = OCIFBSD_STATE_STOPPED;
		(void)state_save(c);
		errno = saved;
		return (-1);
	}
	return (0);
}

/*
 * Restart a container end to end: stop it if it is running or paused, then
 * rebuild its jail from the persisted spec + `ocifbsd network set` config and
 * start it again. Because the container keeps its id, the netcfg overlay
 * (stored as networks/<id>.json) is preserved across the restart — unlike a
 * delete+recreate, which mints a new id and loses it. Valid from any state
 * (running, paused, stopped, or created). Returns 0 on success, -1 with errno
 * set otherwise.
 */
int
container_restart(struct ocifbsd_container *c)
{
	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * Lift a pause first: a SIGSTOP'd init will not act on container_stop's
	 * SIGTERM until it is continued, and container_kill refuses non-running
	 * states outright, so restarting a paused container would otherwise fail
	 * without rebuilding. SIGCONT it and mark it running before stopping.
	 */
	if (c->state == OCIFBSD_STATE_PAUSED ||
	    c->state == OCIFBSD_STATE_PAUSED_HIGH) {
		if (c->init_pid > 0)
			(void)kill(c->init_pid, SIGCONT);
		c->state = OCIFBSD_STATE_RUNNING;
	}

	/* Quiesce a live container; container_stop leaves it stopped. */
	if (c->state == OCIFBSD_STATE_RUNNING) {
		if (container_stop(c, 10) != 0)
			return (-1);
	}

	/*
	 * Rebuild the (now process-less, and after a stop, torn-down) jail via
	 * container_reconfigure_network: it recreates the jail from the spec
	 * with the persisted netcfg overlaid, and tolerates an already-missing
	 * jail (jail_getid < 0 -> nothing to remove). It requires the created
	 * state, so mark the container created before delegating.
	 */
	c->state = OCIFBSD_STATE_CREATED;
	if (container_reconfigure_network(c) != 0)
		return (-1);

	return (container_start(c));
}

/*
 * Establish the container root read-only and/or nosuid when the bundle asks
 * for it (root.readonly / process.noNewPrivileges). FreeBSD nullfs cannot mount
 * a path over itself, so the flags are applied by nullfs-mounting the bundle
 * rootfs onto a distinct mountpoint and using that as the jail root.
 *
 * The mountpoint lives under the runtime state dir ($STATE_DIR/<id>.jailroot),
 * deliberately NOT under the bundle or image-store path — otherwise the live
 * nullfs mount would keep the image directory busy and block `rmi`. It is
 * deterministic from the container id, so delete (in a fresh process) can
 * reconstruct and unmount it. spec->root.path and c->rootfs are repointed at
 * the mountpoint; the original rootfs remains the nullfs source.
 *
 * No-op (returns 0) unless a flag is set, so ordinary containers are unchanged.
 * Returns -1 only if a requested mount could not be established.
 */
static int
establish_secure_rootfs(struct ocifbsd_container *c,
	struct oci_runtime_spec *spec)
{
	char jailroot[PATH_MAX];
	char opts[64];
	char *argv[8];
	int argc = 0;
	char *dup;
	char *dup2;

	if (spec == NULL || c == NULL || c->id == NULL)
		return (0);
	if (!spec->root.readonly && !spec->process.no_new_privileges)
		return (0);		/* open default: nothing to do */

	opts[0] = '\0';
	if (spec->root.readonly)
		strlcpy(opts, "ro", sizeof(opts));
	if (spec->process.no_new_privileges) {
		if (opts[0] != '\0')
			strlcat(opts, ",", sizeof(opts));
		strlcat(opts, "nosuid", sizeof(opts));
	}

	if ((size_t)snprintf(jailroot, sizeof(jailroot), "%s/%s.jailroot",
	    state_base_dir(), c->id) >= sizeof(jailroot))
		return (-1);
	if (mkdir(jailroot, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "error: cannot create jail root %s: %s\n",
		    jailroot, strerror(errno));
		return (-1);
	}
	/*
	 * Clear any overlay left by a previous create/restart so a rebuild does
	 * not stack a second nullfs mount on the same point. umount_path ignores
	 * the not-mounted case, so this is a harmless no-op on first create.
	 */
	umount_path(jailroot);

	/* mount -t nullfs -o <opts> <original rootfs> <jailroot> */
	argv[argc++] = __DECONST(char *, "/sbin/mount");
	argv[argc++] = __DECONST(char *, "-t");
	argv[argc++] = __DECONST(char *, "nullfs");
	argv[argc++] = __DECONST(char *, "-o");
	argv[argc++] = opts;
	argv[argc++] = spec->root.path;
	argv[argc++] = jailroot;
	argv[argc] = NULL;
	if (run_mount_cmd(argv) != 0) {
		fprintf(stderr, "error: cannot mount %s root %s -> %s: %s\n",
		    opts, spec->root.path, jailroot, strerror(errno));
		(void)rmdir(jailroot);
		return (-1);
	}

	/*
	 * Repoint both the jail root (spec->root.path) and the container rootfs
	 * (c->rootfs) at the flagged nullfs view. Allocate both copies up front:
	 * if either strdup fails, tear the mount back down and fail, rather than
	 * (a) leaking the live nullfs mount or (b) leaving spec->root.path and
	 * c->rootfs pointing at different directories — a divergence that would
	 * later let container_delete umount/rmdir the original image rootfs.
	 */
	dup = strdup(jailroot);
	dup2 = strdup(jailroot);
	if (dup == NULL || dup2 == NULL) {
		free(dup);
		free(dup2);
		umount_path(jailroot);
		(void)rmdir(jailroot);
		errno = ENOMEM;
		return (-1);
	}
	free(spec->root.path);
	spec->root.path = dup;
	free(c->rootfs);
	c->rootfs = dup2;
	return (0);
}

/*
 * Create a container from OCI bundle
 */
int
container_create(struct ocifbsd_container **cp, const char *bundle_path,
	const char *name)
{
	struct ocifbsd_container *c;
	struct oci_runtime_spec *spec;
	char config_path[PATH_MAX];
	char *canonical;

	if (cp == NULL || bundle_path == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/* Initialize state directory */
	if (state_init() != 0) {
		return (-1);
	}

	/* Find config.json in bundle */
	if ((size_t)snprintf(config_path, sizeof(config_path), "%s/config.json",
	    bundle_path) >= sizeof(config_path)) {
		/*
		 * A truncated path could name a different, existing config.json
		 * and jail the wrong root/process as root; refuse rather than
		 * proceed on a silently shortened path.
		 */
		errno = ENAMETOOLONG;
		return (-1);
	}

	/* Parse OCI config */
	spec = oci_parse_config(config_path);
	if (spec == NULL) {
		fprintf(stderr, "error: failed to parse OCI config: %s\n",
		    config_path);
		return (-1);
	}

	/*
	 * Resolve relative root.path against the bundle directory so
	 * jail(8) receives an absolute path (required).
	 */
	if (spec->root.path != NULL && spec->root.path[0] != '/') {
		char abspath[PATH_MAX];
		char *resolved;

		if ((size_t)snprintf(abspath, sizeof(abspath), "%s/%s",
		    bundle_path, spec->root.path) >= sizeof(abspath)) {
			/*
			 * A truncated join could resolve to a different existing
			 * directory and jail the wrong root as root; refuse it,
			 * mirroring the config.json length check above.
			 */
			oci_free_spec(spec);
			errno = ENAMETOOLONG;
			return (-1);
		}
		resolved = realpath(abspath, NULL);
		if (resolved == NULL) {
			/* keep joined path even if rootfs not yet fully present */
			resolved = strdup(abspath);
		}
		if (resolved == NULL) {
			oci_free_spec(spec);
			errno = ENOMEM;
			return (-1);
		}
		free(spec->root.path);
		spec->root.path = resolved;
	}

	/* Validate spec */
	if (oci_validate_spec(spec) != 0) {
		oci_free_spec(spec);
		return (-1);
	}

	/* Allocate container */
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		oci_free_spec(spec);
		errno = ENOMEM;
		return (-1);
	}

	c->spec = spec;
	c->bundle_path = strdup(bundle_path);
	c->config_path = strdup(config_path);

	/* Generate or use provided name */
	if (name != NULL) {
		canonical = canonical_name(name);
		if (canonical == NULL) {
			/* Use generated ID as name */
			c->name = NULL;
		} else {
			c->name = canonical;
		}
	}

	/* Generate container ID */
	c->id = generate_container_id();
	if (c->id == NULL) {
		container_free(c);
		errno = ENOMEM;
		return (-1);
	}

	/* Use ID as name if not provided */
	if (c->name == NULL) {
		c->name = strdup(c->id);
	}

	/* Set rootfs path */
	c->rootfs = strdup(spec->root.path);
	if (c->rootfs == NULL) {
		container_free(c);
		errno = ENOMEM;
		return (-1);
	}

	/*
	 * Per-container log path. The container's init process has its stdio
	 * redirected here (see container_start) so it does not inherit — and
	 * hold open — the CLI's terminal/pipe.
	 */
	{
		char logbuf[PATH_MAX];

		snprintf(logbuf, sizeof(logbuf), "%s/%s.log",
		    state_base_dir(), c->id);
		c->log_path = strdup(logbuf);
	}

	/*
	 * Apply an opt-in read-only / nosuid root before the jail is created,
	 * repointing the jail root at the flagged nullfs view. No-op unless the
	 * bundle requests it, so ordinary containers are unaffected.
	 */
	if (establish_secure_rootfs(c, spec) != 0) {
		container_free(c);
		return (-1);
	}

	/*
	 * Build the jail from the spec: default the hostname to the container
	 * name, overlay any persisted network configuration, and create the
	 * persistent jail. c->name is always set by this point (it falls back
	 * to the container id above).
	 */
	if (create_jail_from_spec(c, spec) != 0) {
		fprintf(stderr, "error: failed to create jail: %s\n",
		    strerror(errno));
		container_free(c);
		return (-1);
	}

	/* Container created but not started */
	c->state = OCIFBSD_STATE_CREATED;
	c->created_at = time(NULL);

	/* Register container */
	if (container_register(c) != 0) {
		/* Warning only */
		fprintf(stderr, "warning: failed to register container: %s\n",
		    strerror(errno));
	}

	/*
	 * Persist the container. If this fails the jail already exists but no
	 * state file references it, so a later CLI could neither start nor
	 * delete it (an unreapable jail plus any nullfs/devfs overlay). Roll the
	 * creation back — tear down the jail, mounts, and network — and fail,
	 * the same contract container_reconfigure_network already honours.
	 */
	if (state_save(c) != 0) {
		fprintf(stderr, "error: failed to persist container state: %s\n",
		    strerror(errno));
		if (c->jid > 0) {
			(void)jail_remove(c->jid);
			c->jid = 0;
		}
		(void)container_unmount_all(c);
		teardown_container_network(c);
		container_free(c);
		return (-1);
	}

	*cp = c;
	return (0);
}

/*
 * Start a created container
 */
int
container_start(struct ocifbsd_container *c)
{
	pid_t pid;
	int status;

	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (c->state != OCIFBSD_STATE_CREATED) {
		errno = EINVAL;
		fprintf(stderr, "error: container %s not in created state\n",
		    c->id);
		return (-1);
	}

	/*
	 * Guard against a stale or reused jid. The jail created for this
	 * container is named ocifbsd-<id>; if that name no longer resolves to
	 * our recorded jid (host reboot, external teardown, or a jid recycled
	 * by an unrelated jail), attaching c->jid could enter the wrong jail as
	 * root. Verify the identity before attaching and refuse otherwise.
	 */
	{
		char jname[64];
		int real;

		snprintf(jname, sizeof(jname), "ocifbsd-%.12s", c->id);
		real = jail_getid(jname);
		if (real < 0 || real != c->jid) {
			errno = ESRCH;
			fprintf(stderr, "error: container %s jail is missing or "
			    "was replaced; recreate it\n", c->id);
			return (-1);
		}
	}

	/* Ensure OCI spec is available (reloaded by state_load when possible) */
	if (c->spec == NULL && c->config_path != NULL)
		c->spec = oci_parse_config(c->config_path);
	if (c->spec == NULL || c->spec->process.args == NULL ||
	    c->spec->process.args[0] == NULL) {
		errno = EINVAL;
		fprintf(stderr,
		    "error: container %s has no process args (missing OCI config)\n",
		    c->id);
		return (-1);
	}

	/*
	 * Overlay the persisted `ocifbsd network set` configuration onto the
	 * freshly-parsed image spec. Without this, a spec reloaded from the
	 * image config.json carries none of the container's vnet/ip4/gateway
	 * settings, so setup_container_network() below would skip epair wiring
	 * and a service (e.g. redis binding a socket) fails with EPROTONOSUPPORT
	 * in a jail that has no configured interface. Mirrors what
	 * create_jail_from_spec() does at create time.
	 */
	container_overlay_netcfg(c, c->spec);

	/*
	 * Make any requested-but-unenforced security restriction visible, so an
	 * operator is never misled into thinking the container is hardened.
	 */
	warn_unenforced_security(c->spec);

	/*
	 * Apply RCTL resource limits declared in the bundle (opt-in; no-op when
	 * none are declared or RACCT is disabled). Done before the init process
	 * attaches so the jail is bounded from its first process.
	 */
	apply_resource_limits(c);

	/*
	 * Establish VNET connectivity (epair into the jail + address) for
	 * bundles that request it, before the init process attaches.
	 */
	setup_container_network(c);

	/*
	 * Host-side mounts into rootfs before the init process attaches.
	 * Failures of individual mounts are warned; only ENOMEM from
	 * tracking aborts start.
	 */
	if (container_apply_mounts(c) != 0 && errno == ENOMEM) {
		fprintf(stderr, "error: failed to track mounts for %s\n",
		    c->id);
		return (-1);
	}

	/* Run prestart hooks */
	hooks_run_prestart(c);

	/* Fork to start container init process */
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "error: fork failed: %s\n", strerror(errno));
		return (-1);
	}

	if (pid == 0) {
		/* Child process - become the container init */

		/*
		 * Detach from the controlling terminal and redirect stdio so the
		 * container's init does not inherit — and keep open — the CLI's
		 * stdin/stdout/stderr. Without this, a foreground `run`/`start`
		 * blocks the caller until the container exits (the container holds
		 * the caller's pipe). stdin comes from /dev/null; stdout/stderr go
		 * to the per-container log. Done before jail_attach so the log path
		 * on the host filesystem is still reachable.
		 */
		setsid();
		{
			int lfd, nfd;

			nfd = open(_PATH_DEVNULL, O_RDONLY);
			if (nfd >= 0) {
				dup2(nfd, STDIN_FILENO);
				if (nfd > STDERR_FILENO)
					close(nfd);
			}
			lfd = -1;
			if (c->log_path != NULL)
				lfd = open(c->log_path,
				    O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (lfd < 0)
				lfd = open(_PATH_DEVNULL, O_WRONLY);
			if (lfd >= 0) {
				dup2(lfd, STDOUT_FILENO);
				dup2(lfd, STDERR_FILENO);
				if (lfd > STDERR_FILENO)
					close(lfd);
			}
		}

		/* Attach to jail */
		if (jail_attach(c->jid) != 0) {
			fprintf(stderr, "error: failed to attach to jail: %s\n",
			    strerror(errno));
			_exit(126);
		}

		/*
		 * Apply the requested MAC label to this (the container init)
		 * process, inside the jail. Best-effort: a labeling policy must
		 * be loaded for it to take effect.
		 */
		if (c->spec->freebsd != NULL)
			(void)apply_process_mac_label(c->spec->freebsd->mac_label);

		/* Set working directory */
		if (c->spec->process.cwd != NULL) {
			if (chdir(c->spec->process.cwd) != 0) {
				fprintf(stderr, "error: failed to change directory: %s\n",
				    strerror(errno));
			}
		}

		/* Set up environment */
		setup_process_env(c);

		/*
		 * Apply OCI process.rlimits via setrlimit(2). Jail-level
		 * RCTL (freebsd.rctl_rules) is still Phase 4 work.
		 */
		if (c->spec->process.rlimits != NULL) {
			int ri;

			for (ri = 0; ri < c->spec->process.n_rlimits; ri++) {
				struct oci_rlimit *rl =
				    &c->spec->process.rlimits[ri];
				struct rlimit rlp;
				int resource = -1;

				if (rl->type == NULL)
					continue;
				if (strcmp(rl->type, "RLIMIT_CPU") == 0)
					resource = RLIMIT_CPU;
				else if (strcmp(rl->type, "RLIMIT_FSIZE") == 0)
					resource = RLIMIT_FSIZE;
				else if (strcmp(rl->type, "RLIMIT_DATA") == 0)
					resource = RLIMIT_DATA;
				else if (strcmp(rl->type, "RLIMIT_STACK") == 0)
					resource = RLIMIT_STACK;
				else if (strcmp(rl->type, "RLIMIT_CORE") == 0)
					resource = RLIMIT_CORE;
				else if (strcmp(rl->type, "RLIMIT_RSS") == 0)
					resource = RLIMIT_RSS;
				else if (strcmp(rl->type, "RLIMIT_MEMLOCK") == 0)
					resource = RLIMIT_MEMLOCK;
				else if (strcmp(rl->type, "RLIMIT_NPROC") == 0)
					resource = RLIMIT_NPROC;
				else if (strcmp(rl->type, "RLIMIT_NOFILE") == 0)
					resource = RLIMIT_NOFILE;
				else if (strcmp(rl->type, "RLIMIT_AS") == 0 ||
				    strcmp(rl->type, "RLIMIT_VMEM") == 0)
					resource = RLIMIT_AS;
				if (resource < 0)
					continue;
				rlp.rlim_cur = rl->soft;
				rlp.rlim_max = rl->hard;
				if (setrlimit(resource, &rlp) != 0) {
					fprintf(stderr,
					    "warning: setrlimit %s failed: %s\n",
					    rl->type, strerror(errno));
				}
			}
		}

		/*
		 * Drop to the configured process.user gid/uid. Whenever we drop
		 * privilege at all — a non-root uid OR a non-root gid — clear
		 * root's supplementary groups and set the primary group before
		 * dropping the uid, in that order (setgid(2)/setgroups(2) need
		 * privilege). gid 0 is treated as an explicit "wheel primary",
		 * not "unset": a spec of {uid:1000} (gid defaulting to 0) must
		 * still have wheel/operator cleared from the supplementary set,
		 * or the init would keep them after setuid — a privilege hole
		 * against group-readable host paths. All three calls fail closed.
		 */
		if (c->spec->process.uid != 0 || c->spec->process.gid != 0) {
			gid_t gid = c->spec->process.gid;

			if (setgroups(1, &gid) != 0) {
				fprintf(stderr, "error: setgroups failed: %s\n",
				    strerror(errno));
				_exit(126);
			}
			if (setgid(gid) != 0) {
				fprintf(stderr, "error: setgid(%u) failed: %s\n",
				    (unsigned)gid, strerror(errno));
				_exit(126);
			}
			if (setuid(c->spec->process.uid) != 0) {
				fprintf(stderr, "error: setuid(%u) failed: %s\n",
				    (unsigned)c->spec->process.uid,
				    strerror(errno));
				_exit(126);
			}
		}

		/* Set process title */
		setproctitle("ocifbsd: %s [%s]",
		    c->name ? c->name : "(unnamed)",
		    c->id ? c->id : "(no-id)");

		/* Execute the container command */
		if (c->spec->process.args && c->spec->process.args[0]) {
			execvp(c->spec->process.args[0], c->spec->process.args);
		} else {
			/* Default: run sh */
			char sh[] = "/bin/sh";
			char *sh_args[] = { sh, NULL };
			execvp("/bin/sh", sh_args);
		}

		/* If execvp fails */
		fprintf(stderr, "error: failed to exec: %s\n", strerror(errno));
		_exit(127);
	}

	/* Parent process */
	c->init_pid = pid;
	c->state = OCIFBSD_STATE_RUNNING;
	c->started_at = time(NULL);

	/* Wait briefly to check if process starts */
	{
		struct timespec ts = { 0, 100 * 1000 * 1000 }; /* 100ms */
		nanosleep(&ts, NULL);
	}
	/*
	 * Only a return of exactly `pid` means the init was reaped and `status`
	 * is valid. A return of 0 means it is still running; a return of -1
	 * (EINTR/ECHILD) leaves `status` uninitialized — do not read it and do
	 * not tear down a jail whose init may well still be executing.
	 */
	if (waitpid(pid, &status, WNOHANG) == pid &&
	    (WIFEXITED(status) || WIFSIGNALED(status))) {
		/*
		 * The init process exited within the startup window. Drop the
		 * host-side mounts and remove the jail so a failed start does
		 * not leak a jail and nullfs/devfs mounts on every attempt
		 * (they previously accumulated until a manual delete).
		 */
		if (WIFEXITED(status)) {
			c->exit_code = WEXITSTATUS(status);
		} else {
			c->exit_code = 128 + WTERMSIG(status);
			fprintf(stderr,
			    "error: container init exited on signal %d\n",
			    WTERMSIG(status));
		}
		c->state = OCIFBSD_STATE_STOPPED;
		c->finished_at = time(NULL);
		c->init_pid = 0;
		(void)container_unmount_all(c);
		if (c->jid > 0) {
			(void)jail_remove(c->jid);
			c->jid = 0;
		}
		state_save(c);

		/*
		 * Distinguish a genuine start failure from a container that
		 * simply ran and exited quickly. The start child uses exit code
		 * 126 (jail_attach failed) and 127 (execvp failed) to signal that
		 * the process never actually started; report those as an error
		 * (with a meaningful errno). Any other exit — including 0 — means
		 * the container started and exited on its own, which is a normal
		 * outcome (like a short-lived foreground run), so return
		 * success and let the recorded exit_code/STOPPED state stand.
		 */
		if (WIFEXITED(status) &&
		    (WEXITSTATUS(status) == 126 || WEXITSTATUS(status) == 127)) {
			errno = (WEXITSTATUS(status) == 127) ? ENOENT : EACCES;
			return (-1);
		}
		return (0);
	}

	/* Update state */
	state_save(c);

	/* Run poststart hooks */
	hooks_run_poststart(c);

	return (0);
}

/*
 * Send signal to container init process.
 *
 * Idempotent: already-stopped/created containers, missing init pid, or
 * kill(2) returning ESRCH (dead init) succeed so delete --force is simple.
 */
int
container_kill(struct ocifbsd_container *c, int sig)
{
	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (c->state != OCIFBSD_STATE_RUNNING) {
		if (c->state == OCIFBSD_STATE_STOPPED ||
		    c->state == OCIFBSD_STATE_CREATED)
			return (0);
		errno = EINVAL;
		return (-1);
	}

	if (c->init_pid <= 0) {
		c->state = OCIFBSD_STATE_STOPPED;
		c->finished_at = time(NULL);
		state_save(c);
		return (0);
	}

	/*
	 * The stored init_pid is persisted across CLI invocations, and after
	 * our first process exits the container init is reparented to init(8).
	 * If it has since exited, its PID may have been recycled by an
	 * unrelated process; signaling that would be a serious (root) bug.
	 * Verify the PID still belongs to this container's jail first.
	 */
	if (!pid_in_jail(c->init_pid, c->jid)) {
		c->state = OCIFBSD_STATE_STOPPED;
		c->finished_at = time(NULL);
		c->init_pid = 0;
		state_save(c);
		return (0);
	}

	if (kill(c->init_pid, sig) != 0) {
		if (errno == ESRCH) {
			c->state = OCIFBSD_STATE_STOPPED;
			c->finished_at = time(NULL);
			c->init_pid = 0;
			state_save(c);
			return (0);
		}
		return (-1);
	}

	return (0);
}

/*
 * Execute a command inside a running container (jail_attach + execvp).
 * Returns the command's exit code (0-255, 128+sig on signal death),
 * or -1 with errno set if the exec could not be arranged.
 */
int
container_exec(struct ocifbsd_container *c, char **args, const char *cwd)
{
	pid_t pid;
	int status;

	if (c == NULL || args == NULL || args[0] == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (c->state != OCIFBSD_STATE_RUNNING || c->jid <= 0 || c->id == NULL) {
		errno = EINVAL;
		fprintf(stderr, "error: container %s is not running\n",
		    c->id ? c->id : "(no-id)");
		return (-1);
	}

	/*
	 * Guard against jid reuse before attaching: the state may still say
	 * RUNNING with jid N after the original jail was removed and an
	 * unrelated jail reused N, in which case jail_attach would exec the
	 * caller's command as root in the wrong jail. Require the ocifbsd-<id>
	 * name to still resolve to our recorded jid, as container_start does.
	 */
	{
		char jname[64];

		snprintf(jname, sizeof(jname), "ocifbsd-%.12s", c->id);
		if (jail_getid(jname) != c->jid) {
			errno = ESRCH;
			fprintf(stderr, "error: container %s jail is gone or was "
			    "recycled; refusing to attach\n", c->id);
			return (-1);
		}
	}

	/* Reload the spec so the exec inherits the container environment. */
	if (c->spec == NULL && c->config_path != NULL)
		c->spec = oci_parse_config(c->config_path);

	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "error: fork failed: %s\n", strerror(errno));
		return (-1);
	}

	if (pid == 0) {
		if (jail_attach(c->jid) != 0) {
			fprintf(stderr, "error: failed to attach to jail: %s\n",
			    strerror(errno));
			_exit(126);
		}

		if (cwd == NULL && c->spec != NULL)
			cwd = c->spec->process.cwd;
		if (chdir(cwd != NULL ? cwd : "/") != 0) {
			fprintf(stderr, "error: failed to change directory: %s\n",
			    strerror(errno));
		}

		if (c->spec != NULL)
			setup_process_env(c);

		execvp(args[0], args);
		fprintf(stderr, "error: failed to exec %s: %s\n", args[0],
		    strerror(errno));
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0)
		return (-1);
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	if (WIFSIGNALED(status))
		return (128 + WTERMSIG(status));
	return (-1);
}

/*
 * Gracefully stop a container: SIGTERM the init process, poll for exit
 * up to timeout_sec seconds, then SIGKILL. The caller is usually not
 * the parent of init, so exit is detected via kill(pid, 0) rather than
 * waitpid. Idempotent for already-stopped containers.
 */
int
container_stop(struct ocifbsd_container *c, int timeout_sec)
{
	struct timespec tick = { 0, 100 * 1000 * 1000 }; /* 100ms */
	int waited_ms, timeout_ms;

	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (c->state != OCIFBSD_STATE_RUNNING)
		return (container_kill(c, SIGTERM));

	if (c->init_pid <= 0) {
		c->state = OCIFBSD_STATE_STOPPED;
		c->finished_at = time(NULL);
		state_save(c);
		return (0);
	}

	/*
	 * Gate every signal on jail membership rather than kill(pid, 0): once
	 * init exits, its PID can be recycled by an unrelated host process, and
	 * a bare kill(pid, 0)/kill(pid, SIGKILL) would then report and then
	 * kill that stranger as root. If the PID is already gone from the jail,
	 * the container has stopped and there is nothing to signal.
	 */
	if (!pid_in_jail(c->init_pid, c->jid)) {
		c->state = OCIFBSD_STATE_STOPPED;
		c->finished_at = time(NULL);
		c->init_pid = 0;
		state_save(c);
		return (0);
	}

	if (kill(c->init_pid, SIGTERM) != 0 && errno != ESRCH)
		return (-1);

	timeout_ms = (timeout_sec > 0 ? timeout_sec : 10) * 1000;
	for (waited_ms = 0; waited_ms < timeout_ms; waited_ms += 100) {
		if (!pid_in_jail(c->init_pid, c->jid))
			break;
		/* Reap if we happen to be the parent (run/stop same process) */
		(void)waitpid(c->init_pid, NULL, WNOHANG);
		nanosleep(&tick, NULL);
	}

	if (pid_in_jail(c->init_pid, c->jid)) {
		(void)kill(c->init_pid, SIGKILL);
		(void)waitpid(c->init_pid, NULL, WNOHANG);
	}

	c->state = OCIFBSD_STATE_STOPPED;
	c->finished_at = time(NULL);
	c->init_pid = 0;
	state_save(c);

	return (0);
}

/*
 * Delete a container
 */
int
container_delete(struct ocifbsd_container *c)
{
	int ret;

	bool was_started;

	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * poststop hooks run only for containers that actually reached the
	 * running state, and only after the process is stopped and mounts
	 * are dropped — per the OCI runtime lifecycle. Running them first
	 * (as the previous code did) exposed hooks to a still-live process
	 * and still-mounted filesystems, and fired them for never-started
	 * containers.
	 */
	was_started = (c->started_at != 0);

	/* Stop container if running or paused */
	if ((c->state == OCIFBSD_STATE_RUNNING ||
	    c->state == OCIFBSD_STATE_PAUSED ||
	    c->state == OCIFBSD_STATE_PAUSED_HIGH) && c->init_pid > 0) {
		/*
		 * Only signal the stored PID if it still belongs to this jail: a
		 * recycled PID must not be SIGKILL'd as root. jail_remove below
		 * tears down whatever is actually in the jail regardless, so this
		 * signal is just a fast path, not the real teardown.
		 */
		if (pid_in_jail(c->init_pid, c->jid)) {
			kill(c->init_pid, SIGKILL);
			(void)waitpid(c->init_pid, NULL, WNOHANG);
		}
	}

	/* Drop host-side mounts before removing the jail */
	(void)container_unmount_all(c);

	/* Run poststop hooks (after the process is gone and unmounted) */
	if (was_started)
		hooks_run_poststop(c);

	/*
	 * Remove the jail, resolved by its ocifbsd-<id> name rather than the
	 * stored jid: a jid can be recycled by an unrelated jail once ours is
	 * gone, and removing it by number would destroy the wrong jail.
	 */
	if (c->id != NULL) {
		char jname[64];
		int real;

		snprintf(jname, sizeof(jname), "ocifbsd-%.12s", c->id);
		real = jail_getid(jname);
		if (real >= 0) {
			ret = jail_remove(real);
			if (ret != 0)
				fprintf(stderr,
				    "warning: failed to remove jail: %s\n",
				    strerror(errno));
		}
		c->jid = 0;
	}

	/*
	 * Remove any RCTL rules for this jail subject so limits do not leak
	 * after the jail is gone (rctl(8) rules on a jail: subject otherwise
	 * persist). Best-effort and a no-op when RACCT is unavailable.
	 */
	if (rctl_check_available()) {
		char jname[MAXHOSTNAMELEN];

		container_jail_name(c, jname, sizeof(jname));
		(void)rctl_remove_rules(jname);
	}

	/*
	 * Destroy the host-side epair for a VNET container. The jail removal
	 * above reclaimed the in-jail peer, but the host side survives and would
	 * otherwise leak (eventually exhausting interface slots).
	 */
	teardown_container_network(c);

	/*
	 * Tear down the read-only/nosuid root overlay, if any, now that the jail
	 * that held it busy is gone. c->rootfs is the $STATE_DIR/<id>.jailroot
	 * nullfs mountpoint; the bundle's real rootfs (the nullfs source) is left
	 * intact. Reloaded specs carry root.readonly / noNewPrivileges, so this
	 * fires on a later `delete` from a fresh process too.
	 */
	if (c->spec != NULL && c->rootfs != NULL &&
	    (c->spec->root.readonly || c->spec->process.no_new_privileges)) {
		umount_path(c->rootfs);
		(void)rmdir(c->rootfs);
	}

	/* Unregister from memory */
	container_unregister(c->id);

	/* Delete state file and the container log (else logs accumulate in
	 * the state dir after the container is gone). Derive the log path from
	 * the id — it matches container_create and works even when the
	 * container was reloaded from state (which does not persist log_path). */
	state_delete(c->id);
	if (c->id != NULL) {
		char logbuf[PATH_MAX];

		snprintf(logbuf, sizeof(logbuf), "%s/%s.log",
		    state_base_dir(), c->id);
		(void)unlink(logbuf);
	}

	/* Update state */
	c->state = OCIFBSD_STATE_STOPPED;
	c->finished_at = time(NULL);

	return (0);
}

/*
 * Pause a container
 */
int
container_pause(struct ocifbsd_container *c)
{
	if (c == NULL || c->state != OCIFBSD_STATE_RUNNING) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * Stop the container init. Full process-tree walk (all PIDs in
	 * the jail) is a later hardening step; SIGSTOP on init freezes
	 * the common single-process case and most simple trees.
	 */
	if (c->init_pid <= 0) {
		errno = ESRCH;
		return (-1);
	}
	/*
	 * Verify the stored PID still belongs to this jail before signaling.
	 * After our first process exits, init is reparented and its PID can be
	 * recycled by an unrelated host process; SIGSTOP'ing that as root would
	 * freeze the wrong process. Treat a recycled/gone PID as exited.
	 */
	if (!pid_in_jail(c->init_pid, c->jid)) {
		c->state = OCIFBSD_STATE_STOPPED;
		c->finished_at = time(NULL);
		c->init_pid = 0;
		state_save(c);
		errno = ESRCH;
		return (-1);
	}
	if (kill(c->init_pid, SIGSTOP) != 0)
		return (-1);

	c->state = OCIFBSD_STATE_PAUSED;
	state_save(c);

	return (0);
}

/*
 * Resume a paused container
 */
int
container_resume(struct ocifbsd_container *c)
{
	if (c == NULL || c->state != OCIFBSD_STATE_PAUSED) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * As in container_pause, only continue the PID if it still belongs to
	 * this jail; a recycled PID must not receive SIGCONT. A PID that is gone
	 * means the container died while paused — reconcile to stopped.
	 */
	if (c->init_pid > 0) {
		if (!pid_in_jail(c->init_pid, c->jid)) {
			c->state = OCIFBSD_STATE_STOPPED;
			c->finished_at = time(NULL);
			c->init_pid = 0;
			state_save(c);
			errno = ESRCH;
			return (-1);
		}
		if (kill(c->init_pid, SIGCONT) != 0 && errno != ESRCH)
			return (-1);
	}

	c->state = OCIFBSD_STATE_RUNNING;
	state_save(c);

	return (0);
}

/*
 * Wait for container to exit
 */
int
container_wait(struct ocifbsd_container *c)
{
	int status;

	if (c == NULL) {
		errno = EINVAL;
		return (-1);
	}

	if (c->state != OCIFBSD_STATE_RUNNING && c->state != OCIFBSD_STATE_STOPPED) {
		errno = EINVAL;
		return (-1);
	}

	if (c->init_pid <= 0) {
		errno = ESRCH;
		return (-1);
	}

	/*
	 * Wait for init to exit. If this process is init's parent (started it in
	 * this same invocation), waitpid reaps it and yields the real exit code.
	 * But a container started by an earlier CLI process has had its init
	 * reparented to init(8): waitpid then fails with ECHILD, and the old code
	 * silently returned a stale exit_code while leaving the state RUNNING.
	 * Fall back to polling jail membership (as container_stop does) so we
	 * actually block until init is gone, then record the stopped state.
	 */
	if (waitpid(c->init_pid, &status, 0) > 0) {
		if (WIFEXITED(status)) {
			c->exit_code = WEXITSTATUS(status);
		} else if (WIFSIGNALED(status)) {
			c->exit_code = 128 + WTERMSIG(status);
		}
	} else if (errno == ECHILD) {
		struct timespec ts = { 0, 200 * 1000 * 1000 }; /* 200ms */

		while (pid_in_jail(c->init_pid, c->jid))
			nanosleep(&ts, NULL);
		/* exit code is unobservable for a non-child; leave it as recorded */
	} else {
		return (-1);
	}

	c->state = OCIFBSD_STATE_STOPPED;
	c->finished_at = time(NULL);
	c->init_pid = 0;
	state_save(c);

	return (c->exit_code);
}

/*
 * Generate container state as JSON for inspect
 */
int
container_inspect(struct ocifbsd_container *c, char **json_out)
{
	char *json;
	int len;
	/*
	 * bundle_path/rootfs/config_path are filesystem paths, which on FreeBSD
	 * may contain '"' and other bytes that would break the JSON string.
	 * Escape them once, up front, so the size pass and the fill pass below
	 * see identical strings (id/name are validated to a safe charset).
	 */
	char ebundle[PATH_MAX * 6], erootfs[PATH_MAX * 6], econfig[PATH_MAX * 6];

	if (c == NULL || json_out == NULL) {
		errno = EINVAL;
		return (-1);
	}

	ocifbsd_json_escape(c->bundle_path ? c->bundle_path : "", ebundle,
	    sizeof(ebundle));
	ocifbsd_json_escape(c->rootfs ? c->rootfs : "", erootfs,
	    sizeof(erootfs));
	ocifbsd_json_escape(c->config_path ? c->config_path : "", econfig,
	    sizeof(econfig));

	/* asprintf() is hidden by -D_XOPEN_SOURCE=700. Use a two-pass
	 * snprintf(NULL, 0) to size, malloc, then snprintf again to fill.
	 * Verbose but avoids the feature-test-macro dance. */
	len = snprintf(NULL, 0,
	    "{"
	    "\"id\": \"%s\","
	    "\"name\": \"%s\","
	    "\"state\": \"%s\","
	    "\"created\": %ld,"
	    "\"started\": %ld,"
	    "\"finished\": %ld,"
	    "\"exit_code\": %d,"
	    "\"bundle\": \"%s\","
	    "\"rootfs\": \"%s\","
	    "\"config\": \"%s\""
	    "}",
	    c->id ? c->id : "",
	    c->name ? c->name : "",
	    ocifbsd_state_to_string(c->state),
	    (long)c->created_at,
	    (long)c->started_at,
	    (long)c->finished_at,
	    c->exit_code,
	    ebundle,
	    erootfs,
	    econfig);
	if (len < 0)
		return (-1);
	json = malloc(len + 1);
	if (json == NULL) {
		errno = ENOMEM;
		return (-1);
	}
	snprintf(json, len + 1,
	    "{"
	    "\"id\": \"%s\","
	    "\"name\": \"%s\","
	    "\"state\": \"%s\","
	    "\"created\": %ld,"
	    "\"started\": %ld,"
	    "\"finished\": %ld,"
	    "\"exit_code\": %d,"
	    "\"bundle\": \"%s\","
	    "\"rootfs\": \"%s\","
	    "\"config\": \"%s\""
	    "}",
	    c->id ? c->id : "",
	    c->name ? c->name : "",
	    ocifbsd_state_to_string(c->state),
	    (long)c->created_at,
	    (long)c->started_at,
	    (long)c->finished_at,
	    c->exit_code,
	    ebundle,
	    erootfs,
	    econfig);

	if (len < 0) {
		errno = ENOMEM;
		return (-1);
	}

	*json_out = json;
	return (0);
}
