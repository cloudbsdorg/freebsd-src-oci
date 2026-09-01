/*-
 * Copyright (c) 2026 REVYTECH, Inc.
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
 */

#ifndef _OCIFBSD_H
#define _OCIFBSD_H

#include <sys/types.h>
#include <sys/resource.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Version information */
#define OCIFBSD_VERSION		"0.1.0"
#define OCIFBSD_NAME		"FreeBSD OCI Runtime"
#define OCIFBSD_STATE_DIR	"/var/run/ocifbsd"
#define OCIFBSD_DATA_DIR	"/var/lib/ocifbsd"
#define OCIFBSD_CONFIG_DIR	"/etc/ocifbsd"

/*
 * Administrative group. Members of this group (in addition to root) may
 * view and modify container/runtime state via ocifbsd. If the group does
 * not exist on the host, access is restricted to root only.
 */
#define OCIFBSD_ADMIN_GROUP	"ocifbsd"

/* Directory/file modes for runtime state (root:ocifbsd, no world access). */
#define OCIFBSD_STATE_DIR_MODE	0750
#define OCIFBSD_STATE_FILE_MODE	0640

/* Container ID length (SHA-256 hex = 64 chars + null) */
#define OCIFBSD_MAX_CONTAINER_ID_LENGTH	65

/* Container states - aligned with OCI Runtime Spec */
typedef enum {
	OCIFBSD_STATE_UNKNOWN = 0,
	OCIFBSD_STATE_CREATED,
	OCIFBSD_STATE_RUNNING,
	OCIFBSD_STATE_STOPPED,
	OCIFBSD_STATE_PAUSED,
	OCIFBSD_STATE_PAUSED_HIGH,
} ocifbsd_state_t;

/* Forward declaration for the OCI runtime spec (defined below). */
struct oci_runtime_spec;

/* Container structure */
struct ocifbsd_container {
	char			*id;		/* Container ID (UUID) */
	char			*name;		/* Human-readable name */
	char			*rootfs;	/* Root filesystem path */
	char			*bundle_path;	/* OCI bundle directory */
	struct oci_runtime_spec	*spec;		/* Parsed OCI runtime spec */
	int			jid;		/* Jail ID */
	pid_t			init_pid;	/* Init process PID */
	ocifbsd_state_t		state;		/* Current state */
	time_t			created_at;	/* Creation timestamp */
	time_t			started_at;	/* Start timestamp */
	time_t			finished_at;	/* Finish timestamp */
	int			exit_code;	/* Exit code */
	char			*config_path;	/* Path to config.json */
	char			*log_path;	/* Path to container log */
	char			**applied_mounts; /* Absolute mount targets we mounted */
	int			n_applied_mounts;
};

/* OCI Runtime Specification structures */
struct oci_root {
	char	*path;		/* Root filesystem path */
	bool	readonly;	/* Readonly rootfs */
};

struct oci_rlimit {
	char	*type;		/* e.g. RLIMIT_NOFILE */
	rlim_t	hard;
	rlim_t	soft;
};

struct oci_process {
	char	**args;		/* Command arguments */
	char	**env;		/* Environment variables */
	char	*cwd;		/* Working directory */
	char	*console_size;	/* Console size (W:H) */
	int	tty;		/* Allocate TTY */
	int	terminal;	/* Terminal mode */
	uid_t	uid;		/* User ID */
	gid_t	gid;		/* Group ID */
	int	nice;		/* Nice value */
	char	**capabilities;	/* Linux capabilities (ignored) */
	char	*rlimits_no_reset; /* Rlimits not to reset */
	struct oci_rlimit *rlimits;
	int	n_rlimits;
	/*
	 * process.noNewPrivileges (OCI). Default false = open/permissive: the
	 * container may gain privileges via set-user-ID binaries as usual. When
	 * true, ocifbsd mounts the container root nosuid so set-user-ID and
	 * set-group-ID bits cannot raise privilege. Restriction is opt-in.
	 */
	bool	no_new_privileges;
};

struct oci_mount {
	char	*source;	/* Source path */
	char	*destination;	/* Destination path */
	char	*type;		/* Filesystem type */
	char	*options;	/* Mount options */
	bool	readonly;	/* Read-only mount */
};

struct oci_hook {
	char	*path;		/* Path to hook */
	char	**args;		/* Hook arguments */
	char	**env;		/* Environment variables */
	char	*timeout;	/* Timeout in seconds */
};

struct oci_hooks {
	struct oci_hook	**prestart;
	struct oci_hook	**poststart;
	struct oci_hook	**poststop;
	int		n_prestart;
	int		n_poststart;
	int		n_poststop;
};

struct oci_freebsd {
	bool	vnet;			/* Enable VNET (network isolation) */
	char	**ip4;			/* IPv4 addresses */
	int	n_ip4;			/* Count of IPv4 addresses */
	char	**ip6;			/* IPv6 addresses */
	int	n_ip6;			/* Count of IPv6 addresses */
	char	*hostname;		/* Hostname */
	char	*domainname;		/* Domainname */
	char	**dns;			/* DNS nameservers */
	int	n_dns;			/* Count of DNS nameservers */
	char	**default_gateway4;	/* Default IPv4 gateway */
	int	n_default_gateway4;	/* Count of default IPv4 gateways */
	char	**default_gateway6;	/* Default IPv6 gateway */
	int	n_default_gateway6;	/* Count of default IPv6 gateways */
	char	*mac_label;		/* MAC label */
	char	**rctl_rules;		/* RCTL rules */
	int	n_rctl_rules;		/* Count of RCTL rules */
	int	vnet_memory_limit;	/* VNET memory limit */
	int	allow_raw_sockets;	/* Allow raw sockets */
	int	allow_socket_af;	/* Allow socket address families */
	char	**securelevel;		/* Securelevel */
	int	n_securelevel;		/* Count of securelevel entries */
};

struct oci_runtime_spec {
	struct oci_root		root;
	struct oci_process	process;
	struct oci_mount	*mounts;
	int			n_mounts;
	struct oci_hooks	*hooks;
	struct oci_freebsd	*freebsd;
	char			*hostname;
	char			*domainname;
	char			**additional_gids;
	char			*oom_score_adj;
	/*
	 * linux.readonlyPaths / linux.maskedPaths (OCI). Both default empty =
	 * open/permissive: no path is forced read-only or masked. When set,
	 * ocifbsd remounts each readonly path read-only (nullfs -o ro over
	 * itself) and masks each masked path with an empty read-only overlay,
	 * inside the container root. Restriction is opt-in.
	 */
	char			**readonly_paths;
	int			n_readonly_paths;
	char			**masked_paths;
	int			n_masked_paths;
};

/* Container lifecycle operations */
int	container_create(struct ocifbsd_container **cp,
		    const char *bundle_path, const char *name);
int	container_start(struct ocifbsd_container *c);
int	container_kill(struct ocifbsd_container *c, int sig);
int	container_delete(struct ocifbsd_container *c);
int	container_exec(struct ocifbsd_container *c, char **args,
		    const char *cwd);
int	container_stop(struct ocifbsd_container *c, int timeout_sec);
int	container_pause(struct ocifbsd_container *c);
int	container_resume(struct ocifbsd_container *c);
int	container_reconfigure_network(struct ocifbsd_container *c);
int	container_wait(struct ocifbsd_container *c);
int	container_inspect(struct ocifbsd_container *c, char **json_out);
int	container_apply_mounts(struct ocifbsd_container *c);
int	container_unmount_all(struct ocifbsd_container *c);
struct ocifbsd_container *container_get_by_id(const char *id);
struct ocifbsd_container *container_get_by_name(const char *name);
struct ocifbsd_container *container_get_by_jid(int jid);
void	container_free(struct ocifbsd_container *c);

/* OCI spec parsing and translation */
struct oci_runtime_spec *oci_parse_config(const char *config_path);
void	oci_free_spec(struct oci_runtime_spec *spec);
struct jailparam *oci_spec_to_jail_params(const struct oci_runtime_spec *spec,
		    size_t *nparams);
void	oci_spec_default_hostname(struct oci_runtime_spec *spec,
		    const char *fallback);
struct netcfg;
void	netcfg_apply_to_spec(const struct netcfg *nc,
		    struct oci_runtime_spec *spec);
int	oci_validate_spec(const struct oci_runtime_spec *spec);

/* State management */
int	state_init(void);
int	state_save(const struct ocifbsd_container *c);
int	state_delete(const char *id);
struct ocifbsd_container *state_load(const char *id);
struct ocifbsd_container **state_list(int *n);
int	state_lock(void);
void	state_unlock(void);
/*
 * Cross-process per-container advisory lock. state_lock_container returns a
 * fd (>= 0) to pass to state_unlock_container, or -1 on error (fail closed).
 */
int	state_lock_container(const char *id);
void	state_unlock_container(int fd);

/* Hooks execution */
int	hooks_run_prestart(const struct ocifbsd_container *c);
int	hooks_run_poststart(const struct ocifbsd_container *c);
int	hooks_run_poststop(const struct ocifbsd_container *c);

/* Utility functions */
char   *generate_container_id(void);
char   *canonical_name(const char *name);
char   *resolve_bundle_path(const char *bundle);
int	ensure_directory(const char *path, mode_t mode);
int	safe_write(int fd, const void *buf, size_t n);
char   *read_file(const char *path, size_t *len);
int	write_file(const char *path, const void *data, size_t len);

/* Logging */
void	ocifbsd_log(int priority, const char *fmt, ...);
void	ocifbsd_log_init(const char *ident);
void	ocifbsd_set_verbose(bool verbose);

/* Error handling */
const char *ocifbsd_state_to_string(ocifbsd_state_t state);
ocifbsd_state_t ocifbsd_reconcile_state(ocifbsd_state_t stored, bool jail_alive);
const char *ocifbsd_strerror(int error);

/* Access control: root or the admin group may view/modify runtime state. */
enum ocifbsd_access_op {
	OCIFBSD_OP_VIEW,
	OCIFBSD_OP_MODIFY,
};
bool ocifbsd_access_allowed(uid_t euid, gid_t allowed_gid,
	    const gid_t *groups, int ngroups);
int ocifbsd_require_access(enum ocifbsd_access_op op);
void ocifbsd_secure_path(const char *path, mode_t mode);

/*
 * Container state type for orchestration compatibility
 */
typedef enum {
	CONTAINER_STATE_UNKNOWN = 0,
	CONTAINER_STATE_CREATED,
	CONTAINER_STATE_RUNNING,
	CONTAINER_STATE_STOPPED,
	CONTAINER_STATE_PAUSED,
	CONTAINER_STATE_RESTARTING,
	CONTAINER_STATE_REMOVING,
	CONTAINER_STATE_DEAD
} container_state_t;

/*
 * Orchestration interface functions
 * These provide a simpler interface for the orchestration layer
 */
int	ocifbsd_create_container(const char *name, const char *image,
	    const char *command, const char *args, const char *pod_id,
	    char **container_id);
int	ocifbsd_start_container(const char *container_id);
int	ocifbsd_stop_container(const char *container_id, int sig);
int	ocifbsd_delete_container(const char *container_id, bool force);
int	ocifbsd_get_container_state(const char *container_id,
	    container_state_t *state, int *exit_code);
int	ocifbsd_logs(const char *container_id, int tail, bool follow);
int	ocifbsd_pause_container(const char *container_id);
int	ocifbsd_resume_container(const char *container_id);
char	*ocifbsd_get_container_info(const char *container_id);

/*
 * Scheduler initialization for orchestration
 */
int	ocifbsd_scheduler_init(void);

/*
 * Event functions for orchestration
 */
int	ocifbsd_event_subscribe(void (*callback)(const char *, void *), void *arg);
int	ocifbsd_event_publish(const char *event_type, const char *message);

#endif /* _OCIFBSD_H */
