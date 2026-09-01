/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Tests for logd's HTTP log forwarding (logd_http_post), exercised against an
 * in-process one-shot HTTP server so no external service or python is needed.
 */

#include <sys/param.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <atf-c.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The function under test lives in forward.c; pull it in directly. */
#include "logd/forward.c"

struct sink {
	int	listen_fd;
	int	port;
	char	body[4096];	/* captured request body */
	char	ctype[256];	/* captured Content-Type */
};

/* One-shot HTTP server: accept a single connection, read the request, record
 * its Content-Type and body, and reply 200. */
static void *
sink_thread(void *arg)
{
	struct sink *s = arg;
	int fd;
	char buf[8192];
	ssize_t n, total = 0;
	const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
	    "Connection: close\r\n\r\nok";

	fd = accept(s->listen_fd, NULL, NULL);
	if (fd < 0)
		return (NULL);
	/* Read until headers+body arrive (small payloads: one read suffices,
	 * but loop briefly to be safe). */
	while (total < (ssize_t)sizeof(buf) - 1 &&
	    (n = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0) {
		total += n;
		buf[total] = '\0';
		if (strstr(buf, "\r\n\r\n") != NULL &&
		    strstr(buf, "\r\n\r\n") + 4 <= buf + total)
			break;
	}
	buf[total > 0 ? total : 0] = '\0';
	{
		char *ct = strcasestr(buf, "Content-Type:");
		char *bodystart = strstr(buf, "\r\n\r\n");

		if (ct != NULL) {
			ct += strlen("Content-Type:");
			while (*ct == ' ')
				ct++;
			sscanf(ct, "%255[^\r\n]", s->ctype);
		}
		if (bodystart != NULL)
			snprintf(s->body, sizeof(s->body), "%s", bodystart + 4);
	}
	(void)write(fd, resp, strlen(resp));
	close(fd);
	return (NULL);
}

/* Start the sink on an ephemeral 127.0.0.1 port; returns 0 on success. */
static int
sink_start(struct sink *s, pthread_t *tid)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof(sa);

	memset(s, 0, sizeof(*s));
	s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (s->listen_fd < 0)
		return (-1);
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;	/* kernel picks a free port */
	if (bind(s->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(s->listen_fd, 1) != 0 ||
	    getsockname(s->listen_fd, (struct sockaddr *)&sa, &sl) != 0) {
		close(s->listen_fd);
		return (-1);
	}
	s->port = ntohs(sa.sin_port);
	if (pthread_create(tid, NULL, sink_thread, s) != 0) {
		close(s->listen_fd);
		return (-1);
	}
	return (0);
}

ATF_TC(post_delivers_body);
ATF_TC_HEAD(post_delivers_body, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "logd_http_post delivers the body to a 2xx endpoint");
}
ATF_TC_BODY(post_delivers_body, tc)
{
	struct sink s;
	pthread_t tid;
	char url[64];
	int rc;

	ATF_REQUIRE(sink_start(&s, &tid) == 0);
	snprintf(url, sizeof(url), "http://127.0.0.1:%d/webhook", s.port);
	rc = logd_http_post(url, "{\"msg\":\"hello\"}", "application/json", NULL);
	pthread_join(tid, NULL);
	close(s.listen_fd);

	ATF_CHECK_EQ_MSG(0, rc, "expected 2xx success, got %d", rc);
	ATF_CHECK_STREQ("{\"msg\":\"hello\"}", s.body);
	ATF_CHECK_STREQ("application/json", s.ctype);
}

ATF_TC(post_unreachable_fails);
ATF_TC_HEAD(post_unreachable_fails, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "logd_http_post returns failure for an unreachable endpoint");
}
ATF_TC_BODY(post_unreachable_fails, tc)
{
	/* Port 1 on loopback is not listening: connect must fail. */
	int rc = logd_http_post("http://127.0.0.1:1/x", "{}",
	    "application/json", NULL);

	ATF_CHECK_MSG(rc != 0, "unreachable endpoint should fail, got %d", rc);
}

ATF_TC(post_rejects_bad_args);
ATF_TC_HEAD(post_rejects_bad_args, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "logd_http_post rejects NULL/empty url or NULL body");
}
ATF_TC_BODY(post_rejects_bad_args, tc)
{
	ATF_CHECK(logd_http_post(NULL, "{}", "application/json", NULL) != 0);
	ATF_CHECK(logd_http_post("", "{}", "application/json", NULL) != 0);
	ATF_CHECK(logd_http_post("http://127.0.0.1:1/x", NULL,
	    "application/json", NULL) != 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, post_delivers_body);
	ATF_TP_ADD_TC(tp, post_unreachable_fails);
	ATF_TP_ADD_TC(tp, post_rejects_bad_args);

	return (atf_no_error());
}
