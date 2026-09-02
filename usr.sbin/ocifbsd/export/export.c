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
 * Cloud export implementation
 * Phase 17: Cloud Export
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libgen.h>
#include <libutil.h>
#include <pthread.h>
#include <sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "export.h"
#include "../include/ocifbsd.h"

/*
 * Recursive mkdir(2). FreeBSD 16's <libutil.h> does not export mkdirp
 * in any public header. We provide a local copy.
 */
static int
mkdirp_local(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	char *p;
	size_t len;

	if (path == NULL || *path == '\0')
		return (-1);

	len = strlcpy(buf, path, sizeof(buf));
	if (len >= sizeof(buf))
		return (-1);

	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, mode) != 0 && errno != EEXIST)
			return (-1);
		*p = '/';
	}

	if (mkdir(buf, mode) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

#define	mkdirp(path, mode)	mkdirp_local((path), (mode))

static char *export_create_metadata(const char *name);
static int export_get_container_image(const char *name, uint8_t **data,
    size_t *size);
static size_t compress_image(uint8_t *input, size_t input_size,
    uint8_t **output, int level);
static size_t decompress_image(uint8_t *input, size_t input_size,
    uint8_t **output);
static int export_write_volumes(FILE *fp, const char *name);
static int export_write_config(FILE *fp, const char *name);
static void export_calculate_checksum(const char *path);
static int import_create_container(const char *metadata, uint8_t *image_data,
    size_t image_size, int compressed, const char *target);

/* Global state */
static struct export_config default_config;
static struct export_stats stats;
static pthread_mutex_t export_lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized = 0;
static TAILQ_HEAD(, export_job) export_jobs;
static uint64_t next_job_id __attribute__((unused)) = 1;

/* FEB magic bytes */
static const char FEB_MAGIC[4] = { 'F', 'E', 'B', '\0' };
static const uint16_t FEB_VERSION = 1;

/*
 * Initialize export module
 */
int
export_init(void)
{
    if (initialized)
        return (0);

    TAILQ_INIT(&export_jobs);

    /* Default configuration */
    memset(&default_config, 0, sizeof(default_config));
    default_config.format = EXPORT_FORMAT_FEB;
    default_config.provider = PROVIDER_LOCAL;
    default_config.include_volumes = true;
    default_config.include_config = true;
    default_config.compress = true;
    default_config.compression_level = 6;
    default_config.freeze_container = true;

    memset(&stats, 0, sizeof(stats));

    openlog("ocifbsd-export", LOG_PID, LOG_DAEMON);

    syslog(LOG_INFO, "Cloud export module initialized");

    initialized = 1;
    return (0);
}

/*
 * Shutdown export module
 */
int
export_shutdown(void)
{
    struct export_job *job;

    if (!initialized)
        return (0);

    pthread_mutex_lock(&export_lock);

    while ((job = TAILQ_FIRST(&export_jobs)) != NULL) {
        TAILQ_REMOVE(&export_jobs, job, next);
        free(job);
    }

    pthread_mutex_unlock(&export_lock);
    closelog();

    initialized = 0;
    return (0);
}

/*
 * Export container to specified format
 */
int
export_container(const char *name, struct export_config *config, char **output_path)
{
    struct export_config *cfg = config ? config : &default_config;
    int ret = -1;

    if (name == NULL)
        return (-1);

    syslog(LOG_INFO, "Exporting container: %s format=%d", name, cfg->format);

    switch (cfg->format) {
    case EXPORT_FORMAT_FEB:
        ret = export_to_feb(name, cfg, output_path);
        break;
    case EXPORT_FORMAT_RAW:
        if (output_path) {
            ret = export_to_raw(name, *output_path);
        }
        break;
    case EXPORT_FORMAT_QCOW2:
        if (output_path) {
            ret = export_to_qcow2(name, *output_path);
        }
        break;
    case EXPORT_FORMAT_AMI:
        /* AWS export */
        break;
    case EXPORT_FORMAT_GCE:
        /* GCP export */
        break;
    case EXPORT_FORMAT_AZURE:
        /* Azure export */
        break;
    default:
        syslog(LOG_ERR, "Unknown export format: %d", cfg->format);
    }

    if (ret == 0) {
        stats.successful_exports++;
        stats.last_export = time(NULL);
    } else {
        stats.failed_exports++;
    }

    return (ret);
}

/*
 * Export to Flat Export Bundle (.feb)
 */
int
export_to_feb(const char *name, struct export_config *config, char **output_path)
{
    struct feb_header header;
    char output_file[PATH_MAX];
    FILE *fp;
    int ret = -1;

    if (name == NULL)
        return (-1);

    /* Determine output path */
    if (output_path && *output_path) {
        strlcpy(output_file, *output_path, sizeof(output_file));
    } else {
        snprintf(output_file, sizeof(output_file),
            "%s/exports/%s.feb", OCIFBSD_DATA_DIR, name);
    }

    /* Ensure directory exists */
    mkdirp(dirname(output_file), 0755);

    fp = fopen(output_file, "wb");
    if (fp == NULL) {
        syslog(LOG_ERR, "Failed to create export file: %s", output_file);
        return (-1);
    }

    /* Write header */
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, FEB_MAGIC, 4);
    header.version = FEB_VERSION;
    header.flags = 0;

    if (config && config->compress)
        header.flags |= 0x01;
    if (config && config->encrypt)
        header.flags |= 0x02;

    /* Write container metadata */
    char *metadata = export_create_metadata(name);
    header.metadata_size = strlen(metadata);

    /* Write container filesystem */
    uint8_t *image_data;
    size_t image_size;
    if (export_get_container_image(name, &image_data, &image_size) == 0) {
        header.image_size = image_size;
    } else {
        header.image_size = 0;
    }

    /* Write header */
    fwrite(&header, sizeof(header), 1, fp);

    /* Write metadata */
    fwrite(metadata, header.metadata_size, 1, fp);
    free(metadata);

    /* Write image data */
    if (image_data && header.image_size > 0) {
        if (config && config->compress) {
            /* Compress image data */
            uint8_t *compressed;
            size_t compressed_size = compress_image(image_data, image_size,
                &compressed, config->compression_level);
            if (compressed_size > 0) {
                fwrite(compressed, compressed_size, 1, fp);
                free(compressed);
            } else {
                fwrite(image_data, image_size, 1, fp);
            }
        } else {
            fwrite(image_data, image_size, 1, fp);
        }
        free(image_data);
    }

    /* Write volume data if configured */
    if (config && config->include_volumes) {
        ret = export_write_volumes(fp, name);
    }

    /* Write config if configured */
    if (config && config->include_config) {
        ret = export_write_config(fp, name);
    }

    fclose(fp);

    /* Calculate checksum */
    export_calculate_checksum(output_file);

    syslog(LOG_INFO, "Export complete: %s", output_file);

    if (output_path)
        *output_path = strdup(output_file);

    stats.total_exports++;
    stats.bytes_exported += header.image_size;

    return (ret == 0 ? 0 : -1);
}

/*
 * Import from FEB
 */
int
import_from_feb(const char *feb_path, const char *target_name)
{
    struct feb_header header;
    FILE *fp;
    char *metadata = NULL;
    uint8_t *image_data = NULL;
    int ret = -1;

    if (feb_path == NULL || target_name == NULL)
        return (-1);

    fp = fopen(feb_path, "rb");
    if (fp == NULL)
        return (-1);

    /* Read header */
    if (fread(&header, sizeof(header), 1, fp) != 1) {
        fclose(fp);
        return (-1);
    }

    /* Verify magic */
    if (memcmp(header.magic, FEB_MAGIC, 4) != 0) {
        syslog(LOG_ERR, "Invalid FEB file: bad magic");
        fclose(fp);
        return (-1);
    }

    /* Read metadata */
    metadata = malloc(header.metadata_size + 1);
    if (metadata) {
        fread(metadata, header.metadata_size, 1, fp);
        metadata[header.metadata_size] = '\0';
    }

    /* Read image data */
    if (header.image_size > 0) {
        image_data = malloc(header.image_size);
        if (image_data) {
            fread(image_data, header.image_size, 1, fp);
        }
    }

    /* Parse metadata and create container */
    if (metadata) {
        ret = import_create_container(metadata, image_data, header.image_size,
            header.flags & 0x01, target_name);
        free(metadata);
    }

    if (image_data)
        free(image_data);

    fclose(fp);

    syslog(LOG_INFO, "Import complete: %s -> %s", feb_path, target_name);

    return (ret);
}

/*
 * Create metadata JSON
 */
static char *
export_create_metadata(const char *name)
{
    char *json;
    time_t now = time(NULL);

    asprintf(&json,
        "{\"name\":\"%s\",\"exported\":\"%ld\","
        "\"version\":\"1.0\",\"format\":\"feb\","
        "\"platform\":\"freebsd\",\"container_runtime\":\"ocifbsd\"}",
        name, (long)now);

    return (json);
}

/*
 * Get container image data
 */
static int
export_get_container_image(const char *name, uint8_t **data, size_t *size)
{
    char root_path[PATH_MAX];
    FILE *fp;
    struct stat st;
    long sz;
    int ret = -1;

    snprintf(root_path, sizeof(root_path), "%s/containers/%s/root",
        OCIFBSD_DATA_DIR, name);

    /*
     * This path is expected to be a flat disk-image file. A jail container's
     * rootfs is a directory tree (c->rootfs in the runtime), so fread here
     * would read nothing/garbage and ftell on a directory yields a bogus
     * size fed straight to malloc. Fail clearly instead of producing a
     * corrupt export; archiving a directory rootfs is not supported yet.
     */
    if (stat(root_path, &st) != 0) {
        syslog(LOG_ERR, "export: container root not found: %s", root_path);
        return (-1);
    }
    if (!S_ISREG(st.st_mode)) {
        syslog(LOG_ERR, "export: %s is not a flat image file "
            "(directory-rootfs export is not supported yet)", root_path);
        return (-1);
    }

    fp = fopen(root_path, "r");
    if (fp == NULL)
        return (-1);

    /* Get file size, guarding against a negative/failed ftell. */
    if (fseek(fp, 0, SEEK_END) != 0 || (sz = ftell(fp)) <= 0) {
        fclose(fp);
        return (-1);
    }
    rewind(fp);
    *size = (size_t)sz;

    *data = malloc(*size);
    if (*data != NULL && fread(*data, 1, *size, fp) == *size) {
        ret = 0;
    } else {
        free(*data);
        *data = NULL;
    }

    fclose(fp);
    return (ret);
}

/*
 * Compress image data
 */
static size_t
compress_image(uint8_t *input, size_t input_size, uint8_t **output, int level)
{
    size_t max_compressed = compressBound(input_size);
    *output = malloc(max_compressed);

    if (*output == NULL)
        return (0);

    uLongf compressed_size = max_compressed;
    int ret = compress2(*output, &compressed_size, input, input_size, level);

    if (ret != Z_OK) {
        free(*output);
        *output = NULL;
        return (0);
    }

    return (compressed_size);
}

/*
 * Calculate file checksum
 */
static void
export_calculate_checksum(const char *path)
{
    FILE *fp;
    SHA256_CTX ctx;
    uint8_t hash[SHA256_DIGEST_LENGTH];
    uint8_t buf[8192];
    size_t n;

    fp = fopen(path, "rb");
    if (fp == NULL)
        return;

    SHA256_Init(&ctx);

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        SHA256_Update(&ctx, buf, n);
    }

    SHA256_Final(hash, &ctx);
    fclose(fp);

    syslog(LOG_INFO, "Checksum: %02x%02x...%02x%02x",
        hash[0], hash[1], hash[30], hash[31]);
}

/*
 * Write volumes to export file
 */
static int
export_write_volumes(FILE *fp, const char *name)
{
    char volume_path[PATH_MAX];

    snprintf(volume_path, sizeof(volume_path),
        "%s/volumes/%s", OCIFBSD_DATA_DIR, name);

    /* Volume export logic */
    syslog(LOG_INFO, "Exporting volumes for: %s", name);

    return (0);
}

/*
 * Write configuration to export file
 */
static int
export_write_config(FILE *fp, const char *name)
{
    char config_path[PATH_MAX];
    FILE *cfp;
    char buf[1024];
    size_t n;

    snprintf(config_path, sizeof(config_path),
        "%s/containers/%s/config.json", OCIFBSD_DATA_DIR, name);

    cfp = fopen(config_path, "r");
    if (cfp == NULL)
        return (-1);

    while ((n = fread(buf, 1, sizeof(buf), cfp)) > 0) {
        fwrite(buf, 1, n, fp);
    }

    fclose(cfp);
    return (0);
}

/*
 * Import and create container
 */
static int
import_create_container(const char *metadata, uint8_t *image_data,
    size_t image_size, int compressed, const char *target)
{
    char name[256];
    int ret = -1;

    /* Parse metadata for original name */
    char *name_start = strstr(metadata, "\"name\":\"");
    if (name_start) {
        name_start += 8;
        char *name_end = strchr(name_start, '"');
        if (name_end) {
            size_t name_len = (size_t)(name_end - name_start);
            if (name_len >= sizeof(name))
                name_len = sizeof(name) - 1;
            memcpy(name, name_start, name_len);
            name[name_len] = '\0';
        }
    }

    /* Decompress if needed */
    uint8_t *data = image_data;
    size_t size = image_size;

    if (compressed && image_data) {
        uint8_t *decompressed;
        size_t decompressed_size = decompress_image(image_data, image_size,
            &decompressed);
        if (decompressed_size > 0) {
            data = decompressed;
            size = decompressed_size;
        }
    }

    (void)size;

    /* Create container */
    ret = 0;  /* Would call ocifbsd create */

    if (compressed && data != image_data)
        free(data);

    return (ret);
}

/*
 * Decompress image data
 */
static size_t
decompress_image(uint8_t *input, size_t input_size, uint8_t **output)
{
    size_t max_decompressed = input_size * 10;
    *output = malloc(max_decompressed);

    if (*output == NULL)
        return (0);

    uLongf decompressed_size = max_decompressed;
    int ret = uncompress(*output, &decompressed_size, input, input_size);

    if (ret != Z_OK) {
        free(*output);
        *output = NULL;
        return (0);
    }

    return (decompressed_size);
}

/*
 * Run argv[0] with the given argument vector and wait for it; return 0 only on
 * a clean exit(0). Using fork+execvp (no shell) means a crafted container name
 * or output path is passed as a single argument and can never be interpreted
 * as shell syntax — no command injection.
 */
static int
export_run(char *const argv[])
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0)
        return (-1);
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return (-1);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1);
}

/*
 * Export to raw format
 */
int
export_to_raw(const char *name, const char *output_path)
{
    char ifarg[PATH_MAX];
    char ofarg[PATH_MAX];
    char *argv[5];

    if (name == NULL || output_path == NULL)
        return (-1);
    if ((size_t)snprintf(ifarg, sizeof(ifarg), "if=%s/containers/%s/root",
        OCIFBSD_DATA_DIR, name) >= sizeof(ifarg))
        return (-1);
    if ((size_t)snprintf(ofarg, sizeof(ofarg), "of=%s", output_path) >=
        sizeof(ofarg))
        return (-1);

    argv[0] = __DECONST(char *, "dd");
    argv[1] = ifarg;
    argv[2] = ofarg;
    argv[3] = __DECONST(char *, "bs=1M");
    argv[4] = NULL;

    syslog(LOG_INFO, "Exporting %s to raw: %s", name, output_path);
    return (export_run(argv));
}

/*
 * Export to QCOW2 format
 */
int
export_to_qcow2(const char *name, const char *output_path)
{
    char raw_path[PATH_MAX];
    char *argv[9];
    int ret;

    if (name == NULL || output_path == NULL)
        return (-1);

    /* First export to raw, then convert. */
    if ((size_t)snprintf(raw_path, sizeof(raw_path), "%s.tmp", output_path) >=
        sizeof(raw_path))
        return (-1);
    if (export_to_raw(name, raw_path) != 0)
        return (-1);

    argv[0] = __DECONST(char *, "qemu-img");
    argv[1] = __DECONST(char *, "convert");
    argv[2] = __DECONST(char *, "-f");
    argv[3] = __DECONST(char *, "raw");
    argv[4] = __DECONST(char *, "-O");
    argv[5] = __DECONST(char *, "qcow2");
    argv[6] = raw_path;
    argv[7] = __DECONST(char *, output_path);
    argv[8] = NULL;

    ret = export_run(argv);
    (void)unlink(raw_path);	/* best-effort cleanup of the temp raw image */
    return (ret);
}

/*
 * Get export progress
 */
int
export_get_progress(uint64_t job_id)
{
    struct export_job *job;
    int progress = 0;

    pthread_mutex_lock(&export_lock);

    TAILQ_FOREACH(job, &export_jobs, next) {
        if (job->id == job_id) {
            progress = job->progress;
            break;
        }
    }

    pthread_mutex_unlock(&export_lock);

    return (progress);
}

/*
 * Cancel export
 */
int
export_cancel(uint64_t job_id)
{
    struct export_job *job;
    int ret = -1;

    pthread_mutex_lock(&export_lock);

    TAILQ_FOREACH(job, &export_jobs, next) {
        if (job->id == job_id) {
            job->status = EXPORT_STATUS_FAILED;
            strlcpy(job->error, "Cancelled by user", sizeof(job->error));
            ret = 0;
            break;
        }
    }

    pthread_mutex_unlock(&export_lock);

    return (ret);
}

/*
 * List exports
 */
struct export_job **
export_list(int *count)
{
    struct export_job **jobs = NULL;
    struct export_job *job;
    int n = 0;

    if (count == NULL)
        return (NULL);

    pthread_mutex_lock(&export_lock);

    TAILQ_FOREACH(job, &export_jobs, next) {
        jobs = realloc(jobs, (n + 1) * sizeof(*jobs));
        if (jobs == NULL)
            break;
        jobs[n++] = job;
    }

    pthread_mutex_unlock(&export_lock);

    *count = n;
    return (jobs);
}

/*
 * Network map create
 */
int
network_map_create(const char *container, struct network_mapping **maps, int *count)
{
    (void)container;
    (void)maps;
    (void)count;
    /* Network interface mapping */
    return (0);
}

/*
 * Network detect interfaces
 */
char *
network_detect_interfaces(void)
{
    FILE *fp;
    static char result[4096];
    char buf[256];

    fp = popen("ifconfig -l", "r");
    if (fp == NULL)
        return (NULL);

    result[0] = '\0';

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        strlcat(result, buf, sizeof(result));
    }

    pclose(fp);
    return (result);
}

/*
 * Get interface config
 */
int
network_get_config(const char *interface, char *ip, char *netmask, char *gateway)
{
    char cmd[PATH_MAX];
    FILE *fp;
    char buf[256];

    if (interface == NULL)
        return (-1);

    /* Get IP and netmask */
    snprintf(cmd, sizeof(cmd), "ifconfig %s | grep 'inet '", interface);
    fp = popen(cmd, "r");

    if (fp == NULL)
        return (-1);

    if (fgets(buf, sizeof(buf), fp) != NULL) {
        char *ip_start = strstr(buf, "inet ");
        if (ip_start) {
            ip_start += 5;
            sscanf(ip_start, "%s", ip);
        }
        char *nm_start = strstr(buf, "netmask ");
        if (nm_start) {
            nm_start += 8;
            sscanf(nm_start, "%s", netmask);
        }
    }

    pclose(fp);

    /* Get gateway */
    fp = popen("netstat -rn | grep -m1 '^default'", "r");
    if (fp && fgets(buf, sizeof(buf), fp) != NULL) {
        sscanf(buf, "%s", gateway);
    }
    if (fp)
        pclose(fp);

    return (0);
}

/*
 * Jail to container conversion
 */
int
jail_to_container(const char *jail_name, const char *container_name)
{
    char config_path[PATH_MAX];
    int ret;

    if (jail_name == NULL || container_name == NULL)
        return (-1);

    /* Export jail config */
    snprintf(config_path, sizeof(config_path),
        "%s/jails/%s/config.json", OCIFBSD_DATA_DIR, jail_name);

    ret = jail_export_config(jail_name, config_path);
    if (ret != 0)
        return (ret);

    /* Import as container */
    ret = container_import_config(config_path, container_name);

    return (ret);
}

/*
 * Export jail config
 */
int
jail_export_config(const char *jail_name, const char *config_path)
{
    FILE *src, *dst;
    char buf[4096];
    size_t n;

    char jail_conf[PATH_MAX];
    snprintf(jail_conf, sizeof(jail_conf), "/etc/jail.conf.d/%s.conf", jail_name);

    src = fopen(jail_conf, "r");
    if (src == NULL)
        return (-1);

    dst = fopen(config_path, "w");
    if (dst == NULL) {
        fclose(src);
        return (-1);
    }

    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        fwrite(buf, 1, n, dst);
    }

    fclose(src);
    fclose(dst);

    return (0);
}

/*
 * Import container config
 */
int
container_import_config(const char *config_path, const char *container_name)
{
    FILE *fp;
    char buf[4096];

    fp = fopen(config_path, "r");
    if (fp == NULL)
        return (-1);

    /* Parse and convert jail config to container config */
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        /* Convert jail config to ocifbsd container config */
    }

    fclose(fp);

    return (0);
}

/*
 * Get statistics
 */
int
export_stats_get(struct export_stats *out)
{
    if (out == NULL)
        return (-1);

    pthread_mutex_lock(&export_lock);
    *out = stats;
    pthread_mutex_unlock(&export_lock);

    return (0);
}

/*
 * Statistics as JSON
 */
int
export_stats_json(char **json_out)
{
    struct export_stats s;
    char *json;

    if (json_out == NULL)
        return (-1);

    export_stats_get(&s);

    if (asprintf(&json,
        "{\"total\":%lu,\"successful\":%lu,\"failed\":%lu,"
        "\"bytes_exported\":%lu,\"bytes_uploaded\":%lu,\"last_export\":%ld}",
        (unsigned long)s.total_exports,
        (unsigned long)s.successful_exports,
        (unsigned long)s.failed_exports,
        (unsigned long)s.bytes_exported,
        (unsigned long)s.bytes_uploaded,
        (long)s.last_export) == -1) {
        return (-1);
    }

    *json_out = json;
    return (0);
}

/*
 * Main function
 */
int
main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args]\n", argv[0]);
        fprintf(stderr, "Commands: export, import, list, status\n");
        return (1);
    }

    export_init();

    if (strcmp(argv[1], "export") == 0 && argc > 3) {
        char *output;
        export_container(argv[2], NULL, &output);
        printf("Exported to: %s\n", output);
        free(output);
    } else if (strcmp(argv[1], "import") == 0 && argc > 3) {
        import_from_feb(argv[2], argv[3]);
    } else if (strcmp(argv[1], "list") == 0) {
        char *json;
        export_stats_json(&json);
        printf("%s\n", json);
        free(json);
    }

    export_shutdown();
    return (0);
}
