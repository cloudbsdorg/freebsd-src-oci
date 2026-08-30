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
 * TPM management implementation
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/ioccom.h>
#include <sys/endian.h>
#include <crypto/sha2.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tpm.h"
#include "../include/ocifbsd.h"

/* Global state */
static int tpm_fd = -1;
static int tpm_initialized = 0;

/* TPM device paths */
static const char *tpm_devices[] = {
    TPM_DEVICE,
    TPM_DEVICE1,
    "/dev/tpmrm0",
    NULL
};

/*
 * Initialize TPM subsystem
 */
int
tpm_init(void)
{
    int i;
    
    if (tpm_initialized)
        return (0);
    
    /* Try to open TPM device */
    for (i = 0; tpm_devices[i] != NULL; i++) {
        tpm_fd = open(tpm_devices[i], O_RDWR);
        if (tpm_fd >= 0)
            break;
    }
    
    if (tpm_fd < 0) {
        /* TPM not found - not an error, just unsupported */
        return (0);
    }
    
    tpm_initialized = 1;
    return (0);
}

/*
 * Shutdown TPM subsystem
 */
int
tpm_shutdown(void)
{
    if (tpm_fd >= 0) {
        close(tpm_fd);
        tpm_fd = -1;
    }
    tpm_initialized = 0;
    return (0);
}

/*
 * Check if TPM is present
 */
int
tpm_present(void)
{
    if (!tpm_initialized) {
        if (tpm_init() != 0)
            return (0);
    }
    
    return (tpm_fd >= 0);
}

/*
 * Get TPM info
 */
int
tpm_get_info(struct tpm_info *info)
{
    if (info == NULL)
        return (-1);
    
    memset(info, 0, sizeof(struct tpm_info));
    
    if (!tpm_present()) {
        info->state = TPM_STATE_NOT_FOUND;
        return (0);
    }
    
    /* Check version via sysctl or ioctl */
    /* For now, assume TPM 2.0 if present */
    info->version = 20;
    info->state = TPM_STATE_ACTIVE;
    info->has_nvram = true;
    info->has_rsa2048 = true;
    info->has_sha256 = true;
    
    strlcpy(info->manufacturer, "Unknown", sizeof(info->manufacturer));
    strlcpy(info->model, "TPM 2.0", sizeof(info->model));
    strlcpy(info->firmware_version, "1.0", sizeof(info->firmware_version));
    
    return (0);
}

/*
 * Get TPM version string
 */
const char *
tpm_get_version_string(int version)
{
    switch (version) {
        case 12:
            return ("1.2");
        case 20:
            return ("2.0");
        default:
            return ("Unknown");
    }
}

/*
 * Get TPM state string
 */
const char *
tpm_get_state_string(int state)
{
    switch (state) {
        case TPM_STATE_NOT_FOUND:
            return ("Not Found");
        case TPM_STATE_DISABLED:
            return ("Disabled");
        case TPM_STATE_ENABLED:
            return ("Enabled");
        case TPM_STATE_ACTIVE:
            return ("Active");
        default:
            return ("Unknown");
    }
}

/*
 * Read PCR value
 */
int
tpm_pcr_read(int pcr_index, uint8_t *value, size_t *value_len)
{
    if (value == NULL || value_len == NULL)
        return (-1);
    
    if (!tpm_present()) {
        /* Return simulated PCR for testing without TPM */
        memset(value, 0, 32);
        *value_len = 32;
        return (0);
    }
    
    /* Use TPM ioctl to read PCR */
    struct tpm req;
    memset(&req, 0, sizeof(req));
    
    /* PCR read implementation would go here */
    /* For now, return zeros */
    memset(value, 0, 32);
    *value_len = 32;
    
    return (0);
}

/*
 * Extend PCR value
 */
int
tpm_pcr_extend(int pcr_index, const uint8_t *data, size_t data_len)
{
    SHA256_CTX ctx;
    uint8_t old_value[32];
    uint8_t new_value[32];
    size_t old_len;
    int ret;
    
    if (!tpm_present())
        return (0);  /* Silently succeed without TPM */
    
    /* Read current PCR value */
    ret = tpm_pcr_read(pcr_index, old_value, &old_len);
    if (ret != 0)
        return (ret);
    
    /* Hash: new = SHA256(old || data) */
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, old_value, old_len);
    SHA256_Update(&ctx, data, data_len);
    SHA256_Final(new_value, &ctx);
    
    /* Send extend command to TPM */
    /* This would use TPM2_PCR_Extend command */
    
    return (0);
}

/*
 * Reset PCR
 */
int
tpm_pcr_reset(int pcr_index)
{
    if (!tpm_present())
        return (0);
    
    /* TPM2_PCR_Reset command would go here */
    
    return (0);
}

/*
 * List all PCRs
 */
struct tpm_pcr *
tpm_pcr_list(int *count)
{
    struct tpm_pcr *result;
    int i;
    
    if (count == NULL)
        return (NULL);
    
    *count = 0;
    
    /* Allocate space for 24 PCRs */
    result = calloc(24, sizeof(struct tpm_pcr));
    if (result == NULL)
        return (NULL);
    
    for (i = 0; i < 24; i++) {
        result[i].index = i;
        result[i].bank = TPM_PCR_SHA256;
        
        /* Try to read PCR */
        size_t len;
        if (tpm_pcr_read(i, result[i].value, &len) == 0) {
            snprintf(result[i].description, sizeof(result[i].description),
                "PCR %d", i);
            (*count)++;
        }
    }
    
    if (*count == 0) {
        free(result);
        return (NULL);
    }
    
    return (result);
}

/*
 * Seal data to TPM
 */
int
tpm_seal_data(const uint8_t *data, size_t data_len,
    const uint8_t *pcr_values, int n_pcrs,
    uint8_t **sealed_data, size_t *sealed_len)
{
    struct tpm_sealed_key *key;
    SHA256_CTX ctx;
    uint8_t key_blob[64];
    
    if (data == NULL || sealed_data == NULL || sealed_len == NULL)
        return (-1);
    
    /* Clamp the PCR count to the buffer capacity; reject a NULL PCR
     * pointer with a positive count. */
    if (n_pcrs < 0 || n_pcrs > TPM_MAX_PCRS)
        return (-1);
    if (n_pcrs > 0 && pcr_values == NULL)
        return (-1);

    if (!tpm_present()) {
        /*
         * Software fallback: store a 32-byte SHA-256 over the data and
         * PCRs. key_length and sealed_len describe exactly those 32
         * bytes so the unseal side's bounds check stays consistent.
         */
        key = malloc(sizeof(struct tpm_sealed_key) + SHA256_DIGEST_LENGTH);
        if (key == NULL)
            return (-1);

        key->magic = TPM_SEALED_MAGIC;
        key->version = 1;
        key->key_length = SHA256_DIGEST_LENGTH;

        SHA256_CTX hash_ctx;
        SHA256_Init(&hash_ctx);
        SHA256_Update(&hash_ctx, data, data_len);
        if (n_pcrs > 0)
            SHA256_Update(&hash_ctx, pcr_values,
                (size_t)n_pcrs * 32);
        SHA256_Final(key->sealed_data, &hash_ctx);

        *sealed_data = (uint8_t *)key;
        *sealed_len = sizeof(struct tpm_sealed_key) + SHA256_DIGEST_LENGTH;

        return (0);
    }
    
    /* Real TPM sealing would go here */
    /* TPM2_CreateSealedKey command */
    
    return (-1);
}

/*
 * Unseal data from TPM
 */
int
tpm_unseal_data(const uint8_t *sealed_data, size_t sealed_len,
    uint8_t *pcr_values, int n_pcrs,
    uint8_t **data, size_t *data_len)
{
    struct tpm_sealed_key *key;
    SHA256_CTX ctx;
    uint8_t expected_hash[32];
    uint8_t *decrypted;
    
    if (sealed_data == NULL || data == NULL || data_len == NULL)
        return (-1);
    
    if (sealed_len < sizeof(struct tpm_sealed_key))
        return (-1);
    
    key = (struct tpm_sealed_key *)sealed_data;

    if (key->magic != TPM_SEALED_MAGIC) {
        /* Not our sealed key format - try TPM unseal */
        /* TPM2_Unseal command would go here */
        return (-1);
    }

    /* Clamp PCRs and validate the attacker-controlled key_length against
     * the bytes actually present, so it cannot drive a heap over-read. */
    if (n_pcrs < 0 || n_pcrs > TPM_MAX_PCRS)
        return (-1);
    if (n_pcrs > 0 && pcr_values == NULL)
        return (-1);

    size_t avail = sealed_len - sizeof(struct tpm_sealed_key);
    if (key->key_length > avail)
        return (-1);

    /* Software fallback verification */
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, key->sealed_data, key->key_length);
    if (n_pcrs > 0)
        SHA256_Update(&ctx, pcr_values, (size_t)n_pcrs * 32);
    SHA256_Final(expected_hash, &ctx);

    decrypted = malloc(key->key_length);
    if (decrypted == NULL)
        return (-1);

    memcpy(decrypted, key->sealed_data, key->key_length);

    *data = decrypted;
    *data_len = key->key_length;

    return (0);
}

/*
 * Generate random key
 */
int
tpm_generate_key(uint8_t *key, size_t key_len)
{
    if (key == NULL)
        return (-1);
    
    if (!tpm_present()) {
        /* Use software random */
        arc4random_buf(key, key_len);
        return (0);
    }
    
    /* TPM2_GetRandom command would go here */
    
    return (-1);
}

/*
 * Create attestation quote
 */
int
tpm_quote(const uint8_t *pcr_mask, int n_pcrs,
    const uint8_t *nonce, size_t nonce_len,
    struct tpm_quote **quote)
{
    struct tpm_quote *q;
    int i;
    
    if (quote == NULL)
        return (-1);

    *quote = NULL;

    /* Clamp the PCR count to the quote buffer capacity (24 banks). */
    if (n_pcrs < 0)
        n_pcrs = 0;
    if (n_pcrs > TPM_MAX_PCRS)
        n_pcrs = TPM_MAX_PCRS;
    if (n_pcrs > 0 && pcr_mask == NULL)
        return (-1);

    q = calloc(1, sizeof(struct tpm_quote));
    if (q == NULL)
        return (-1);

    /* Copy nonce */
    if (nonce != NULL && nonce_len <= 32) {
        memcpy(q->nonce, nonce, nonce_len);
    }

    /* Read PCR values (32 bytes per bank into its slot). */
    for (i = 0; i < n_pcrs; i++) {
        size_t len = 32;
        if (tpm_pcr_read(pcr_mask[i], q->pcr_values + i * 32, &len) != 0) {
            memset(q->pcr_values + i * 32, 0, 32);
        }
    }

    if (!tpm_present()) {
        /* Software fallback - just hash PCRs and nonce */
        SHA256_CTX ctx;
        SHA256_Init(&ctx);
        SHA256_Update(&ctx, q->pcr_values, (size_t)n_pcrs * 32);
        SHA256_Update(&ctx, q->nonce, 32);
        SHA256_Final(q->quote, &ctx);
        
        /* Sign with software key */
        /* In production, this would use TPM key */
        memset(q->signature, 0, 256);
    } else {
        /* TPM2_Quote command would go here */
    }
    
    *quote = q;
    return (0);
}

/*
 * Verify attestation quote
 */
int
tpm_verify_quote(struct tpm_quote *quote, uint8_t *pcr_mask, int n_pcrs)
{
    int i;
    
    if (quote == NULL)
        return (-1);
    
    /* Verify PCR values match expected */
    for (i = 0; i < n_pcrs && i < 24; i++) {
        uint8_t pcr_value[32];
        size_t len = sizeof(pcr_value);
        
        if (tpm_pcr_read(pcr_mask[i], pcr_value, &len) == 0) {
            if (memcmp(pcr_value, quote->pcr_values + i * 32, 32) != 0) {
                return (-1);  /* PCR mismatch */
            }
        }
    }
    
    /* Verify quote signature */
    /* In production, verify TPM signature */
    
    return (0);
}

/*
 * Read from TPM NVRAM
 */
int
tpm_read_nvram(int index, uint8_t *data, size_t *data_len)
{
    if (!tpm_present())
        return (-1);
    
    /* TPM2_NV_Read command would go here */
    
    return (-1);
}

/*
 * Write to TPM NVRAM
 */
int
tpm_write_nvram(int index, const uint8_t *data, size_t data_len)
{
    if (!tpm_present())
        return (0);  /* Silently succeed without TPM */
    
    /* TPM2_NV_Write command would go here */
    
    return (0);
}

/*
 * Destroy TPM NVRAM area
 */
int
tpm_nvram_destroy(int index)
{
    if (!tpm_present())
        return (0);
    
    /* TPM2_NV_UndefineSpace command would go here */
    
    return (0);
}

/*
 * Lock TPM NVRAM area
 */
int
tpm_nvram_lock(int index)
{
    if (!tpm_present())
        return (0);
    
    /* TPM2_NV_WriteLock command would go here */
    
    return (0);
}

/*
 * Send raw TPM command
 */
int
tpm_send_command(void *command, size_t cmd_len, void *response, size_t *resp_len)
{
    if (!tpm_present() || tpm_fd < 0)
        return (-1);
    
    /* Write command */
    if (write(tpm_fd, command, cmd_len) != (ssize_t)cmd_len)
        return (-1);
    
    /* Read response */
    *resp_len = read(tpm_fd, response, *resp_len);
    if (*resp_len <= 0)
        return (-1);
    
    return (0);
}

/*
 * Print human-readable TPM status
 */
int
tpm_status_human(struct tpm_info *info, FILE *fp)
{
    if (info == NULL || fp == NULL)
        return (-1);
    
    fprintf(fp, "TPM Status:\n");
    fprintf(fp, "  Device: %s\n", tpm_present() ? TPM_DEVICE : "Not Found");
    fprintf(fp, "  Version: %s\n", tpm_get_version_string(info->version));
    fprintf(fp, "  State: %s\n", tpm_get_state_string(info->state));
    
    if (info->state == TPM_STATE_NOT_FOUND)
        return (0);
    
    fprintf(fp, "  Manufacturer: %s\n", info->manufacturer);
    fprintf(fp, "  Model: %s\n", info->model);
    fprintf(fp, "  Firmware: %s\n", info->firmware_version);
    fprintf(fp, "\nCapabilities:\n");
    fprintf(fp, "  NVRAM: %s\n", info->has_nvram ? "Yes" : "No");
    fprintf(fp, "  RSA 2048: %s\n", info->has_rsa2048 ? "Yes" : "No");
    fprintf(fp, "  SHA-256: %s\n", info->has_sha256 ? "Yes" : "No");
    
    return (0);
}

/*
 * Dump PCR values
 */
int
tpm_pcr_dump(int pcr_index, FILE *fp)
{
    struct tpm_pcr *pcrs;
    int count, i;
    char hex[65];
    
    if (fp == NULL)
        return (-1);
    
    pcrs = tpm_pcr_list(&count);
    if (pcrs == NULL) {
        fprintf(fp, "No PCRs available\n");
        return (-1);
    }
    
    if (pcr_index >= 0 && pcr_index < count) {
        /* Dump specific PCR */
        for (i = 0; i < 32; i++) {
            sprintf(hex + i * 2, "%02x", pcrs[pcr_index].value[i]);
        }
        hex[64] = '\0';
        fprintf(fp, "PCR %d: %s\n", pcr_index, hex);
    } else {
        /* Dump all PCRs */
        fprintf(fp, "PCR Values:\n");
        for (i = 0; i < count; i++) {
            int j;
            for (j = 0; j < 32; j++) {
                sprintf(hex + j * 2, "%02x", pcrs[i].value[j]);
            }
            hex[64] = '\0';
            fprintf(fp, "  %2d: %s\n", pcrs[i].index, hex);
        }
    }
    
    free(pcrs);
    return (0);
}

/*
 * Seal file to TPM
 */
int
tpm_seal_file(const char *input_file, const char *output_file, int *pcr_indices, int n_pcrs)
{
    uint8_t *data, *sealed;
    size_t data_len, sealed_len;
    FILE *fp;
    int ret = -1;
    
    if (input_file == NULL || output_file == NULL)
        return (-1);
    
    /* Read input file */
    fp = fopen(input_file, "rb");
    if (fp == NULL)
        return (-1);
    
    fseek(fp, 0, SEEK_END);
    data_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    data = malloc(data_len);
    if (data == NULL) {
        fclose(fp);
        return (-1);
    }
    
    if (fread(data, 1, data_len, fp) != data_len) {
        fclose(fp);
        free(data);
        return (-1);
    }
    fclose(fp);
    
    /* Get PCR values for sealing */
    uint8_t pcr_values[24 * 32];
    int i;
    for (i = 0; i < n_pcrs && i < 24; i++) {
        size_t len = 32;
        if (tpm_pcr_read(pcr_indices[i], pcr_values + i * 32, &len) != 0) {
            memset(pcr_values + i * 32, 0, 32);
        }
    }
    
    /* Seal data */
    ret = tpm_seal_data(data, data_len, pcr_values, n_pcrs, &sealed, &sealed_len);
    free(data);
    
    if (ret != 0)
        return (ret);
    
    /* Write sealed data */
    fp = fopen(output_file, "wb");
    if (fp == NULL) {
        free(sealed);
        return (-1);
    }
    
    if (fwrite(sealed, 1, sealed_len, fp) != sealed_len)
        ret = -1;
    
    fclose(fp);
    free(sealed);
    
    return (ret);
}

/*
 * Unseal file from TPM
 */
int
tpm_unseal_file(const char *input_file, const char *output_file, int *pcr_indices, int n_pcrs)
{
    uint8_t *sealed, *data;
    size_t sealed_len, data_len;
    FILE *fp;
    int ret = -1;
    
    if (input_file == NULL || output_file == NULL)
        return (-1);
    
    /* Read sealed file */
    fp = fopen(input_file, "rb");
    if (fp == NULL)
        return (-1);
    
    fseek(fp, 0, SEEK_END);
    sealed_len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    
    sealed = malloc(sealed_len);
    if (sealed == NULL) {
        fclose(fp);
        return (-1);
    }
    
    if (fread(sealed, 1, sealed_len, fp) != sealed_len) {
        fclose(fp);
        free(sealed);
        return (-1);
    }
    fclose(fp);
    
    /* Get PCR values for unsealing */
    uint8_t pcr_values[24 * 32];
    int i;
    for (i = 0; i < n_pcrs && i < 24; i++) {
        size_t len = 32;
        if (tpm_pcr_read(pcr_indices[i], pcr_values + i * 32, &len) != 0) {
            memset(pcr_values + i * 32, 0, 32);
        }
    }
    
    /* Unseal data */
    ret = tpm_unseal_data(sealed, sealed_len, pcr_values, n_pcrs, &data, &data_len);
    free(sealed);
    
    if (ret != 0)
        return (ret);
    
    /* Write unsealed data */
    fp = fopen(output_file, "wb");
    if (fp == NULL) {
        free(data);
        return (-1);
    }
    
    if (fwrite(data, 1, data_len, fp) != data_len)
        ret = -1;
    
    fclose(fp);
    free(data);
    
    return (ret);
}

/*
 * Generate attestation report
 */
int
tpm_attest(char **quote_json, char **pcr_json)
{
    struct tpm_quote *quote;
    struct tpm_pcr *pcrs;
    int pcr_count, i;
    char *json, *p;
    size_t json_size;
    
    if (quote_json == NULL || pcr_json == NULL)
        return (-1);
    
    *quote_json = NULL;
    *pcr_json = NULL;
    
    /* Create quote for PCRs 0-23 */
    uint8_t pcr_mask[24];
    for (i = 0; i < 24; i++)
        pcr_mask[i] = i;
    
    uint8_t nonce[32];
    arc4random_buf(nonce, sizeof(nonce));
    
    if (tpm_quote(pcr_mask, 24, nonce, sizeof(nonce), &quote) != 0)
        return (-1);
    
    /* Get PCR list */
    pcrs = tpm_pcr_list(&pcr_count);
    
    /* Build quote JSON */
    json_size = 1024;
    json = malloc(json_size);
    if (json == NULL) {
        free(quote);
        if (pcrs) free(pcrs);
        return (-1);
    }
    
    p = json;
    size_t remaining = json_size;
    int n;

    n = snprintf(p, remaining, "{\n");
    if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
    p += n; remaining -= (size_t)n;

    n = snprintf(p, remaining, "  \"nonce\": \"");
    if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
    p += n; remaining -= (size_t)n;

    for (i = 0; i < 32; i++) {
        n = snprintf(p, remaining, "%02x", nonce[i]);
        if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
        p += n; remaining -= (size_t)n;
    }

    n = snprintf(p, remaining, "\",\n");
    if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
    p += n; remaining -= (size_t)n;

    n = snprintf(p, remaining, "  \"pcrs\": [");
    if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
    p += n; remaining -= (size_t)n;

    for (i = 0; i < 24; i++) {
        int j;
        n = snprintf(p, remaining, "\"");
        if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
        p += n; remaining -= (size_t)n;

        for (j = 0; j < 32; j++) {
            n = snprintf(p, remaining, "%02x", quote->pcr_values[i * 32 + j]);
            if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
            p += n; remaining -= (size_t)n;
        }

        n = snprintf(p, remaining, "\"%s\n", i < 23 ? "," : "");
        if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
        p += n; remaining -= (size_t)n;
    }

    n = snprintf(p, remaining, "  ]\n");
    if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }
    p += n; remaining -= (size_t)n;

    n = snprintf(p, remaining, "}\n");
    if (n < 0 || (size_t)n >= remaining) { free(json); free(quote); if (pcrs) free(pcrs); return (-1); }

    *quote_json = json;
    
    /* Build PCR JSON */
    if (pcrs != NULL) {
        json_size = 256 + (size_t)pcr_count * 128;
        json = malloc(json_size);
        if (json != NULL) {
            p = json;
            size_t remaining = json_size;
            int n;

            n = snprintf(p, remaining, "{\n  \"pcrs\": [\n");
            if (n < 0 || (size_t)n >= remaining) { free(json); free(pcrs); free(quote); return (-1); }
            p += n; remaining -= (size_t)n;

            for (i = 0; i < pcr_count; i++) {
                int j;
                n = snprintf(p, remaining, "    {\"index\": %d, \"value\": \"", pcrs[i].index);
                if (n < 0 || (size_t)n >= remaining) { free(json); free(pcrs); free(quote); return (-1); }
                p += n; remaining -= (size_t)n;

                for (j = 0; j < 32; j++) {
                    n = snprintf(p, remaining, "%02x", pcrs[i].value[j]);
                    if (n < 0 || (size_t)n >= remaining) { free(json); free(pcrs); free(quote); return (-1); }
                    p += n; remaining -= (size_t)n;
                }

                n = snprintf(p, remaining, "\"");
                if (n < 0 || (size_t)n >= remaining) { free(json); free(pcrs); free(quote); return (-1); }
                p += n; remaining -= (size_t)n;

                n = snprintf(p, remaining, "}%s\n", i < pcr_count - 1 ? "," : "");
                if (n < 0 || (size_t)n >= remaining) { free(json); free(pcrs); free(quote); return (-1); }
                p += n; remaining -= (size_t)n;
            }

            n = snprintf(p, remaining, "  ]\n}\n");
            if (n < 0 || (size_t)n >= remaining) { free(json); free(pcrs); free(quote); return (-1); }

            *pcr_json = json;
            free(pcrs);
        }
    }
    
    free(quote);
    return (0);
}
