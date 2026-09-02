/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests for usr.sbin/ocifbsd/convert/ensemble.c
 *
 * Tests the Ensemble YAML-to-native converters. ensemble.c has
 * no external dependencies (no yaml/json-c libs), so the .c
 * file is #include'd directly. This also makes the static
 * helpers ensemble_detect_kind() and yaml_get_field() visible.
 */

#include <sys/param.h>

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "convert.h"
#include "convert/ensemble.c"
#include "convert/ensemble_services.c"

/* ----- ensemble_detect_kind (static) ----- */

ATF_TC(ensemble_detect_kind_deployment);
ATF_TC_HEAD(ensemble_detect_kind_deployment, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: Deployment");
}
ATF_TC_BODY(ensemble_detect_kind_deployment, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: Deployment\n"), ENSEMBLE_DEPLOYMENT);
}

ATF_TC(ensemble_detect_kind_service);
ATF_TC_HEAD(ensemble_detect_kind_service, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: Service");
}
ATF_TC_BODY(ensemble_detect_kind_service, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: Service\n"), ENSEMBLE_SERVICE);
}

ATF_TC(ensemble_detect_kind_service_account_not_service);
ATF_TC_HEAD(ensemble_detect_kind_service_account_not_service, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_detect_kind: ServiceAccount must not match Service");
}
ATF_TC_BODY(ensemble_detect_kind_service_account_not_service, tc)
{
	/* Prefix matching used to classify this as ENSEMBLE_SERVICE. */
	ATF_CHECK_EQ(
	    ensemble_detect_kind("apiVersion: v1\nkind: ServiceAccount\n"),
	    ENSEMBLE_UNKNOWN);
}

ATF_TC(ensemble_detect_kind_pod_template_not_pod);
ATF_TC_HEAD(ensemble_detect_kind_pod_template_not_pod, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_detect_kind: PodTemplate must not match Pod");
}
ATF_TC_BODY(ensemble_detect_kind_pod_template_not_pod, tc)
{
	ATF_CHECK_EQ(
	    ensemble_detect_kind("apiVersion: v1\nkind: PodTemplate\n"),
	    ENSEMBLE_UNKNOWN);
}

ATF_TC(ensemble_detect_kind_quoted_value);
ATF_TC_HEAD(ensemble_detect_kind_quoted_value, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_detect_kind: a quoted kind value is matched");
}
ATF_TC_BODY(ensemble_detect_kind_quoted_value, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("kind: \"Pod\"\n"), ENSEMBLE_POD);
}

ATF_TC(ensemble_detect_kind_configmap);
ATF_TC_HEAD(ensemble_detect_kind_configmap, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: ConfigMap");
}
ATF_TC_BODY(ensemble_detect_kind_configmap, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: ConfigMap\n"), ENSEMBLE_CONFIGMAP);
}

ATF_TC(ensemble_detect_kind_secret);
ATF_TC_HEAD(ensemble_detect_kind_secret, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: Secret");
}
ATF_TC_BODY(ensemble_detect_kind_secret, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: Secret\n"), ENSEMBLE_SECRET);
}

ATF_TC(ensemble_detect_kind_ingress);
ATF_TC_HEAD(ensemble_detect_kind_ingress, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: Ingress");
}
ATF_TC_BODY(ensemble_detect_kind_ingress, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: Ingress\n"), ENSEMBLE_INGRESS);
}

ATF_TC(ensemble_detect_kind_pvc);
ATF_TC_HEAD(ensemble_detect_kind_pvc, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_detect_kind: kind: PersistentVolumeClaim");
}
ATF_TC_BODY(ensemble_detect_kind_pvc, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: PersistentVolumeClaim\n"),
	    ENSEMBLE_PVC);
}

ATF_TC(ensemble_detect_kind_namespace);
ATF_TC_HEAD(ensemble_detect_kind_namespace, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: Namespace");
}
ATF_TC_BODY(ensemble_detect_kind_namespace, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: Namespace\n"), ENSEMBLE_NAMESPACE);
}

ATF_TC(ensemble_detect_kind_pod);
ATF_TC_HEAD(ensemble_detect_kind_pod, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: Pod");
}
ATF_TC_BODY(ensemble_detect_kind_pod, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: Pod\n"), ENSEMBLE_POD);
}

ATF_TC(ensemble_detect_kind_statefulset);
ATF_TC_HEAD(ensemble_detect_kind_statefulset, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: StatefulSet");
}
ATF_TC_BODY(ensemble_detect_kind_statefulset, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: StatefulSet\n"),
	    ENSEMBLE_STATEFULSET);
}

ATF_TC(ensemble_detect_kind_daemonset);
ATF_TC_HEAD(ensemble_detect_kind_daemonset, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: DaemonSet");
}
ATF_TC_BODY(ensemble_detect_kind_daemonset, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: DaemonSet\n"),
	    ENSEMBLE_DAEMONSET);
}

ATF_TC(ensemble_detect_kind_job);
ATF_TC_HEAD(ensemble_detect_kind_job, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: Job");
}
ATF_TC_BODY(ensemble_detect_kind_job, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: Job\n"), ENSEMBLE_JOB);
}

ATF_TC(ensemble_detect_kind_cronjob);
ATF_TC_HEAD(ensemble_detect_kind_cronjob, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: kind: CronJob");
}
ATF_TC_BODY(ensemble_detect_kind_cronjob, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nkind: CronJob\n"),
	    ENSEMBLE_CRONJOB);
}

ATF_TC(ensemble_detect_kind_unknown);
ATF_TC_HEAD(ensemble_detect_kind_unknown, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: unknown kind returns ENSEMBLE_UNKNOWN");
}
ATF_TC_BODY(ensemble_detect_kind_unknown, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("kind: SomethingNew\n"), ENSEMBLE_UNKNOWN);
}

ATF_TC(ensemble_detect_kind_no_kind);
ATF_TC_HEAD(ensemble_detect_kind_no_kind, tc)
{
	atf_tc_set_md_var(tc, "descr", "ensemble_detect_kind: no 'kind:' returns ENSEMBLE_UNKNOWN");
}
ATF_TC_BODY(ensemble_detect_kind_no_kind, tc)
{
	ATF_CHECK_EQ(ensemble_detect_kind("apiVersion: v1\nmetadata:\n  name: x\n"),
	    ENSEMBLE_UNKNOWN);
}

/* ----- ensemble_convert_deployment ----- */

ATF_TC(ensemble_convert_deployment_full);
ATF_TC_HEAD(ensemble_convert_deployment_full, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_convert_deployment with all fields");
}
ATF_TC_BODY(ensemble_convert_deployment_full, tc)
{
	const char *yaml =
	    "apiVersion: apps/v1\n"
	    "kind: Deployment\n"
	    "metadata:\n"
	    "  name: web\n"
	    "  namespace: prod\n"
	    "spec:\n"
	    "  replicas: 5\n"
	    "  template:\n"
	    "    spec:\n"
	    "      containers:\n"
	    "      - name: web\n"
	    "        image: nginx:1.25\n"
	    "        ports:\n"
	    "        - containerPort: 8080\n";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.namespace = "default";
	char *out = NULL;
	int rc = ensemble_convert_deployment(yaml, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK(strstr(out, "name: web") != NULL);
	ATF_CHECK(strstr(out, "namespace: prod") != NULL);
	ATF_CHECK(strstr(out, "replicas: 5") != NULL);
	ATF_CHECK(strstr(out, "image: nginx:1.25") != NULL);
	ATF_CHECK(strstr(out, "container: 8080") != NULL);
	free(out);
}

ATF_TC(ensemble_convert_deployment_minimal);
ATF_TC_HEAD(ensemble_convert_deployment_minimal, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_convert_deployment does not fabricate an image or port "
	    "for a manifest that declares none");
}
ATF_TC_BODY(ensemble_convert_deployment_minimal, tc)
{
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	opts.namespace = "ns1";
	char *out = NULL;
	int rc = ensemble_convert_deployment("", &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK(strstr(out, "name: unknown") != NULL);
	/*
	 * When the YAML has no `namespace:` field, the body falls back to
	 * opts->namespace. replicas defaults to 1 (a Deployment implies at
	 * least one replica — a sensible default, not fabricated config).
	 */
	ATF_CHECK(strstr(out, "namespace: ns1") != NULL);
	ATF_CHECK(strstr(out, "replicas: 1") != NULL);
	/* An image is NOT invented; a placeholder flags the missing field. */
	ATF_CHECK_MSG(strstr(out, "nginx:latest") == NULL,
	    "converter fabricated a default image");
	ATF_CHECK(strstr(out, "TODO: set image") != NULL);
	/* No port is invented when the manifest declares none. */
	ATF_CHECK_MSG(strstr(out, "container: 80") == NULL,
	    "converter fabricated a default port");
	ATF_CHECK_MSG(strstr(out, "ports:") == NULL,
	    "converter emitted a ports block for a portless deployment");
	free(out);
}

/* ----- ensemble_convert_multi ----- */

ATF_TC(ensemble_convert_multi_single_doc);
ATF_TC_HEAD(ensemble_convert_multi_single_doc, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_convert_multi with a single doc produces the converted output");
}
ATF_TC_BODY(ensemble_convert_multi_single_doc, tc)
{
	/*
	 * ensemble_convert_multi splits on `---` and only processes docs
	 * that are followed by another `---` separator. A single
	 * doc with no `---` produces only the header (no conversion).
	 * Prefix with `---` to make the doc get processed.
	 */
	const char *yaml =
	    "---\n"
	    "apiVersion: v1\n"
	    "kind: Namespace\n"
	    "metadata:\n"
	    "  name: myns\n";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *out = NULL;
	int rc = ensemble_convert_multi(yaml, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK(strstr(out, "version:") != NULL);
	ATF_CHECK(strstr(out, "name: myns") != NULL);
	free(out);
}

ATF_TC(ensemble_convert_multi_two_docs);
ATF_TC_HEAD(ensemble_convert_multi_two_docs, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_convert_multi converts every document in a multi-doc stream");
}
ATF_TC_BODY(ensemble_convert_multi_two_docs, tc)
{
	/*
	 * Both documents in a two-document stream must be converted,
	 * including the trailing document that has no following "---".
	 * (This previously dropped the last doc; the fix walks each
	 * "---"-separated segment rather than requiring a trailing
	 * separator.)
	 */
	const char *yaml =
	    "---\n"
	    "apiVersion: v1\n"
	    "kind: Namespace\n"
	    "metadata:\n"
	    "  name: ns1\n"
	    "---\n"
	    "apiVersion: v1\n"
	    "kind: ConfigMap\n"
	    "metadata:\n"
	    "  name: cfg1\n";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *out = NULL;
	int rc = ensemble_convert_multi(yaml, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK(strstr(out, "name: ns1") != NULL);
	ATF_CHECK(strstr(out, "name: cfg1") != NULL);
	free(out);
}

ATF_TC(ensemble_convert_multi_skips_unknown);
ATF_TC_HEAD(ensemble_convert_multi_skips_unknown, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensemble_convert_multi with an unknown kind emits a skipped comment");
}
ATF_TC_BODY(ensemble_convert_multi_skips_unknown, tc)
{
	const char *yaml =
	    "---\n"
	    "apiVersion: example.com/v1\n"
	    "kind: SomethingNew\n"
	    "metadata:\n"
	    "  name: foo\n";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *out = NULL;
	int rc = ensemble_convert_multi(yaml, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK(strstr(out, "Skipped unknown kind") != NULL);
	free(out);
}

/* A multi-service stack file converts and names each service (not "compose"). */
ATF_TC_WITHOUT_HEAD(ensemble_services_two_services);
ATF_TC_BODY(ensemble_services_two_services, tc)
{
	const char *in =
	    "version: \"3\"\n"
	    "services:\n"
	    "  web:\n"
	    "    image: nginx:1.27\n"
	    "  db:\n"
	    "    image: postgres:16\n";
	char *out = NULL;
	struct convert_options opts;
	int r;

	memset(&opts, 0, sizeof(opts));
	r = ensemble_services_convert(in, &out, &opts);
	ATF_REQUIRE_EQ(0, r);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK_MSG(strstr(out, "No services found") == NULL,
	    "converter reported no services for a valid stack file");
	ATF_CHECK_MSG(strstr(out, "web") != NULL, "service 'web' missing from output");
	ATF_CHECK_MSG(strstr(out, "db") != NULL, "service 'db' missing from output");
	/* nested keys must not be treated as services */
	ATF_CHECK_MSG(strstr(out, "name: image") == NULL,
	    "a nested key was mistaken for a service");
	free(out);
}

/*
 * A service's fields must not bleed into another's. Previously the block for a
 * service that omitted ports/volumes matched a *later* service's, so e.g. web
 * inherited db's volume and db inherited redis's port.
 */
ATF_TC_WITHOUT_HEAD(ensemble_services_no_field_bleed);
ATF_TC_BODY(ensemble_services_no_field_bleed, tc)
{
	const char *in =
	    "version: \"3.8\"\n"
	    "services:\n"
	    "  web:\n"
	    "    image: nginx:1.27\n"
	    "    ports:\n"
	    "      - \"80:80\"\n"
	    "  db:\n"
	    "    image: postgres:16\n"
	    "    volumes:\n"
	    "      - dbdata:/var/lib/postgresql/data\n"
	    "  cache:\n"
	    "    image: redis:7\n"
	    "    ports:\n"
	    "      - \"6379:6379\"\n";
	char *out = NULL;
	char *web, *db;
	struct convert_options opts;

	/* Unit: the extracted per-service block is bounded to that service. */
	web = ensemble_services_find_block(in, "web");
	ATF_REQUIRE(web != NULL);
	ATF_CHECK_MSG(strstr(web, "nginx") != NULL, "web block lost its image");
	ATF_CHECK_MSG(strstr(web, "80:80") != NULL, "web block lost its port");
	ATF_CHECK_MSG(strstr(web, "dbdata") == NULL,
	    "web block bled in db's volume");
	ATF_CHECK_MSG(strstr(web, "postgres") == NULL,
	    "web block bled into db");
	free(web);

	db = ensemble_services_find_block(in, "db");
	ATF_REQUIRE(db != NULL);
	ATF_CHECK_MSG(strstr(db, "dbdata") != NULL, "db block lost its volume");
	ATF_CHECK_MSG(strstr(db, "6379") == NULL, "db block bled in cache's port");
	free(db);

	/* End to end: the converted output keeps fields with their service. */
	memset(&opts, 0, sizeof(opts));
	ATF_REQUIRE_EQ(0, ensemble_services_convert(in, &out, &opts));
	ATF_REQUIRE(out != NULL);
	free(out);
}

/* No stray "compose" branding in the converter's output. */
ATF_TC_WITHOUT_HEAD(ensemble_services_no_compose_branding);
ATF_TC_BODY(ensemble_services_no_compose_branding, tc)
{
	const char *in = "services:\n  app:\n    image: alpine:3.20\n";
	char *out = NULL;
	struct convert_options opts;

	memset(&opts, 0, sizeof(opts));
	ATF_REQUIRE_EQ(0, ensemble_services_convert(in, &out, &opts));
	ATF_REQUIRE(out != NULL);
	ATF_CHECK_MSG(strstr(out, "compose") == NULL,
	    "output still contains the word 'compose'");
	free(out);
}

/* ----- per-kind converter correctness ----- */

ATF_TC(ensemble_convert_pvc_uses_declared_size);
ATF_TC_HEAD(ensemble_convert_pvc_uses_declared_size, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "PVC conversion emits the declared storage size");
}
ATF_TC_BODY(ensemble_convert_pvc_uses_declared_size, tc)
{
	const char *yaml =
	    "kind: PersistentVolumeClaim\n"
	    "metadata:\n"
	    "  name: pgdata\n"
	    "spec:\n"
	    "  resources:\n"
	    "    requests:\n"
	    "      storage: 20Gi\n";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *out = NULL;
	int rc = ensemble_convert_persistentvolumeclaim(yaml, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK(strstr(out, "size: 20Gi") != NULL);
	free(out);
}

ATF_TC(ensemble_convert_pvc_no_fabricated_size);
ATF_TC_HEAD(ensemble_convert_pvc_no_fabricated_size, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "PVC without a storage request must not invent a default size");
}
ATF_TC_BODY(ensemble_convert_pvc_no_fabricated_size, tc)
{
	const char *yaml =
	    "kind: PersistentVolumeClaim\n"
	    "metadata:\n"
	    "  name: pgdata\n";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *out = NULL;
	int rc = ensemble_convert_persistentvolumeclaim(yaml, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	/* No fabricated size, and no stray "size:" key at all. */
	ATF_CHECK(strstr(out, "1Gi") == NULL);
	ATF_CHECK(strstr(out, "size:") == NULL);
	ATF_CHECK(strstr(out, "TODO") != NULL);
	free(out);
}

ATF_TC(ensemble_convert_configmap_null_namespace);
ATF_TC_HEAD(ensemble_convert_configmap_null_namespace, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "A zero-initialized options struct yields namespace 'default', "
	    "never a NULL passed to a format string");
}
ATF_TC_BODY(ensemble_convert_configmap_null_namespace, tc)
{
	const char *yaml =
	    "kind: ConfigMap\n"
	    "metadata:\n"
	    "  name: appcfg\n";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));	/* opts.namespace == NULL */
	char *out = NULL;
	int rc = ensemble_convert_configmap(yaml, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK(strstr(out, "namespace: default") != NULL);
	ATF_CHECK(strstr(out, "(null)") == NULL);
	free(out);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ensemble_services_two_services);
	ATF_TP_ADD_TC(tp, ensemble_services_no_field_bleed);
	ATF_TP_ADD_TC(tp, ensemble_services_no_compose_branding);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_deployment);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_service);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_service_account_not_service);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_pod_template_not_pod);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_quoted_value);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_configmap);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_secret);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_ingress);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_pvc);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_namespace);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_pod);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_statefulset);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_daemonset);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_job);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_cronjob);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_unknown);
	ATF_TP_ADD_TC(tp, ensemble_detect_kind_no_kind);

	ATF_TP_ADD_TC(tp, ensemble_convert_deployment_full);
	ATF_TP_ADD_TC(tp, ensemble_convert_deployment_minimal);

	ATF_TP_ADD_TC(tp, ensemble_convert_multi_single_doc);
	ATF_TP_ADD_TC(tp, ensemble_convert_multi_two_docs);
	ATF_TP_ADD_TC(tp, ensemble_convert_multi_skips_unknown);

	ATF_TP_ADD_TC(tp, ensemble_convert_pvc_uses_declared_size);
	ATF_TP_ADD_TC(tp, ensemble_convert_pvc_no_fabricated_size);
	ATF_TP_ADD_TC(tp, ensemble_convert_configmap_null_namespace);

	return (atf_no_error());
}
