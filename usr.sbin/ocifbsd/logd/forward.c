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
 * Log forwarding implementation
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/queue.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <curl/curl.h>

#include "logd.h"

/*
 * HTTP POST a log payload to a forwarding endpoint. Used by the fluentd,
 * Elasticsearch, Splunk, and custom-webhook forwarders in logd.c. Returns 0
 * only on a 2xx response; -1 on any transport error or non-2xx status so the
 * caller can count the failure and retry. content_type defaults to
 * application/json; extra_header (e.g. an "Authorization: ..." line) is added
 * when non-NULL. The transfer is bounded by a connect and total timeout so a
 * stuck endpoint cannot wedge the forwarder thread.
 */
/* Discard a response body — never let it reach stdout, and cap it so a hostile
 * or huge reply cannot stall the forwarder thread. */
static size_t
logd_discard_body(char *ptr, size_t size, size_t nmemb, void *userp)
{
	size_t n = size * nmemb;
	size_t *seen = userp;

	(void)ptr;
	*seen += n;
	return (*seen > (1u << 20) ? 0 : n);    /* abort past 1 MiB */
}

/* Append to a curl header list without leaking the list on OOM. */
static int
hdr_add(struct curl_slist **list, const char *s)
{
	struct curl_slist *n = curl_slist_append(*list, s);

	if (n == NULL)
		return (-1);
	*list = n;
	return (0);
}

int
logd_http_post(const char *url, const char *body, const char *content_type,
    const char *extra_header)
{
	CURL *curl;
	CURLcode rc;
	struct curl_slist *hdrs = NULL;
	char ctype[128];
	long code = 0;
	size_t discarded = 0;
	int ret = -1;

	if (url == NULL || url[0] == '\0' || body == NULL)
		return (-1);
	/*
	 * Only http/https. logd runs as root, so an attacker-influenced
	 * endpoint like file:///etc/master.passwd or dict:// would otherwise
	 * turn this POST into arbitrary local-file read / SSRF via libcurl's
	 * other protocol handlers.
	 */
	if (strncasecmp(url, "http://", 7) != 0 &&
	    strncasecmp(url, "https://", 8) != 0)
		return (-1);

	curl = curl_easy_init();
	if (curl == NULL)
		return (-1);

	snprintf(ctype, sizeof(ctype), "Content-Type: %s",
	    content_type != NULL ? content_type : "application/json");
	if (hdr_add(&hdrs, ctype) != 0 ||
	    (extra_header != NULL && extra_header[0] != '\0' &&
	     hdr_add(&hdrs, extra_header) != 0) ||
	    /* Suppress the default "Expect: 100-continue" round trip. */
	    hdr_add(&hdrs, "Expect:") != 0) {
		curl_slist_free_all(hdrs);
		curl_easy_cleanup(curl);
		return (-1);
	}

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
	/* Defense in depth: constrain the transfer (and any redirects) to
	 * http/https regardless of the URL check above. */
#if LIBCURL_VERSION_NUM >= 0x075500   /* 7.85.0: the *_STR options */
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
	    (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
	    (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
	/* Never emit the response body to stdout; discard it. */
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, logd_discard_body);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &discarded);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "ocifbsd-logd/1.0");

	rc = curl_easy_perform(curl);
	if (rc == CURLE_OK) {
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		if (code >= 200 && code < 300)
			ret = 0;
	}
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	return (ret);
}
