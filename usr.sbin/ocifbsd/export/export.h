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
 * Cloud export header
 */

#ifndef _OCIFBSD_EXPORT_H
#define _OCIFBSD_EXPORT_H

#include <sys/param.h>
#include <sys/queue.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Export formats */
#define EXPORT_FORMAT_FEB     0   /* Flat Export Bundle */
#define EXPORT_FORMAT_RAW    1   /* Raw disk image */
#define EXPORT_FORMAT_QCOW2 2   /* QCOW2 */
#define EXPORT_FORMAT_VHD    3   /* Virtual Hard Disk */
#define EXPORT_FORMAT_AMI    4   /* AWS AMI */
#define EXPORT_FORMAT_GCE    5   /* GCP image */
#define EXPORT_FORMAT_AZURE  6   /* Azure VHD */

/* Cloud providers */
#define PROVIDER_AWS     0
#define PROVIDER_GCP     1
#define PROVIDER_AZURE   2
#define PROVIDER_LOCAL   3

/* Export status */
#define EXPORT_STATUS_PENDING    0
#define EXPORT_STATUS_RUNNING    1
#define EXPORT_STATUS_UPLOADING  2
#define EXPORT_STATUS_COMPLETE   3
#define EXPORT_STATUS_FAILED    4

/* Flat Export Bundle (.feb) structure */
struct feb_header {
    char            magic[4];       /* "FEB\0" */
    uint16_t        version;         /* Version 1 */
    uint16_t        flags;           /* Flags */
    uint32_t        metadata_size;    /* Size of metadata JSON */
    uint32_t        image_size;       /* Size of image data */
    uint32_t        volume_size;      /* Size of volume data */
    uint32_t        config_size;      /* Size of config data */
    uint8_t         checksum[32];     /* SHA256 checksum */
};

/* Export configuration */
struct export_config {
    int             format;         /* EXPORT_FORMAT_* */
    int             provider;        /* PROVIDER_* */
    bool            include_volumes;
    bool            include_config;
    bool            compress;
    int             compression_level;  /* 1-9 */
    bool            encrypt;
    char            *encryption_key;
    char            *output_path;
    char            *volume_snapshot;
    bool            freeze_container;
    int             network_timeout;
    int             upload_timeout;
};

/* AWS export config */
struct aws_export_config {
    char            *region;
    char            *bucket;
    char            *prefix;
    char            *ami_name;
    char            *ami_description;
    char            *instance_type;
    char            *ssh_key;
    char            *vpc_id;
    char            *subnet_id;
    char            *security_group_ids;
    bool            encrypt_volume;
    char            *kms_key_id;
    char            *sse_kms_key_id;
    char            *iam_role;
    bool            public_image;
};

/* GCP export config */
struct gcp_export_config {
    char            *project;
    char            *bucket;
    char            *image_name;
    char            *image_family;
    char            *machine_type;
    char            *network;
    char            *subnet;
    char            *zone;
    bool            use_temp_disk;
    uint64_t        temp_disk_size_gb;
    bool            encrypt;
    char            *kms_key;
    char            *service_account;
};

/* Azure export config */
struct azure_export_config {
    char            *resource_group;
    char            *storage_account;
    char            *container;
    char            *blob_name;
    char            *image_name;
    char            *location;
    char            *vm_size;
    char            *storage_sku;   /* Standard_LRS, Premium_LRS, etc. */
    int             os_type;         /* Linux=0, Windows=1 */
    bool            hyperv_generation; /* v1 or v2 */
    bool            encrypt;
    char            *vault_id;
    char            *disk_encryption_set;
};

/* Network interface mapping */
struct network_mapping {
    char            source_interface[64];
    char            source_mac[18];
    char            source_ip[16];
    char            source_netmask[16];
    char            source_gateway[16];
    char            *source_vlan;

    char            dest_interface[64];
    char            dest_network[16];
    char            dest_netmask[16];
    int             dest_dhcp;        /* 0=static, 1=dhcp */
};

/* Export job */
struct export_job {
    uint64_t        id;
    char            name[256];
    char            container_name[256];
    int             status;
    time_t          created;
    time_t          completed;
    int             progress;        /* 0-100 */
    char            error[512];
    char            *output_url;
    uint64_t        output_size;
    struct export_config config;
    TAILQ_ENTRY(export_job) next;
};

/* Export statistics */
struct export_stats {
    uint64_t        total_exports;
    uint64_t        successful_exports;
    uint64_t        failed_exports;
    uint64_t        bytes_exported;
    uint64_t        bytes_uploaded;
    time_t          last_export;
};

/* Function declarations */

/* Core export */
int     export_init(void);
int     export_shutdown(void);

int     export_container(const char *name, struct export_config *config,
            char **output_path);
int     export_jail(const char *name, struct export_config *config,
            char **output_path);

int     export_to_feb(const char *name, struct export_config *config,
            char **output_path);
int     import_from_feb(const char *feb_path, const char *target_name);

int     export_to_raw(const char *name, const char *output_path);
int     export_to_qcow2(const char *name, const char *output_path);

int     export_get_progress(uint64_t job_id);
int     export_cancel(uint64_t job_id);
struct export_job **export_list(int *count);

/* AWS export */
int     export_to_aws(const char *name, struct aws_export_config *aws_config,
            char **ami_id);
int     aws_upload_image(const char *image_path, struct aws_export_config *config,
            char **ami_id);
int     aws_import_image(const char *bucket, const char *key,
            char **ami_id);
int     aws_share_image(const char *ami_id, const char *account_id);
int     aws_copy_image(const char *ami_id, const char *dest_region);

/* GCP export */
int     export_to_gcp(const char *name, struct gcp_export_config *gcp_config,
            char **image_url);
int     gcp_upload_image(const char *image_path, struct gcp_export_config *config,
            char **image_url);
int     gcp_import_image(const char *bucket, const char *object,
            char **image_url);
int     gcp_share_image(const char *image_url, const char *project_id);

/* Azure export */
int     export_to_azure(const char *name, struct azure_export_config *az_config,
            char **disk_uri);
int     azure_upload_vhd(const char *vhd_path, struct azure_export_config *config,
            char **blob_uri);
int     azure_create_managed_image(const char *blob_uri,
            struct azure_export_config *config, char **image_id);

/* Network adaptation */
int     network_map_create(const char *container, struct network_mapping **maps,
            int *count);
int     network_adapt_aws(struct network_mapping *maps, int count);
int     network_adapt_gcp(struct network_mapping *maps, int count);
int     network_adapt_azure(struct network_mapping *maps, int count);
int     network_adapt_local(struct network_mapping *maps, int count);

char    *network_detect_interfaces(void);
int     network_get_config(const char *interface, char *ip, char *netmask,
            char *gateway);

/* Jail to container conversion */
int     jail_to_container(const char *jail_name, const char *container_name);
int     jail_export_config(const char *jail_name, const char *config_path);
int     container_import_config(const char *config_path, const char *container_name);

/* Volume export */
int     export_volume_snapshot(const char *volume_name, const char *snapshot_name);
int     export_volume_to_file(const char *volume_name, const char *output_path,
            uint64_t max_size);
int     export_volume_to_s3(const char *volume_name, const char *bucket,
            const char *key);

/* Configuration */
int     export_get_config(struct export_config *config);
int     export_set_config(struct export_config *config);

int     export_set_aws_credentials(const char *access_key, const char *secret_key);
int     export_set_gcp_credentials(const char *key_path);
int     export_set_azure_credentials(const char *subscription_id,
            const char *tenant_id, const char *client_id, const char *client_secret);

/* Statistics */
int     export_stats_get(struct export_stats *stats);
int     export_stats_json(char **json_out);

/* CLI commands */
int     cmd_export_run(int argc, char *argv[]);
int     cmd_export_list(int argc, char *argv[]);
int     cmd_export_status(int argc, char *argv[]);
int     cmd_export_cancel(int argc, char *argv[]);
int     cmd_export_aws(int argc, char *argv[]);
int     cmd_export_gcp(int argc, char *argv[]);
int     cmd_export_azure(int argc, char *argv[]);

#endif /* _OCIFBSD_EXPORT_H */
