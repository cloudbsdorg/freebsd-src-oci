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
 * REST API server header
 */

#ifndef _OCIFBSD_API_H
#define _OCIFBSD_API_H

#include <sys/param.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <event.h>

/* API configuration */
struct api_config {
	char bind_address[64];
	uint16_t port;
	bool enable_tls;
	char *tls_cert;
	char *tls_key;
	int max_connections;
	int request_timeout;
	int worker_threads;
	char *unix_socket;
	bool enable_metrics;
};

/* HTTP methods */
#define HTTP_GET     0
#define HTTP_POST    1
#define HTTP_PUT     2
#define HTTP_PATCH   3
#define HTTP_DELETE  4

/* API response */
struct api_response {
	int status_code;
	char status_message[64];
	char *body;
	size_t body_len;
	char content_type[64];
	char *headers;
};

/* API endpoint handler */
typedef int (*api_handler)(struct api_request *req, struct api_response *resp);

struct api_endpoint {
	char path[256];
	int method;                /* HTTP_* */
	api_handler handler;
	char description[512];
	struct api_endpoint *next;
};

/* API request */
struct api_request {
	char method[16];
	char path[512];
	char query[1024];
	char *body;
	size_t body_len;
	char remote_addr[64];
	char *headers;
	void *context;             /* auth context */
};

/* API server state */
struct api_server {
	int listen_fd;
	struct event_base *evbase;
	pthread_t *workers;
	int n_workers;
	bool running;
	struct api_endpoint *endpoints;
	pthread_mutex_t lock;
};

/* API initialization */
int api_init(struct api_config *config);
int api_shutdown(void);

/* Server control */
int api_start(void);
int api_stop(void);

/* Endpoint registration */
int api_register_endpoint(const char *path, int method, api_handler handler, const char *description);
int api_unregister_endpoint(const char *path, int method);

/* Request parsing */
struct api_request *api_request_parse(int client_fd);
int api_request_free(struct api_request *req);

/* Response building */
int api_response_init(struct api_response *resp);
int api_response_set_status(struct api_response *resp, int code, const char *message);
int api_response_set_body(struct api_response *resp, const void *body, size_t len, const char *content_type);
int api_response_add_header(struct api_response *resp, const char *name, const char *value);
int api_response_send(struct api_response *resp, int client_fd);
int api_response_free(struct api_response *resp);

/* JSON helpers */
int api_json_error(struct api_response *resp, int code, const char *error);
int api_json_ok(struct api_response *resp, const char *data);

/* Authentication middleware */
int api_auth_middleware(struct api_request *req, struct api_response *resp);

/* Rate limiting */
int api_rate_limit(const char *client_id, int window_seconds, int max_requests);

/* CORS */
int api_cors_preflight(struct api_request *req, struct api_response *resp);
int api_add_cors_headers(struct api_response *resp);

/* Health check endpoint */
int api_health_check(struct api_request *req, struct api_response *resp);

/* Metrics endpoint */
int api_metrics_prometheus(struct api_request *req, struct api_response *resp);

#endif /* _OCIFBSD_API_H */
