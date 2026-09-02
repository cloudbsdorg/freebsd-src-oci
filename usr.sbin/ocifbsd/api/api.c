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
 * REST API server implementation
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>

#include "api.h"
#include "../include/ocifbsd.h"

/* Global server state */
static struct api_server *server = NULL;
static struct api_config server_config;
static int api_initialized = 0;
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;

/* Rate limiting state */
static struct {
    char client_id[256];
    time_t window_start;
    int request_count;
} *rate_limit_state = NULL;
static int rate_limit_count = 0;
/* Upper bound on tracked clients so a spoofed-source flood can't grow the
 * rate-limit table without bound (expired slots are recycled first). */
#define API_RATE_LIMIT_MAX	4096
static pthread_mutex_t rate_limit_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Initialize API server
 */
int
api_init(struct api_config *config)
{
    int i;
    
    if (__sync_fetch_and_add(&api_initialized, 0) != 0)
        return (0);
    
    pthread_mutex_lock(&api_lock);
    
    if (config != NULL) {
        memcpy(&server_config, config, sizeof(struct api_config));
    } else {
        memset(&server_config, 0, sizeof(server_config));
        strlcpy(server_config.bind_address, "0.0.0.0", sizeof(server_config.bind_address));
        server_config.port = 8080;
        server_config.max_connections = 1000;
        server_config.request_timeout = 30;
        server_config.worker_threads = 4;
    }
    
    /* Allocate server structure */
    server = calloc(1, sizeof(struct api_server));
    if (server == NULL) {
        pthread_mutex_unlock(&api_lock);
        return (-1);
    }
    
    server->n_workers = server_config.worker_threads;
    server->workers = calloc(server->n_workers, sizeof(pthread_t));
    if (server->workers == NULL) {
        free(server);
        server = NULL;
        pthread_mutex_unlock(&api_lock);
        return (-1);
    }
    
    server->endpoints = NULL;
    server->running = false;
    
    pthread_mutex_init(&server->lock, NULL);
    
    /* Register default endpoints */
    api_register_endpoint("/health", HTTP_GET, api_health_check, "Health check");
    api_register_endpoint("/metrics", HTTP_GET, api_metrics_prometheus, "Prometheus metrics");
    
    __sync_fetch_and_add(&api_initialized, 1);
    pthread_mutex_unlock(&api_lock);
    
    return (0);
}

/*
 * Shutdown API server
 */
int
api_shutdown(void)
{
    if (__sync_fetch_and_add(&api_initialized, 0) == 0)
        return (0);
    
    api_stop();
    
    pthread_mutex_lock(&api_lock);
    
    if (server != NULL) {
        /* Destroy the mutex BEFORE freeing the struct it lives in — the
         * original order freed server and then dereferenced it (use-after-
         * free / NULL deref). */
        pthread_mutex_destroy(&server->lock);
        if (server->workers != NULL) {
            free(server->workers);
        }
        free(server);
        server = NULL;
    }

    __sync_fetch_and_add(&api_initialized, 0);
    pthread_mutex_unlock(&api_lock);
    
    return (0);
}

/*
 * Start API server
 */
int
api_start(void)
{
    struct sockaddr_in addr;
    int reuse = 1;
    
    if (server == NULL)
        return (-1);
    
    /* Create listening socket */
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0)
        return (-1);
    
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    /* Bind to address */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(server_config.port);
    inet_pton(AF_INET, server_config.bind_address, &addr.sin_addr);
    
    if (bind(server->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
        return (-1);
    }
    
    if (listen(server->listen_fd, server_config.max_connections) < 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
        return (-1);
    }
    
    server->running = true;
    
    /* Start worker threads */
    for (int i = 0; i < server->n_workers; i++) {
        pthread_create(&server->workers[i], NULL, api_worker_thread, NULL);
    }
    
    syslog(LOG_INFO, "API server listening on %s:%d",
        server_config.bind_address, server_config.port);
    
    return (0);
}

/*
 * Stop API server
 */
int
api_stop(void)
{
    if (server == NULL)
        return (-1);
    
    server->running = false;
    
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
        server->listen_fd = -1;
    }
    
    /* Wait for workers */
    for (int i = 0; i < server->n_workers; i++) {
        pthread_join(server->workers[i], NULL);
    }
    
    syslog(LOG_INFO, "API server stopped");
    
    return (0);
}

/*
 * Worker thread
 */
static void *
api_worker_thread(void *arg)
{
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    (void)arg;
    
    while (server != NULL && server->running) {
        client_fd = accept(server->listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (server->running)
                usleep(10000);
            continue;
        }
        
        /* Handle request */
        api_handle_client(client_fd, &client_addr);
        close(client_fd);
    }
    
    return (NULL);
}

/*
 * Handle client request
 */
static void
api_handle_client(int client_fd, struct sockaddr_in *client_addr)
{
    struct api_request *req;
    struct api_response *resp;
    char client_ip[64];
    
    inet_ntop(AF_INET, &client_addr->sin_addr, client_ip, sizeof(client_ip));
    
    /* Parse request */
    req = api_request_parse(client_fd);
    if (req == NULL) {
        /* Send 400 Bad Request */
        return;
    }
    
    strlcpy(req->remote_addr, client_ip, sizeof(req->remote_addr));
    
    /* Allocate response */
    resp = calloc(1, sizeof(struct api_response));
    if (resp == NULL) {
        api_request_free(req);
        return;
    }
    
    /* Check rate limit */
    if (api_rate_limit(client_ip, 60, 100) != 0) {
        api_json_error(resp, 429, "Rate limit exceeded");
        api_response_send(resp, client_fd);
        api_response_free(resp);
        api_request_free(req);
        return;
    }
    
    /* CORS preflight */
    if (strcmp(req->method, "OPTIONS") == 0) {
        api_cors_preflight(req, resp);
        api_response_send(resp, client_fd);
        api_response_free(resp);
        api_request_free(req);
        return;
    }
    
    /* Auth middleware */
    if (api_auth_middleware(req, resp) != 0) {
        api_response_send(resp, client_fd);
        api_response_free(resp);
        api_request_free(req);
        return;
    }
    
    /* Route request */
    api_route_request(req, resp);
    
    /* Add CORS headers */
    api_add_cors_headers(resp);
    
    /* Send response */
    api_response_send(resp, client_fd);
    
    api_response_free(resp);
    api_request_free(req);
}

/*
 * Route request to handler
 */
static void
api_route_request(struct api_request *req, struct api_response *resp)
{
    struct api_endpoint *ep;
    
    pthread_mutex_lock(&server->lock);
    for (ep = server->endpoints; ep != NULL; ep = ep->next) {
        /* Match the request method against the endpoint's method. The old
         * `ep->method == HTTP_GET || ...` short-circuited to true for every
         * GET-registered route, so it also answered POST/PUT/PATCH/DELETE. */
        if (strcmp(req->path, ep->path) == 0 &&
            strcmp(req->method,
            ep->method == HTTP_GET ? "GET" :
            ep->method == HTTP_POST ? "POST" :
            ep->method == HTTP_PUT ? "PUT" :
            ep->method == HTTP_PATCH ? "PATCH" : "DELETE") == 0) {
            pthread_mutex_unlock(&server->lock);
            ep->handler(req, resp);
            return;
        }
    }
    pthread_mutex_unlock(&server->lock);
    
    /* No handler found */
    api_json_error(resp, 404, "Not found");
}

/*
 * Register endpoint
 */
int
api_register_endpoint(const char *path, int method, api_handler handler, const char *description)
{
    struct api_endpoint *ep;
    
    if (server == NULL || path == NULL || handler == NULL)
        return (-1);
    
    ep = calloc(1, sizeof(struct api_endpoint));
    if (ep == NULL)
        return (-1);
    
    strlcpy(ep->path, path, sizeof(ep->path));
    ep->method = method;
    ep->handler = handler;
    if (description != NULL)
        strlcpy(ep->description, description, sizeof(ep->description));
    
    pthread_mutex_lock(&server->lock);
    ep->next = server->endpoints;
    server->endpoints = ep;
    pthread_mutex_unlock(&server->lock);
    
    return (0);
}

/*
 * Parse HTTP request
 */
struct api_request *
api_request_parse(int client_fd)
{
    struct api_request *req;
    char buf[8192];
    ssize_t n;
    char *method, *path, *version;
    char *line;
    char *saveptr;
    
    req = calloc(1, sizeof(struct api_request));
    if (req == NULL)
        return (NULL);
    
    /* Read request line */
    n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) {
        free(req);
        return (NULL);
    }
    buf[n] = '\0';
    
    /* Parse request line */
    line = strtok_r(buf, "\r\n", &saveptr);
    if (line == NULL) {
        free(req);
        return (NULL);
    }
    
    method = strtok(line, " ");
    path = strtok(NULL, " ");
    version = strtok(NULL, " ");
    
    if (method == NULL || path == NULL) {
        free(req);
        return (NULL);
    }
    
    strlcpy(req->method, method, sizeof(req->method));
    strlcpy(req->path, path, sizeof(req->path));
    
    /* Parse query string */
    char *q = strchr(req->path, '?');
    if (q != NULL) {
        *q = '\0';
        strlcpy(req->query, q + 1, sizeof(req->query));
    }
    
    /* Read headers and body */
    char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start != NULL) {
        body_start += 4;
        size_t header_len = body_start - buf;
        req->body = strdup(body_start);
        req->body_len = n - header_len;
        
        /* Parse headers */
        req->headers = malloc(header_len + 1);
        memcpy(req->headers, buf, header_len);
        req->headers[header_len] = '\0';
    }
    
    return (req);
}

/*
 * Free request
 */
int
api_request_free(struct api_request *req)
{
    if (req == NULL)
        return (0);
    
    if (req->body != NULL)
        free(req->body);
    if (req->headers != NULL)
        free(req->headers);
    
    free(req);
    return (0);
}

/*
 * Initialize response
 */
int
api_response_init(struct api_response *resp)
{
    if (resp == NULL)
        return (-1);
    
    memset(resp, 0, sizeof(struct api_response));
    resp->status_code = 200;
    strlcpy(resp->status_message, "OK", sizeof(resp->status_message));
    strlcpy(resp->content_type, "application/json", sizeof(resp->content_type));
    
    return (0);
}

/*
 * Set response status
 */
int
api_response_set_status(struct api_response *resp, int code, const char *message)
{
    if (resp == NULL)
        return (-1);
    
    resp->status_code = code;
    if (message != NULL)
        strlcpy(resp->status_message, message, sizeof(resp->status_message));
    
    return (0);
}

/*
 * Set response body
 */
int
api_response_set_body(struct api_response *resp, const void *body, size_t len, const char *content_type)
{
    if (resp == NULL)
        return (-1);
    
    if (body != NULL && len > 0) {
        resp->body = malloc(len);
        if (resp->body == NULL)
            return (-1);
        memcpy(resp->body, body, len);
        resp->body_len = len;
    }
    
    if (content_type != NULL)
        strlcpy(resp->content_type, content_type, sizeof(resp->content_type));
    
    return (0);
}

/*
 * Add header to response
 */
int
api_response_add_header(struct api_response *resp, const char *name, const char *value)
{
    char header[512];
    char *new_headers;
    size_t new_len;
    
    if (resp == NULL || name == NULL || value == NULL)
        return (-1);
    
    snprintf(header, sizeof(header), "%s: %s\r\n", name, value);
    
    if (resp->headers == NULL) {
        resp->headers = strdup(header);
    } else {
        new_len = strlen(resp->headers) + strlen(header) + 1;
        new_headers = malloc(new_len);
        if (new_headers == NULL)
            return (-1);
        strlcpy(new_headers, resp->headers, new_len);
        strlcat(new_headers, header, new_len);
        free(resp->headers);
        resp->headers = new_headers;
    }
    
    return (0);
}

/*
 * Send response to client
 */
int
api_response_send(struct api_response *resp, int client_fd)
{
    char header[1024];
    ssize_t sent;
    
    if (resp == NULL)
        return (-1);
    
    /* Build HTTP response header */
    snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "%s"
        "\r\n",
        resp->status_code,
        resp->status_message,
        resp->content_type,
        resp->body_len,
        resp->headers ? resp->headers : "");
    
    /* Send header */
    sent = send(client_fd, header, strlen(header), 0);
    if (sent < 0)
        return (-1);
    
    /* Send body */
    if (resp->body != NULL && resp->body_len > 0) {
        sent = send(client_fd, resp->body, resp->body_len, 0);
        if (sent < 0)
            return (-1);
    }
    
    return (0);
}

/*
 * Free response
 */
int
api_response_free(struct api_response *resp)
{
    if (resp == NULL)
        return (0);
    
    if (resp->body != NULL)
        free(resp->body);
    if (resp->headers != NULL)
        free(resp->headers);
    
    free(resp);
    return (0);
}

/*
 * JSON error response
 */
int
api_json_error(struct api_response *resp, int code, const char *error)
{
    char body[1024];
    
    if (resp == NULL || error == NULL)
        return (-1);
    
    api_response_init(resp);
    resp->status_code = code;
    
    const char *msg;
    switch (code) {
        case 400: msg = "Bad Request"; break;
        case 401: msg = "Unauthorized"; break;
        case 403: msg = "Forbidden"; break;
        case 404: msg = "Not Found"; break;
        case 429: msg = "Too Many Requests"; break;
        case 500: msg = "Internal Server Error"; break;
        default: msg = "Error"; break;
    }
    
    strlcpy(resp->status_message, msg, sizeof(resp->status_message));
    
    /* Escape the caller-supplied message so it cannot break the JSON body. */
    {
        char eerr[1024];

        snprintf(body, sizeof(body), "{\"error\": \"%s\"}",
            ocifbsd_json_escape(error, eerr, sizeof(eerr)));
    }
    return api_response_set_body(resp, body, strlen(body), "application/json");
}

/*
 * JSON ok response
 */
int
api_json_ok(struct api_response *resp, const char *data)
{
    if (resp == NULL)
        return (-1);
    
    api_response_init(resp);
    return api_response_set_body(resp, data, strlen(data), "application/json");
}

/*
 * Auth middleware
 */
int
api_auth_middleware(struct api_request *req, struct api_response *resp)
{
    char auth_header[512];
    char *token;
    
    if (req == NULL || resp == NULL)
        return (-1);
    
    /* Extract Authorization header */
    char *headers = req->headers;
    if (headers == NULL)
        return (0);  /* No auth required */
    
    char *auth_line = strstr(headers, "Authorization:");
    if (auth_line == NULL)
        return (0);
    
    /* Skip "Authorization:" */
    token = auth_line + 14;
    while (*token == ' ')
        token++;
    
    /* Extract token */
    char *end = strchr(token, '\r');
    if (end != NULL)
        *end = '\0';
    
    /*
     * Token validation is not wired up here: the validator lives in
     * security-daemon/auth.c (auth_token_validate) and is not exposed as a
     * library this module can link against. Rather than the old behavior —
     * accepting ANY token, a fail-OPEN security hole — fail CLOSED: a caller
     * that presents credentials we cannot verify must be denied, never
     * treated as authenticated. Requests with no Authorization header remain
     * anonymous (handled above) so unauthenticated/public endpoints still
     * work, but a presented-but-unverifiable token is a 401.
     *
     * To enable real token validation, extract security-daemon/auth.c into a
     * linkable library and call auth_token_validate(token, user) here,
     * setting req->user on success.
     */
    (void)token;
    resp->status_code = 401;
    return (-1);
}

/*
 * Rate limiting
 */
int
api_rate_limit(const char *client_id, int window_seconds, int max_requests)
{
    time_t now;
    int i;
    int reuse = -1;	/* first expired slot we can recycle */

    if (client_id == NULL)
        return (0);

    time(&now);

    pthread_mutex_lock(&rate_limit_lock);

    for (i = 0; i < rate_limit_count; i++) {
        if (strcmp(rate_limit_state[i].client_id, client_id) == 0) {
            if (now - rate_limit_state[i].window_start > window_seconds) {
                rate_limit_state[i].window_start = now;
                rate_limit_state[i].request_count = 1;
            } else {
                rate_limit_state[i].request_count++;
                if (rate_limit_state[i].request_count > max_requests) {
                    pthread_mutex_unlock(&rate_limit_lock);
                    return (-1);
                }
            }
            pthread_mutex_unlock(&rate_limit_lock);
            return (0);
        }
        /* Remember an expired slot to recycle rather than growing. */
        if (reuse < 0 &&
            now - rate_limit_state[i].window_start > window_seconds)
            reuse = i;
    }

    /*
     * New client. Recycle an expired slot if one exists; otherwise cap the
     * table so a flood of distinct/spoofed source addresses cannot grow it
     * without bound (memory-exhaustion DoS). At the cap with nothing to
     * recycle, fail open (allow) rather than reject legitimate traffic.
     */
    if (reuse >= 0) {
        i = reuse;
    } else if (rate_limit_count < API_RATE_LIMIT_MAX) {
        void *_new = realloc(rate_limit_state,
            (rate_limit_count + 1) * sizeof(rate_limit_state[0]));
        if (_new == NULL) {
            pthread_mutex_unlock(&rate_limit_lock);
            return (0);  /* Allow on allocation failure */
        }
        rate_limit_state = _new;
        i = rate_limit_count++;
    } else {
        pthread_mutex_unlock(&rate_limit_lock);
        return (0);  /* table full, no expired slot: fail open */
    }

    strlcpy(rate_limit_state[i].client_id, client_id,
        sizeof(rate_limit_state[i].client_id));
    rate_limit_state[i].window_start = now;
    rate_limit_state[i].request_count = 1;

    pthread_mutex_unlock(&rate_limit_lock);

    return (0);
}

/*
 * CORS preflight
 */
int
api_cors_preflight(struct api_request *req, struct api_response *resp)
{
    char *origin;
    
    if (req == NULL || resp == NULL)
        return (-1);
    
    api_response_init(resp);
    resp->status_code = 204;
    strlcpy(resp->status_message, "No Content", sizeof(resp->status_message));
    
    /* Extract Origin header */
    if (req->headers != NULL) {
        origin = strstr(req->headers, "Origin:");
        if (origin != NULL) {
            origin += 7;
            while (*origin == ' ')
                origin++;
            char *end = strchr(origin, '\r');
            if (end != NULL)
                *end = '\0';
            
            api_response_add_header(resp, "Access-Control-Allow-Origin", origin);
        }
    }
    
    api_response_add_header(resp, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    api_response_add_header(resp, "Access-Control-Allow-Headers", "Authorization, Content-Type");
    api_response_add_header(resp, "Access-Control-Max-Age", "86400");
    
    return (0);
}

/*
 * Add CORS headers
 */
int
api_add_cors_headers(struct api_response *resp)
{
    if (resp == NULL)
        return (-1);
    
    api_response_add_header(resp, "Access-Control-Allow-Origin", "*");
    api_response_add_header(resp, "X-Content-Type-Options", "nosniff");
    api_response_add_header(resp, "X-Frame-Options", "DENY");
    
    return (0);
}

/*
 * Health check endpoint
 */
int
api_health_check(struct api_request *req, struct api_response *resp)
{
    char body[256];
    
    (void)req;
    
    api_response_init(resp);
    
    snprintf(body, sizeof(body),
        "{\"status\": \"healthy\", \"version\": \"%s\", \"timestamp\": %ld}",
        OCIFBSD_VERSION,
        (long)time(NULL));
    
    return api_response_set_body(resp, body, strlen(body), "application/json");
}

/*
 * Prometheus metrics endpoint
 */
int
api_metrics_prometheus(struct api_request *req, struct api_response *resp)
{
    char *metrics;
    
    (void)req;
    
    api_response_init(resp);
    
    /* Get metrics in Prometheus format */
    metrics = metrics_serialize_prometheus();
    if (metrics == NULL)
        return api_json_error(resp, 500, "Failed to collect metrics");
    
    api_response_set_body(resp, metrics, strlen(metrics), "text/plain; version=0.0.4");
    free(metrics);
    
    return (0);
}
