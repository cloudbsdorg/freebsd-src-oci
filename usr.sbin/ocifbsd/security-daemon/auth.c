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
 * Authentication and authorization implementation
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <crypto/rijndael/rijndael.h>
#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <login_cap.h>
#include <pam_conv.h>
#include <pam_types.h>
#include <pwd.h>
#include <sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "auth.h"
#include "../include/ocifbsd.h"

/* Global state */
static int auth_initialized = 0;
static pthread_mutex_t auth_lock = PTHREAD_MUTEX_INITIALIZER;

/* User database (simple file-based for now) */
static struct {
    struct user_identity **users;
    int n_users;
} user_db;

/* Token registry */
static struct token_tree token_registry;
static int token_registry_initialized = 0;
static pthread_mutex_t token_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t next_token_id = 1;

/* Secret registry */
static struct secret_tree secret_registry;
static int secret_registry_initialized = 0;
static pthread_mutex_t secret_lock = PTHREAD_MUTEX_INITIALIZER;

/* Audit log */
static struct audit_tree audit_registry;
static int audit_registry_initialized = 0;
static pthread_mutex_t audit_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t next_audit_id = 1;
static FILE *audit_file = NULL;

/* Role definitions */
static struct {
    char name[64];
    int permissions;
} role_defs[] = {
    { ROLE_CLUSTER_ADMIN, PERM_READ | PERM_WRITE | PERM_DELETE | PERM_ADMIN },
    { ROLE_ADMIN, PERM_READ | PERM_WRITE | PERM_DELETE },
    { ROLE_EDITOR, PERM_READ | PERM_WRITE },
    { ROLE_VIEWER, PERM_READ },
    { ROLE_OPERATOR, PERM_READ | PERM_WRITE },
};

/* Encryption key (in production, load from secure key storage) */
static uint8_t encryption_key[32];
static int encryption_key_initialized = 0;

/* Default role permissions */
static int default_permissions[] = {
    PERM_READ,                   /* viewer */
    PERM_READ | PERM_WRITE,      /* editor */
    PERM_READ | PERM_WRITE | PERM_DELETE,  /* admin */
    PERM_READ | PERM_WRITE | PERM_DELETE | PERM_ADMIN,  /* cluster-admin */
    PERM_READ | PERM_WRITE,      /* operator */
};

/*
 * Compare functions for RB trees
 */
int token_compare(struct auth_token *a, struct auth_token *b)
{
    return (strcmp(a->token_id, b->token_id));
}

int secret_compare(struct secret *a, struct secret *b)
{
    int cmp = strcmp(a->namespace, b->namespace);
    if (cmp != 0)
        return (cmp);
    return (strcmp(a->name, b->name));
}

int audit_compare(struct audit_entry *a, struct audit_entry *b)
{
    if (a->id < b->id)
        return (-1);
    if (a->id > b->id)
        return (1);
    return (0);
}

int rb_compare(struct role_binding *a, struct role_binding *b)
{
    int cmp = strcmp(a->namespace, b->namespace);
    if (cmp != 0)
        return (cmp);
    return (strcmp(a->name, b->name));
}

/*
 * Initialize auth subsystem
 */
int
auth_init(void)
{
    if (__sync_fetch_and_add(&auth_initialized, 0) != 0)
        return (0);
    
    pthread_mutex_lock(&auth_lock);
    
    if (!token_registry_initialized) {
        RB_INIT(&token_registry);
        token_registry_initialized = 1;
    }
    
    if (!secret_registry_initialized) {
        RB_INIT(&secret_registry);
        secret_registry_initialized = 1;
    }
    
    if (!audit_registry_initialized) {
        RB_INIT(&audit_registry);
        audit_registry_initialized = 1;
        
        /* Open audit log file */
        audit_file = fopen(OCIFBSD_VAR_DIR "/audit.log", "a");
    }
    
    /* Initialize encryption key */
    if (!encryption_key_initialized) {
        /* In production, read from secure key storage */
        arc4random_buf(encryption_key, sizeof(encryption_key));
        encryption_key_initialized = 1;
    }
    
    /* Create default users if they don't exist */
    auth_user_create("admin", NULL);  /* NULL = generate random password */
    
    __sync_fetch_and_add(&auth_initialized, 1);
    pthread_mutex_unlock(&auth_lock);
    
    return (0);
}

/*
 * Shutdown auth subsystem
 */
int
auth_shutdown(void)
{
    if (__sync_fetch_and_add(&auth_initialized, 0) == 0)
        return (0);
    
    if (audit_file != NULL) {
        fclose(audit_file);
        audit_file = NULL;
    }
    
    __sync_fetch_and_add(&auth_initialized, 0);
    return (0);
}

/*
 * Create user
 */
int
auth_user_create(const char *username, const char *password)
{
    struct user_identity *user;
    
    if (username == NULL)
        return (-1);
    
    pthread_mutex_lock(&auth_lock);
    
    /* Check if user exists */
    for (int i = 0; i < user_db.n_users; i++) {
        if (strcmp(user_db.users[i]->username, username) == 0) {
            pthread_mutex_unlock(&auth_lock);
            errno = EEXIST;
            return (-1);
        }
    }
    
    user = calloc(1, sizeof(struct user_identity));
    if (user == NULL) {
        pthread_mutex_unlock(&auth_lock);
        return (-1);
    }
    
    strlcpy(user->username, username, sizeof(user->username));
    user->enabled = true;
    user->last_login = 0;
    user->password_expires = time(NULL) + 90 * 24 * 60 * 60;  /* 90 days */
    
    /* Add to user database */
    user_db.users = realloc(user_db.users,
        (user_db.n_users + 1) * sizeof(struct user_identity *));
    user_db.users[user_db.n_users++] = user;
    
    /* Set default role based on username */
    if (strcmp(username, "admin") == 0) {
        user->roles = malloc(2 * sizeof(char *));
        user->roles[0] = strdup(ROLE_CLUSTER_ADMIN);
        user->roles[1] = NULL;
        user->n_roles = 1;
    } else {
        user->roles = malloc(2 * sizeof(char *));
        user->roles[0] = strdup(ROLE_VIEWER);
        user->roles[1] = NULL;
        user->n_roles = 1;
    }
    
    /* If password provided, hash and store it */
    if (password != NULL) {
        /* Password storage would go here */
    }
    
    pthread_mutex_unlock(&auth_lock);
    
    /* Audit log */
    audit_log(username, "user.create", RES_CLUSTER, username, "success", "user created");
    
    return (0);
}

/*
 * Delete user
 */
int
auth_user_delete(const char *username)
{
    int i;
    
    if (username == NULL)
        return (-1);
    
    pthread_mutex_lock(&auth_lock);
    
    for (i = 0; i < user_db.n_users; i++) {
        if (strcmp(user_db.users[i]->username, username) == 0) {
            /* Revoke all tokens for this user */
            auth_token_revoke_user(username);
            
            /* Remove from database */
            free(user_db.users[i]);
            for (; i < user_db.n_users - 1; i++) {
                user_db.users[i] = user_db.users[i + 1];
            }
            user_db.n_users--;
            
            pthread_mutex_unlock(&auth_lock);
            
            audit_log(username, "user.delete", RES_CLUSTER, username, "success", "user deleted");
            return (0);
        }
    }
    
    pthread_mutex_unlock(&auth_lock);
    errno = ENOENT;
    return (-1);
}

/*
 * Get user by username
 */
struct user_identity *
auth_user_get(const char *username)
{
    struct user_identity *user = NULL;
    
    if (username == NULL)
        return (NULL);
    
    pthread_mutex_lock(&auth_lock);
    
    for (int i = 0; i < user_db.n_users; i++) {
        if (strcmp(user_db.users[i]->username, username) == 0) {
            user = user_db.users[i];
            break;
        }
    }
    
    pthread_mutex_unlock(&auth_lock);
    
    return (user);
}

/*
 * List all users
 */
struct user_identity **
auth_users_list(int *count)
{
    struct user_identity **list;
    
    if (count == NULL)
        return (NULL);
    
    pthread_mutex_lock(&auth_lock);
    
    list = malloc(user_db.n_users * sizeof(struct user_identity *));
    if (list != NULL) {
        for (int i = 0; i < user_db.n_users; i++) {
            list[i] = user_db.users[i];
        }
        *count = user_db.n_users;
    }
    
    pthread_mutex_unlock(&auth_lock);
    
    return (list);
}

/*
 * Authenticate user with password
 */
int
auth_authenticate(const char *username, const char *password)
{
    struct user_identity *user;
    
    user = auth_user_get(username);
    if (user == NULL)
        return (-1);
    
    if (!user->enabled) {
        errno = EACCES;
        return (-1);
    }
    
    if (user->password_expires < time(NULL)) {
        errno = ETIMEDOUT;
        return (-1);
    }
    
    /*
     * Password verification is not yet implemented because
     * struct user_identity has no password_hash field. To fix:
     *
     *   1. Add to struct user_identity (in auth.h):
     *        char password_hash[256];  /* crypt(3) output */
     *
     *   2. Add auth_user_add() function that takes a plaintext
     *      password, calls crypt(password, salt) to hash it, and
     *      stores the result in password_hash.
     *
     *   3. Replace this comment with:
     *        if (strcmp(user->password_hash, crypt(password,
     *            user->password_hash)) != 0) {
     *            errno = EACCES;
     *            return (-1);
     *        }
     *
     * Until that refactor, the function accepts any password.
     * This is a SECURITY ISSUE for production deployment.
     * See MIGRATION.md and SECURITY.md for the full plan.
     */
    (void)password;
    
    pthread_mutex_lock(&auth_lock);
    user->last_login = time(NULL);
    pthread_mutex_unlock(&auth_lock);
    
    return (0);
}

/*
 * Authenticate via PAM
 */
int
auth_authenticate_pam(const char *username, const char *password)
{
    pam_handle_t *pamh;
    struct pam_conv conv;
    int pam_err;
    struct pam_message msg;
    const struct pam_message *msgp;
    struct pam_response *resp;
    
    if (username == NULL || password == NULL)
        return (-1);
    
    /* Setup PAM conversation */
    msg.msg_style = PAM_PROMPT_ECHO_OFF;
    msg.msg = password;
    msgp = &msg;
    resp = NULL;
    
    conv.conv = function pam_conv_func;
    conv.appdata_ptr = (void *)password;
    
    pam_err = pam_start("ocifbsd", username, &conv, &pamh);
    if (pam_err != PAM_SUCCESS)
        return (-1);
    
    pam_err = pam_authenticate(pamh, 0);
    if (pam_err != PAM_SUCCESS) {
        pam_end(pamh, pam_err);
        return (-1);
    }
    
    pam_err = pam_acct_mgmt(pamh, 0);
    pam_end(pamh, pam_err);
    
    if (pam_err == PAM_SUCCESS) {
        /* Update last login */
        struct user_identity *user = auth_user_get(username);
        if (user != NULL) {
            pthread_mutex_lock(&auth_lock);
            user->last_login = time(NULL);
            pthread_mutex_unlock(&auth_lock);
        }
        return (0);
    }
    
    return (-1);
}

/*
 * Create auth token
 */
int
auth_token_create(const char *username, char *token_out, size_t token_len)
{
    struct auth_token *token;
    struct user_identity *user;
    SHA256_CTX ctx;
    uint8_t hash[SHA256_DIGEST_LENGTH];
    
    if (username == NULL || token_out == NULL)
        return (-1);
    
    user = auth_user_get(username);
    if (user == NULL)
        return (-1);
    
    pthread_mutex_lock(&token_lock);
    
    token = calloc(1, sizeof(struct auth_token));
    if (token == NULL) {
        pthread_mutex_unlock(&token_lock);
        return (-1);
    }
    
    /* Generate token ID */
    uint64_t token_id = __sync_fetch_and_add(&next_token_id, 1);
    snprintf(token->token_id, sizeof(token->token_id), "%lu", token_id);
    
    /* Add random component */
    uint8_t random_bytes[16];
    arc4random_buf(random_bytes, sizeof(random_bytes));
    char random_hex[33];
    for (int i = 0; i < 16; i++) {
        sprintf(random_hex + i * 2, "%02x", random_bytes[i]);
    }
    random_hex[32] = '\0';
    
    strlcat(token->token_id, random_hex, sizeof(token->token_id));
    
    /* Hash the token for storage */
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, token->token_id, strlen(token->token_id));
    SHA256_Final(hash, &ctx);
    
    /* Store hash */
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(token->token_id + i * 2, "%02x", hash[i]);
    }
    
    strlcpy(token->username, username, sizeof(token->username));
    token->created = time(NULL);
    token->expires = time(NULL) + 24 * 60 * 60;  /* 24 hours */
    token->last_used = time(NULL);
    token->permissions = PERM_READ;  /* Default permissions */
    
    /* Calculate permissions from roles */
    for (int i = 0; i < user->n_roles; i++) {
        for (int j = 0; j < sizeof(role_defs) / sizeof(role_defs[0]); j++) {
            if (strcmp(user->roles[i], role_defs[j].name) == 0) {
                token->permissions |= role_defs[j].permissions;
            }
        }
    }
    
    RB_INSERT(token_tree, &token_registry, token);
    pthread_mutex_unlock(&token_lock);
    
    /* Return original token (not hash) */
    snprintf(token_out, token_len, "%lu%s", token_id, random_hex);
    
    audit_log(username, "auth.token.create", RES_CLUSTER, token->token_id, "success", "token created");
    
    return (0);
}

/*
 * Validate token
 */
int
auth_token_validate(const char *token, struct user_identity *user_out)
{
    struct auth_token token_find, *token_entry;
    SHA256_CTX ctx;
    uint8_t hash[SHA256_DIGEST_LENGTH];
    char token_hash[65];
    
    if (token == NULL || user_out == NULL)
        return (-1);
    
    /* Hash the token */
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, token, strlen(token));
    SHA256_Final(hash, &ctx);
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(token_hash + i * 2, "%02x", hash[i]);
    }
    token_hash[64] = '\0';
    
    strlcpy(token_find.token_id, token_hash, sizeof(token_find.token_id));
    
    pthread_mutex_lock(&token_lock);
    token_entry = RB_FIND(token_tree, &token_registry, &token_find);
    
    if (token_entry == NULL) {
        pthread_mutex_unlock(&token_lock);
        return (-1);
    }
    
    /* Check expiration */
    if (token_entry->expires < time(NULL)) {
        pthread_mutex_unlock(&token_lock);
        errno = ETIMEDOUT;
        return (-1);
    }
    
    /* Update last used */
    token_entry->last_used = time(NULL);
    
    /* Get user info */
    struct user_identity *user = auth_user_get(token_entry->username);
    if (user != NULL && user_out != NULL) {
        memcpy(user_out, user, sizeof(struct user_identity));
    }
    
    pthread_mutex_unlock(&token_lock);
    
    return (0);
}

/*
 * Revoke token
 */
int
auth_token_revoke(const char *token)
{
    struct auth_token token_find, *token_entry;
    SHA256_CTX ctx;
    uint8_t hash[SHA256_DIGEST_LENGTH];
    char token_hash[65];
    
    if (token == NULL)
        return (-1);
    
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, token, strlen(token));
    SHA256_Final(hash, &ctx);
    
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(token_hash + i * 2, "%02x", hash[i]);
    }
    token_hash[64] = '\0';
    
    strlcpy(token_find.token_id, token_hash, sizeof(token_find.token_id));
    
    pthread_mutex_lock(&token_lock);
    token_entry = RB_FIND(token_tree, &token_registry, &token_find);
    
    if (token_entry != NULL) {
        RB_REMOVE(token_tree, &token_registry, token_entry);
        free(token_entry);
    }
    
    pthread_mutex_unlock(&token_lock);
    
    return (token_entry != NULL ? 0 : -1);
}

/*
 * Revoke all tokens for user
 */
int
auth_token_revoke_user(const char *username)
{
    struct auth_token *token, *next;
    
    if (username == NULL)
        return (-1);
    
    pthread_mutex_lock(&token_lock);
    
    RB_FOREACH_SAFE(token, token_tree, &token_registry, next) {
        if (strcmp(token->username, username) == 0) {
            RB_REMOVE(token_tree, &token_registry, token);
            free(token);
        }
    }
    
    pthread_mutex_unlock(&token_lock);
    
    return (0);
}

/*
 * Check permission
 */
int
auth_check_permission(const char *username, const char *resource, int permission)
{
    struct user_identity *user;
    int i, j;
    
    if (username == NULL || resource == NULL)
        return (-1);
    
    user = auth_user_get(username);
    if (user == NULL)
        return (-1);
    
    /* Check user roles for permission */
    for (i = 0; i < user->n_roles; i++) {
        for (j = 0; j < sizeof(role_defs) / sizeof(role_defs[0]); j++) {
            if (strcmp(user->roles[i], role_defs[j].name) == 0) {
                if ((role_defs[j].permissions & permission) == permission) {
                    return (0);  /* Permission granted */
                }
            }
        }
    }
    
    return (-1);
}

/*
 * Create secret
 */
int
secret_create(const char *name, const char *namespace, const char *type, const void *data, size_t len)
{
    struct secret *secret;
    
    if (name == NULL || namespace == NULL || data == NULL)
        return (-1);
    
    pthread_mutex_lock(&secret_lock);
    
    secret = calloc(1, sizeof(struct secret));
    if (secret == NULL) {
        pthread_mutex_unlock(&secret_lock);
        return (-1);
    }
    
    strlcpy(secret->name, name, sizeof(secret->name));
    strlcpy(secret->namespace, namespace, sizeof(secret->namespace));
    if (type != NULL)
        strlcpy(secret->type, type, sizeof(secret->type));
    else
        strlcpy(secret->type, "opaque", sizeof(secret->type));
    
    /* Encrypt data */
    if (secret_encrypt(data, len, (void **)&secret->data, &secret->data_len) != 0) {
        free(secret);
        pthread_mutex_unlock(&secret_lock);
        return (-1);
    }
    
    secret->created = time(NULL);
    secret->updated = time(NULL);
    secret->version = 1;
    
    RB_INSERT(secret_tree, &secret_registry, secret);
    pthread_mutex_unlock(&secret_lock);
    
    return (0);
}

/*
 * Get secret
 */
void *
secret_get(const char *name, const char *namespace, size_t *len)
{
    struct secret secret_find, *secret_entry;
    void *decrypted;
    
    if (name == NULL || namespace == NULL)
        return (NULL);
    
    strlcpy(secret_find.name, name, sizeof(secret_find.name));
    strlcpy(secret_find.namespace, namespace, sizeof(secret_find.namespace));
    
    pthread_mutex_lock(&secret_lock);
    secret_entry = RB_FIND(secret_tree, &secret_registry, &secret_find);
    
    if (secret_entry == NULL) {
        pthread_mutex_unlock(&secret_lock);
        return (NULL);
    }
    
    /* Decrypt data */
    if (secret_decrypt(secret_entry->data, secret_entry->data_len, &decrypted, len) != 0) {
        pthread_mutex_unlock(&secret_lock);
        return (NULL);
    }
    
    pthread_mutex_unlock(&secret_lock);
    
    return (decrypted);
}

/*
 * Encrypt data using AES-256-CBC.
 * Output format: 16-byte IV || ciphertext (PKCS#7 padded)
 */
int
secret_encrypt(void *data, size_t len, void **out, size_t *out_len)
{
    keyInstance key;
    uint8_t iv[16];
    uint8_t *buf;
    size_t padded_len;
    int pad;
    size_t i;

    if (data == NULL || out == NULL || len == 0)
        return (-1);

    if (!encryption_key_initialized) {
        errno = EINVAL;
        return (-1);
    }

    /* PKCS#7 pad to next 16-byte boundary */
    pad = 16 - (len % 16);
    padded_len = len + pad;

    /* Output: 16-byte IV + padded ciphertext */
    *out_len = 16 + padded_len;
    buf = malloc(*out_len);
    if (buf == NULL)
        return (-1);

    /* Generate random IV */
    arc4random_buf(iv, sizeof(iv));
    memcpy(buf, iv, sizeof(iv));

    /* Initialize AES-256 key schedule */
    if (rijndael_makeKey(&key, DIR_ENCRYPT, 256, encryption_key) != 1) {
        free(buf);
        errno = EINVAL;
        return (-1);
    }

    /* Encrypt each 16-byte block in CBC mode */
    uint8_t prev[16];
    memcpy(prev, iv, sizeof(prev));
    for (i = 0; i < padded_len; i += 16) {
        uint8_t block[16];
        size_t in_len = (i + 16 <= len) ? 16 : (len - i);
        size_t j;

        memcpy(block, (uint8_t *)data + i, in_len);
        /* PKCS#7 padding */
        for (j = in_len; j < 16; j++)
            block[j] = (uint8_t)pad;
        /* CBC: XOR with previous ciphertext (or IV) */
        for (j = 0; j < 16; j++)
            block[j] ^= prev[j];
        /* Encrypt */
        rijndael_encrypt(block, &buf[16 + i], &key);
        memcpy(prev, &buf[16 + i], sizeof(prev));
    }

    *out = buf;
    return (0);
}

/*
 * Decrypt data using AES-256-CBC.
 * Input format: 16-byte IV || ciphertext (PKCS#7 padded)
 */
int
secret_decrypt(void *data, size_t len, void **out, size_t *out_len)
{
    keyInstance key;
    const uint8_t *iv;
    const uint8_t *ciphertext;
    size_t ciphertext_len;
    uint8_t *buf;
    size_t i;

    if (data == NULL || out == NULL)
        return (-1);
    /* Need at least IV (16) + one block (16) = 32 bytes */
    if (len < 32 || (len % 16) != 0) {
        errno = EINVAL;
        return (-1);
    }

    if (!encryption_key_initialized) {
        errno = EINVAL;
        return (-1);
    }

    iv = (const uint8_t *)data;
    ciphertext = iv + 16;
    ciphertext_len = len - 16;

    buf = malloc(ciphertext_len);
    if (buf == NULL)
        return (-1);

    /* Initialize AES-256 key schedule */
    if (rijndael_makeKey(&key, DIR_DECRYPT, 256, encryption_key) != 1) {
        free(buf);
        errno = EINVAL;
        return (-1);
    }

    /* Decrypt each 16-byte block in CBC mode */
    for (i = 0; i < ciphertext_len; i += 16) {
        uint8_t block[16];
        size_t j;

        memcpy(block, &ciphertext[i], 16);
        rijndael_decrypt(block, &buf[i], &key);
        /* CBC: XOR with previous ciphertext (or IV) */
        if (i == 0) {
            for (j = 0; j < 16; j++)
                buf[i + j] ^= iv[j];
        } else {
            for (j = 0; j < 16; j++)
                buf[i + j] ^= ciphertext[i - 16 + j];
        }
    }

    /* Remove PKCS#7 padding */
    uint8_t pad = buf[ciphertext_len - 1];
    if (pad < 1 || pad > 16) {
        free(buf);
        errno = EINVAL;
        return (-1);
    }
    *out_len = ciphertext_len - pad;
    *out = buf;
    return (0);
}

/*
 * Audit log
 */
int
audit_log(const char *user, const char *action, const char *resource,
    const char *resource_name, const char *result, const char *details)
{
    struct audit_entry *entry;
    char line[2048];
    
    if (user == NULL)
        user = "system";
    if (action == NULL)
        action = "unknown";
    
    entry = calloc(1, sizeof(struct audit_entry));
    if (entry == NULL)
        return (-1);
    
    entry->id = __sync_fetch_and_add(&next_audit_id, 1);
    entry->timestamp = time(NULL);
    
    strlcpy(entry->user, user, sizeof(entry->user));
    if (action != NULL)
        strlcpy(entry->action, action, sizeof(entry->action));
    if (resource != NULL)
        strlcpy(entry->resource, resource, sizeof(entry->resource));
    if (resource_name != NULL)
        strlcpy(entry->resource_name, resource_name, sizeof(entry->resource_name));
    if (result != NULL)
        strlcpy(entry->result, result, sizeof(entry->result));
    if (details != NULL)
        strlcpy(entry->details, details, sizeof(entry->details));
    
    /* Write to audit file */
    pthread_mutex_lock(&audit_lock);
    
    if (audit_file != NULL) {
        snprintf(line, sizeof(line),
            "{\"id\":%lu,\"ts\":%ld,\"user\":\"%s\",\"action\":\"%s\",\"resource\":\"%s\",\"name\":\"%s\",\"result\":\"%s\",\"details\":\"%s\"}\n",
            entry->id,
            (long)entry->timestamp,
            entry->user,
            entry->action,
            entry->resource,
            entry->resource_name,
            entry->result,
            entry->details);
        fputs(line, audit_file);
        fflush(audit_file);
    }
    
    RB_INSERT(audit_tree, &audit_registry, entry);
    pthread_mutex_unlock(&audit_lock);
    
    return (0);
}

/*
 * Query audit logs
 */
struct audit_entry **
audit_query(time_t start, time_t end, const char *user, const char *action, const char *resource, int *count)
{
    struct audit_entry **result;
    struct audit_entry *entry;
    int alloc = 256;
    int n = 0;
    
    if (count == NULL)
        return (NULL);
    
    *count = 0;
    result = calloc(alloc, sizeof(struct audit_entry *));
    if (result == NULL)
        return (NULL);
    
    pthread_mutex_lock(&audit_lock);
    
    RB_FOREACH(entry, audit_tree, &audit_registry) {
        if (start != 0 && entry->timestamp < start)
            continue;
        if (end != 0 && entry->timestamp > end)
            continue;
        if (user != NULL && strcmp(entry->user, user) != 0)
            continue;
        if (action != NULL && strcmp(entry->action, action) != 0)
            continue;
        if (resource != NULL && strcmp(entry->resource, resource) != 0)
            continue;
        
        if (n >= alloc) {
            alloc *= 2;
            result = realloc(result, alloc * sizeof(struct audit_entry *));
            if (result == NULL) {
                pthread_mutex_unlock(&audit_lock);
                *count = 0;
                return (NULL);
            }
        }
        
        result[n++] = entry;
    }
    
    pthread_mutex_unlock(&audit_lock);
    
    *count = n;
    return (result);
}

/*
 * Export audit logs as JSON
 */
int
audit_export_json(FILE *fp, time_t start, time_t end)
{
    struct audit_entry **entries;
    int count, i;
    
    if (fp == NULL)
        return (-1);
    
    entries = audit_query(start, end, NULL, NULL, NULL, &count);
    if (entries == NULL)
        return (-1);
    
    fprintf(fp, "{\"audit\":[\n");
    for (i = 0; i < count; i++) {
        fprintf(fp,
            "{\"id\":%lu,\"ts\":%ld,\"user\":\"%s\",\"action\":\"%s\",\"resource\":\"%s\",\"name\":\"%s\",\"result\":\"%s\"}%s\n",
            entries[i]->id,
            (long)entries[i]->timestamp,
            entries[i]->user,
            entries[i]->action,
            entries[i]->resource,
            entries[i]->resource_name,
            entries[i]->result,
            i < count - 1 ? "," : "");
    }
    fprintf(fp, "]}\n");
    
    free(entries);
    return (0);
}

/*
 * Validate a certificate identifier (CA name / node name / node id) before it
 * is interpolated into an openssl command line run via system(3). These are
 * root-daemon helpers, so an id containing shell metacharacters — e.g. a
 * peer-supplied node id like "n`reboot`" or "n; rm -rf /" — would be command
 * injection. Accept only the characters real cert identifiers use
 * (alphanumerics and - _ .), which excludes every shell metacharacter.
 */
static bool
auth_id_is_safe(const char *id)
{
    size_t i, len;

    if (id == NULL)
        return (false);
    len = strlen(id);
    if (len == 0 || len > 128)
        return (false);
    for (i = 0; i < len; i++) {
        char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return (false);
    }
    return (true);
}

/*
 * Generate CA certificate
 */
int
cert_generate_ca(const char *name)
{
    char cmd[1024];

    if (!auth_id_is_safe(name))
        return (-1);
    
    /* Generate CA using openssl */
    snprintf(cmd, sizeof(cmd),
        "openssl req -new -x509 -days 3650 -nodes "
        " -keyout %s/%s.key -out %s/%s.crt "
        " -subj '/CN=%s-CA' 2>/dev/null",
        OCIFBSD_CERT_DIR, name,
        OCIFBSD_CERT_DIR, name,
        name);
    
    return (system(cmd));
}

/*
 * Generate node certificate
 */
int
cert_generate_node(const char *name, const char *node_id)
{
    char cmd[1024];

    if (!auth_id_is_safe(name) || !auth_id_is_safe(node_id))
        return (-1);
    
    /* Generate node key and CSR */
    snprintf(cmd, sizeof(cmd),
        "openssl req -new -nodes "
        " -keyout %s/%s.key -out /tmp/%s.csr "
        " -subj '/CN=node-%s' 2>/dev/null",
        OCIFBSD_CERT_DIR, name,
        name, node_id);
    
    if (system(cmd) != 0)
        return (-1);
    
    /* Sign with CA */
    snprintf(cmd, sizeof(cmd),
        "openssl x509 -req -days 365 "
        " -in /tmp/%s.csr "
        " -CA %s/ca.crt -CAkey %s/ca.key "
        " -out %s/%s.crt "
        " -extfile /dev/stdin <<< 'subjectAltName=DNS:node-%s' 2>/dev/null",
        name,
        OCIFBSD_CERT_DIR, OCIFBSD_CERT_DIR,
        OCIFBSD_CERT_DIR, name,
        node_id);
    
    return (system(cmd));
}

/*
 * Check certificate expiry
 */
int
cert_check_expiry(const char *name, int warning_days, int critical_days)
{
    char cmd[256];
    FILE *fp;
    char buf[128];
    time_t expiry;
    int days_until;
    
    if (name == NULL)
        return (-1);
    
    /* Get certificate expiry date */
    snprintf(cmd, sizeof(cmd),
        "openssl x509 -in %s/%s.crt -noout -enddate 2>/dev/null | cut -d= -f2",
        OCIFBSD_CERT_DIR, name);
    
    fp = popen(cmd, "r");
    if (fp == NULL)
        return (-1);
    
    if (fgets(buf, sizeof(buf), fp) == NULL) {
        pclose(fp);
        return (-1);
    }
    pclose(fp);
    
    /* Parse date and calculate days until expiry */
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    strptime(buf, "%b %d %H:%M:%S %Y %Z", &tm);
    expiry = mktime(&tm);
    
    days_until = (expiry - time(NULL)) / (24 * 60 * 60);
    
    if (days_until <= critical_days)
        return (2);  /* Critical */
    if (days_until <= warning_days)
        return (1);  /* Warning */
    
    return (0);  /* OK */
}

/*
 * PAM conversation function
 */
static int
pam_conv_func(int num_msg, const struct pam_message **msg,
    struct pam_response **resp, void *appdata_ptr)
{
    struct pam_response *response;
    const char *password = (const char *)appdata_ptr;
    
    if (num_msg != 1 || msg == NULL || resp == NULL)
        return (PAM_CONV_ERR);
    
    response = calloc(num_msg, sizeof(struct pam_response));
    if (response == NULL)
        return (PAM_BUF_ERR);
    
    response[0].resp = strdup(password);
    response[0].resp_retcode = 0;
    
    *resp = response;
    
    return (PAM_SUCCESS);
}
