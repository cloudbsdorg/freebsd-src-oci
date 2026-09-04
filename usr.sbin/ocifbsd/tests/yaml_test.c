/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests for the indent-aware YAML reader (convert/yaml.c).
 *
 * Covers the cases the old substring scanner got wrong -- a key matched at the
 * wrong nesting level, a key matched inside another key's name, a key matched
 * inside a comment or a quoted value, a value truncated at a colon or a '#'
 * that was not a comment -- plus the emission escaping that stops a manifest
 * value from injecting structure into the generated document.
 */

#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yaml.h"

static int tests_run, tests_failed;

static void
check_str(const char *what, const char *got, const char *want)
{

	tests_run++;
	if (got == NULL && want == NULL)
		return;
	if (got != NULL && want != NULL && strcmp(got, want) == 0)
		return;
	tests_failed++;
	printf("FAIL %s: got %s%s%s, want %s%s%s\n", what,
	    got ? "\"" : "", got ? got : "(null)", got ? "\"" : "",
	    want ? "\"" : "", want ? want : "(null)", want ? "\"" : "");
}

static void
check_true(const char *what, int cond)
{

	tests_run++;
	if (!cond) {
		tests_failed++;
		printf("FAIL %s\n", what);
	}
}

/* A realistic Ensemble-style Deployment: 'name' appears at three depths. */
static const char *manifest =
"apiVersion: apps/v1\n"
"kind: Deployment\n"
"metadata:\n"
"  name: web-frontend\n"
"  namespace: production\n"
"  labels:\n"
"    app: web\n"
"spec:\n"
"  replicas: 3\n"
"  selector:\n"
"    matchLabels:\n"
"      app: web\n"
"  template:\n"
"    metadata:\n"
"      labels:\n"
"        app: web\n"
"    spec:\n"
"      hostname: not-the-name\n"
"      containers:\n"
"        - name: nginx\n"
"          image: docker.io/library/nginx:1.25\n"
"          ports:\n"
"            - containerPort: 80\n"
"              protocol: TCP\n"
"        - name: sidecar\n"
"          image: docker.io/library/busybox:latest\n";

static void
test_paths(void)
{
	struct yaml_node *d;

	d = yaml_parse(manifest);
	check_true("manifest parses", d != NULL);
	if (d == NULL)
		return;

	/* The whole point: each 'name' is addressable by where it lives. */
	check_str("metadata.name", yaml_get_scalar(d, "metadata.name"),
	    "web-frontend");
	check_str("metadata.namespace",
	    yaml_get_scalar(d, "metadata.namespace"), "production");
	check_str("container 0 name",
	    yaml_get_scalar(d, "spec.template.spec.containers.0.name"),
	    "nginx");
	check_str("container 1 name",
	    yaml_get_scalar(d, "spec.template.spec.containers.1.name"),
	    "sidecar");
	check_str("replicas", yaml_get_scalar(d, "spec.replicas"), "3");

	/* A colon inside a value must not split the scalar. */
	check_str("image keeps its tag",
	    yaml_get_scalar(d, "spec.template.spec.containers.0.image"),
	    "docker.io/library/nginx:1.25");

	/* Nested sequence inside a sequence item. */
	check_str("containerPort",
	    yaml_get_scalar(d,
	    "spec.template.spec.containers.0.ports.0.containerPort"), "80");
	check_str("protocol",
	    yaml_get_scalar(d,
	    "spec.template.spec.containers.0.ports.0.protocol"), "TCP");

	/* 'name' must not be found inside 'hostname'. */
	check_str("hostname is its own key",
	    yaml_get_scalar(d, "spec.template.spec.hostname"),
	    "not-the-name");

	/* Absent paths are absent, not guesses. */
	check_str("missing path", yaml_get_scalar(d, "metadata.nope"), NULL);
	check_str("index past end",
	    yaml_get_scalar(d, "spec.template.spec.containers.9.name"), NULL);
	check_str("kind", yaml_get_scalar(d, "kind"), "Deployment");

	yaml_free(d);
}

static void
test_comments_and_quoting(void)
{
	struct yaml_node *d;
	static const char *src =
	    "# name: commented-out\n"
	    "a: plain value   # trailing comment\n"
	    "b: \"quoted: with colon\"\n"
	    "c: 'single #not-a-comment'\n"
	    "d: value#not-a-comment\n"
	    "e: \"\"\n"
	    "f: 'it''s escaped'\n"
	    "g:\n"
	    "  h: nested\n";

	d = yaml_parse(src);
	check_true("comment doc parses", d != NULL);
	if (d == NULL)
		return;

	/* A commented-out key must not be visible at all. */
	check_str("comment line ignored", yaml_get_scalar(d, "name"), NULL);
	check_str("trailing comment stripped", yaml_get_scalar(d, "a"),
	    "plain value");
	check_str("colon inside quotes", yaml_get_scalar(d, "b"),
	    "quoted: with colon");
	check_str("hash inside quotes", yaml_get_scalar(d, "c"),
	    "single #not-a-comment");
	/* '#' without preceding space is part of the value, not a comment. */
	check_str("hash without space", yaml_get_scalar(d, "d"),
	    "value#not-a-comment");
	check_str("empty quoted value", yaml_get_scalar(d, "e"), "");
	check_str("doubled single quote", yaml_get_scalar(d, "f"),
	    "it's escaped");
	check_str("nested under empty key", yaml_get_scalar(d, "g.h"),
	    "nested");

	yaml_free(d);
}

static void
test_sequence_styles(void)
{
	struct yaml_node *d;
	/* Dash at the parent's own indent -- the other legal block style. */
	static const char *src =
	    "items:\n"
	    "- name: first\n"
	    "  value: 1\n"
	    "- name: second\n"
	    "  value: 2\n"
	    "scalars:\n"
	    "  - alpha\n"
	    "  - beta\n";

	d = yaml_parse(src);
	check_true("seq doc parses", d != NULL);
	if (d == NULL)
		return;

	check_str("dash-at-key item 0", yaml_get_scalar(d, "items.0.name"),
	    "first");
	check_str("dash-at-key item 1", yaml_get_scalar(d, "items.1.name"),
	    "second");
	check_str("dash-at-key value 1", yaml_get_scalar(d, "items.1.value"),
	    "2");
	check_str("scalar seq 0", yaml_get_scalar(d, "scalars.0"), "alpha");
	check_str("scalar seq 1", yaml_get_scalar(d, "scalars.1"), "beta");
	check_true("items has exactly 2",
	    yaml_get(d, "items")->nchildren == 2);
	check_true("scalars has exactly 2",
	    yaml_get(d, "scalars")->nchildren == 2);

	yaml_free(d);
}

static void
test_rejects(void)
{
	struct yaml_node *d;

	/* A tab in the indentation is invalid YAML; refuse, do not guess. */
	d = yaml_parse("a:\n\tb: c\n");
	check_true("tab indent rejected", d == NULL);
	yaml_free(d);

	/* A bare line that is neither mapping nor sequence is out of subset. */
	d = yaml_parse("just some prose\n");
	check_true("bare scalar rejected", d == NULL);
	yaml_free(d);

	/* NULL input must not crash. */
	d = yaml_parse(NULL);
	check_true("NULL input rejected", d == NULL);

	/* Empty and whitespace-only documents are valid and simply empty. */
	d = yaml_parse("");
	check_true("empty doc ok", d != NULL && d->nchildren == 0);
	yaml_free(d);
	d = yaml_parse("\n\n   \n# only a comment\n---\n");
	check_true("comment-only doc ok", d != NULL && d->nchildren == 0);
	yaml_free(d);

	/* Lookups on an empty tree return nothing rather than misbehaving. */
	d = yaml_parse("a: b\n");
	check_str("empty path", yaml_get_scalar(d, ""), NULL);
	check_str("NULL path", yaml_get_scalar(d, NULL), NULL);
	check_str("NULL root", yaml_get_scalar(NULL, "a"), NULL);
	yaml_free(d);
}

/*
 * The injection case. A manifest value containing a newline must not be able
 * to add structure to the document the converter generates.
 */
static void
test_quote(void)
{
	char buf[512];

	check_str("plain passes through",
	    yaml_quote("web", buf, sizeof(buf)), "web");
	check_str("empty becomes ''",
	    yaml_quote("", buf, sizeof(buf)), "''");
	check_str("NULL becomes ''",
	    yaml_quote(NULL, buf, sizeof(buf)), "''");
	check_str("newline is neutralised",
	    yaml_quote("web\nreplicas: 999", buf, sizeof(buf)),
	    "'web replicas: 999'");
	check_str("colon is quoted",
	    yaml_quote("a:b", buf, sizeof(buf)), "'a:b'");
	check_str("comment marker is quoted",
	    yaml_quote("a # b", buf, sizeof(buf)), "'a # b'");
	check_str("embedded quote is doubled",
	    yaml_quote("it's", buf, sizeof(buf)), "'it''s'");
	check_str("trailing space is quoted",
	    yaml_quote("web ", buf, sizeof(buf)), "'web '");

	/* A quoted value must survive a parse round trip unchanged. */
	{
		char line[1024];
		struct yaml_node *d;
		const char *nasty = "web\nreplicas: 999\nname: pwned";

		snprintf(line, sizeof(line), "name: %s\n",
		    yaml_quote(nasty, buf, sizeof(buf)));
		d = yaml_parse(line);
		check_true("injected doc still parses", d != NULL);
		if (d != NULL) {
			/* Exactly one key: the injection did not add any. */
			check_true("injection added no keys",
			    d->nchildren == 1);
			check_str("no injected replicas",
			    yaml_get_scalar(d, "replicas"), NULL);
			yaml_free(d);
		}
	}

	/* A short buffer must truncate safely, never overflow. */
	{
		char small[8];
		const char *r = yaml_quote("aaaaaaaaaaaaaaaaaaaa:bbbb",
		    small, sizeof(small));

		check_true("short buffer NUL-terminated",
		    r != NULL && strlen(r) < sizeof(small));
	}
}

static void
test_find_scalar(void)
{
	struct yaml_node *d;

	d = yaml_parse(manifest);
	if (d == NULL) {
		check_true("find_scalar manifest parses", 0);
		return;
	}
	/* Depth-first document order: metadata.name comes first. */
	check_str("find first name", yaml_find_scalar(d, "name"),
	    "web-frontend");
	check_str("find kind", yaml_find_scalar(d, "kind"), "Deployment");
	check_str("find absent", yaml_find_scalar(d, "nosuchkey"), NULL);
	yaml_free(d);
}

/*
 * Typed validation. A field that is supposed to be a number must be one --
 * atoi() accepted every one of these and produced a plausible wrong answer.
 */
static void
test_typed(void)
{
	struct yaml_node *d;
	static const char *src =
	    "good: 8080\n"
	    "zero: 0\n"
	    "negative: -5\n"
	    "spaced:   42  \n"
	    "alpha: abc\n"
	    "trailing: 8080x\n"
	    "fractional: 3.5\n"
	    "huge: 99999999999999999999\n"
	    "empty: \"\"\n"
	    "plus: +7\n"
	    "hex: 0x50\n"
	    "two: 3 4\n";
	long v;
	const char *why;

	d = yaml_parse(src);
	check_true("typed doc parses", d != NULL);
	if (d == NULL)
		return;

	/* Accepted. */
	v = -1;
	check_true("valid port accepted",
	    yaml_get_int(d, "good", YAML_PORT_MIN, YAML_PORT_MAX, &v,
	    &why) == 0 && v == 8080);
	v = -1;
	check_true("surrounding space tolerated",
	    yaml_get_int(d, "spaced", 0, 100, &v, &why) == 0 && v == 42);
	v = -1;
	check_true("explicit plus accepted",
	    yaml_get_int(d, "plus", 0, 100, &v, &why) == 0 && v == 7);
	v = -1;
	check_true("negative accepted when in range",
	    yaml_get_int(d, "negative", -10, 10, &v, &why) == 0 && v == -5);

	/* Rejected -- each of these atoi() silently accepted. */
	check_true("non-numeric rejected",
	    yaml_get_int(d, "alpha", 0, 100, &v, &why) != 0);
	check_true("trailing junk rejected",
	    yaml_get_int(d, "trailing", YAML_PORT_MIN, YAML_PORT_MAX, &v,
	    &why) != 0);
	check_true("fractional rejected",
	    yaml_get_int(d, "fractional", 0, 100, &v, &why) != 0);
	check_true("overflow rejected",
	    yaml_get_int(d, "huge", 0, 100, &v, &why) != 0);
	check_true("empty rejected",
	    yaml_get_int(d, "empty", 0, 100, &v, &why) != 0);
	check_true("two numbers rejected",
	    yaml_get_int(d, "two", 0, 100, &v, &why) != 0);
	/* Base 10 only: 0x50 must not silently become 80. */
	check_true("hex rejected in a decimal field",
	    yaml_get_int(d, "hex", 0, 65535, &v, &why) != 0);

	/* Range is enforced, not merely advisory. */
	check_true("port 0 rejected",
	    yaml_get_int(d, "zero", YAML_PORT_MIN, YAML_PORT_MAX, &v,
	    &why) != 0);
	check_true("below range rejected",
	    yaml_get_int(d, "negative", 0, 10, &v, &why) != 0);
	check_true("above range rejected",
	    yaml_get_int(d, "good", 0, 1024, &v, &why) != 0);

	/* A missing field is distinguishable from a malformed one. */
	check_true("absent field reported",
	    yaml_get_int(d, "nosuch", 0, 100, &v, &why) != 0);
	check_true("absent field explains itself", why != NULL);

	/* *out is untouched on every failure. */
	v = 12345;
	(void)yaml_get_int(d, "alpha", 0, 100, &v, &why);
	check_true("out untouched on failure", v == 12345);

	/* Boundaries. */
	check_true("port 65535 accepted",
	    yaml_parse_int("65535", YAML_PORT_MIN, YAML_PORT_MAX, &v,
	    &why) == 0 && v == 65535);
	check_true("port 65536 rejected",
	    yaml_parse_int("65536", YAML_PORT_MIN, YAML_PORT_MAX, &v,
	    &why) != 0);
	check_true("NULL string rejected",
	    yaml_parse_int(NULL, 0, 10, &v, &why) != 0);

	yaml_free(d);
}

int
main(void)
{

	test_paths();
	test_typed();
	test_comments_and_quoting();
	test_sequence_styles();
	test_rejects();
	test_quote();
	test_find_scalar();

	printf("yaml_test: %d checks, %d failed\n", tests_run, tests_failed);
	return (tests_failed == 0 ? 0 : 1);
}
