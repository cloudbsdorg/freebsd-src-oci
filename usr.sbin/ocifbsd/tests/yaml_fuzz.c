/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Robustness harness for convert/yaml.c.
 *
 * A manifest is untrusted input, so the parser must never crash, read out of
 * bounds, or leak on ANY byte sequence -- it may only succeed or return an
 * error. This drives it with three input families:
 *
 *   1. random bytes (structure-blind)
 *   2. random mutations of a real manifest (structure-aware: these keep enough
 *      shape to reach deep parser states that random bytes never will)
 *   3. adversarial hand-written shapes (deep nesting, huge lines, unbalanced
 *      quotes, lone dashes, CRLF, NUL-adjacent truncation)
 *
 * Every parse is followed by lookups and a free, so it is meaningful under
 * AddressSanitizer. Run with a repeat count to watch RSS for leaks:
 * FreeBSD's ASan has no leak detector, so growth over many iterations is the
 * available signal.
 *
 * Usage: yaml_fuzz [iterations] [seed]
 */

#include <sys/types.h>
#include <sys/resource.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "yaml.h"

static const char *seed_doc =
"apiVersion: apps/v1\n"
"kind: Deployment\n"
"metadata:\n"
"  name: web\n"
"  namespace: prod\n"
"spec:\n"
"  replicas: 3\n"
"  template:\n"
"    spec:\n"
"      containers:\n"
"        - name: nginx\n"
"          image: nginx:1.25\n"
"          ports:\n"
"            - containerPort: 80\n";

static const char *paths[] = {
	"metadata.name", "metadata.namespace", "spec.replicas", "kind",
	"spec.template.spec.containers.0.image",
	"spec.template.spec.containers.0.ports.0.containerPort",
	"spec.template.spec.containers.5.name", "", "a.b.c.d.e.f",
	"0", "999999", "spec.template.spec.containers.0.ports.0",
};

/* xorshift: deterministic, so a failing seed reproduces exactly. */
static uint32_t rng_state = 1;

static uint32_t
rng(void)
{

	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return (rng_state);
}

/* Parse, interrogate, free. Any crash is the harness failing. */
static void
exercise(const char *text)
{
	struct yaml_node *d;
	size_t i;

	d = yaml_parse(text);
	if (d == NULL)
		return;			/* a clean rejection is a valid result */
	for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		long v;
		const char *why;

		(void)yaml_get_scalar(d, paths[i]);
		(void)yaml_get_int(d, paths[i], 0, 65535, &v, &why);
	}
	(void)yaml_find_scalar(d, "name");
	(void)yaml_find_scalar(d, "nothing-here");
	yaml_free(d);
}

static void
fuzz_random_bytes(void)
{
	char buf[512];
	size_t n, i;

	n = rng() % sizeof(buf);
	for (i = 0; i < n; i++) {
		/* Bias toward YAML-significant bytes to reach real states. */
		switch (rng() % 8) {
		case 0: buf[i] = ':'; break;
		case 1: buf[i] = ' '; break;
		case 2: buf[i] = '\n'; break;
		case 3: buf[i] = '-'; break;
		case 4: buf[i] = '#'; break;
		case 5: buf[i] = (rng() % 2) ? '"' : '\''; break;
		default: buf[i] = (char)(32 + rng() % 95); break;
		}
	}
	buf[n] = '\0';
	exercise(buf);
}

static void
fuzz_mutate(void)
{
	char buf[2048];
	size_t len, nmut, i;

	strlcpy(buf, seed_doc, sizeof(buf));
	len = strlen(buf);
	nmut = 1 + rng() % 8;
	for (i = 0; i < nmut; i++) {
		size_t pos = rng() % len;

		switch (rng() % 4) {
		case 0:				/* substitute */
			buf[pos] = (char)(32 + rng() % 95);
			break;
		case 1:				/* truncate */
			buf[pos] = '\0';
			len = pos;
			if (len == 0)
				return;
			break;
		case 2:				/* delete a byte */
			memmove(buf + pos, buf + pos + 1, len - pos);
			len--;
			if (len == 0)
				return;
			break;
		case 3:				/* inject a significant byte */
			if (len + 1 < sizeof(buf)) {
				memmove(buf + pos + 1, buf + pos,
				    len - pos + 1);
				buf[pos] = ":\n- #\"'\t"[rng() % 8];
				len++;
			}
			break;
		}
	}
	exercise(buf);
}

static void
adversarial(void)
{
	char big[8192];
	size_t i;

	exercise("a:\n");
	exercise("- \n");
	exercise("-\n-\n-\n");
	exercise("a: 'unterminated\n");
	exercise("a: \"unterminated\n");
	exercise("'unterminated key: v\n");
	exercise("a:\r\n  b: c\r\n");
	exercise("a: b\n\n\n\n");
	exercise(":\n");
	exercise("a::::\n");
	exercise("---\n---\n---\n");
	exercise("#\n#\n#\n");
	exercise("a: b # # # #\n");

	/* Nesting far deeper than the parser's stack: must reject, not smash. */
	{
		char deep[4096];
		size_t o = 0;

		for (i = 0; i < 200 && o + 8 < sizeof(deep); i++) {
			int n = snprintf(deep + o, sizeof(deep) - o,
			    "%*sk%zu:\n", (int)(i * 2), "", i);
			if (n < 0 || (size_t)n >= sizeof(deep) - o)
				break;
			o += (size_t)n;
		}
		deep[o] = '\0';
		exercise(deep);
	}

	/* One enormous line, and an enormous indent. */
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';
	exercise(big);
	big[0] = 'a'; big[1] = ':'; big[2] = ' ';
	exercise(big);
	memset(big, ' ', sizeof(big) - 1);
	strlcpy(big + sizeof(big) - 8, "a: b\n", 8);
	exercise(big);

	/* Deeply nested sequences. */
	exercise("a:\n- b:\n  - c:\n    - d:\n      - e: f\n");
}

int
main(int argc, char **argv)
{
	long iters = 20000;
	struct rusage ru;
	long rss_start, rss_end;
	long i;

	if (argc > 1)
		iters = atol(argv[1]);
	if (argc > 2)
		rng_state = (uint32_t)atol(argv[2]);
	if (rng_state == 0)
		rng_state = 1;

	adversarial();

	getrusage(RUSAGE_SELF, &ru);
	rss_start = ru.ru_maxrss;

	for (i = 0; i < iters; i++) {
		fuzz_random_bytes();
		fuzz_mutate();
		if ((i % 500) == 0)
			exercise(seed_doc);
	}

	getrusage(RUSAGE_SELF, &ru);
	rss_end = ru.ru_maxrss;

	printf("yaml_fuzz: %ld iterations survived (seed %u)\n", iters,
	    rng_state);
	printf("yaml_fuzz: peak RSS %ld -> %ld KiB (delta %ld)\n",
	    rss_start, rss_end, rss_end - rss_start);
	/*
	 * Each iteration parses and frees. A real leak shows as RSS climbing
	 * with the iteration count; a bounded delta is allocator behaviour.
	 */
	if (rss_end - rss_start > 65536) {
		printf("yaml_fuzz: FAILED - RSS grew by more than 64 MiB, "
		    "which indicates a leak\n");
		return (1);
	}
	printf("yaml_fuzz: PASSED\n");
	return (0);
}
