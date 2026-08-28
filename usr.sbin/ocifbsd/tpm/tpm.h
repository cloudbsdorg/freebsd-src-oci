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
 * TPM management header
 */

#ifndef _OCIFBSD_TPM_H
#define _OCIFBSD_TPM_H

#include <sys/param.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>

/* TPM device */
#define TPM_DEVICE "/dev/tpm0"
#define TPM_DEVICE1 "/dev/tpm"

#define OCIFBSD_TPM_AES_KEY_SIZE 32
#define OCIFBSD_TPM_SEALED_KEY_SIZE 256

/* Software-fallback sealed-key magic. Must fit the 16-bit magic field
 * (the previous 0x4F4346 truncated, so unseal could never match). */
#define TPM_SEALED_MAGIC 0x4346

/* Maximum number of PCR banks (matches struct tpm_quote's pcr_values). */
#define TPM_MAX_PCRS 24

/* TPM states */
#define TPM_STATE_NOT_FOUND    0
#define TPM_STATE_DISABLED     1
#define TPM_STATE_ENABLED       2
#define TPM_STATE_ACTIVE       3

/* TPM PCR banks */
#define TPM_PCR_SHA256 0

/* PCR indices */
#define TPM_PCR_BOOT_AGGRESSIVE 0
#define TPM_PCR_BOOT_POLICY     1
#define TPM_PCR_BOOT_SRTM       2
#define TPM_PCR_BOOT_MRTM       3
#define TPM_PCR_OCIFBSD_CONFIG  16
#define TPM_PCR_OCIFBSD_POLICY  17
#define TPM_PCR_OCIFBSD_KEY     18

/* TPM information */
struct tpm_info {
    int version;                /* 1.2 or 2.0 */
    int state;                  /* TPM_STATE_* */
    char manufacturer[32];
    char model[64];
    char firmware_version[32];
    bool has_nvram;
    bool has_rsa2048;
    bool has_sha256;
};

/* PCR measurement */
struct tpm_pcr {
    int index;
    int bank;                   /* SHA256, etc. */
    uint8_t value[32];
    char description[128];
};

/* TPM key blob for sealing */
struct tpm_sealed_key {
    uint16_t magic;             /* magic number */
    uint16_t version;            /* structure version */
    uint32_t key_length;         /* sealed key length */
    uint8_t sealed_data[];      /* variable length sealed data */
};

/* TPM attestation quote */
struct tpm_quote {
    uint8_t quote[256];         /* quote signature */
    uint8_t pcr_values[32 * 24];/* PCR values (max 24 banks) */
    uint8_t nonce[32];           /* challenge nonce */
    uint8_t signature[256];      /* quote signature */
};

/* TPM initialization */
int tpm_init(void);
int tpm_shutdown(void);

/* TPM status */
int tpm_present(void);
int tpm_get_info(struct tpm_info *info);
const char *tpm_get_version_string(int version);
const char *tpm_get_state_string(int state);

/* PCR operations */
int tpm_pcr_read(int pcr_index, uint8_t *value, size_t *value_len);
int tpm_pcr_extend(int pcr_index, const uint8_t *data, size_t data_len);
int tpm_pcr_reset(int pcr_index);
struct tpm_pcr *tpm_pcr_list(int *count);

/* Key sealing/unsealing */
int tpm_seal_data(const uint8_t *data, size_t data_len,
    const uint8_t *pcr_values, int n_pcrs,
    uint8_t **sealed_data, size_t *sealed_len);
int tpm_unseal_data(const uint8_t *sealed_data, size_t sealed_len,
    uint8_t *pcr_values, int n_pcrs,
    uint8_t **data, size_t *data_len);
int tpm_generate_key(uint8_t *key, size_t key_len);

/* Attestation */
int tpm_quote(const uint8_t *pcr_mask, int n_pcrs,
    const uint8_t *nonce, size_t nonce_len,
    struct tpm_quote **quote);
int tpm_verify_quote(struct tpm_quote *quote, uint8_t *pcr_mask, int n_pcrs);

/* Certificate operations */
int tpm_read_nvram(int index, uint8_t *data, size_t *data_len);
int tpm_write_nvram(int index, const uint8_t *data, size_t data_len);
int tpm_nvram_destroy(int index);
int tpm_nvram_lock(int index);

/* TPM commands (direct) */
int tpm_send_command(void *command, size_t cmd_len, void *response, size_t *resp_len);

/* Common-sensen CLI helpers */
int tpm_status human(struct tpm_info *info, FILE *fp);
int tpm_pcr_dump(int pcr_index, FILE *fp);
int tpm_seal_file(const char *input_file, const char *output_file, int *pcr_indices, int n_pcrs);
int tpm_unseal_file(const char *input_file, const char *output_file, int *pcr_indices, int n_pcrs);
int tpm_attest(char **quote_json, char **pcr_json);

#endif /* _OCIFBSD_TPM_H */
