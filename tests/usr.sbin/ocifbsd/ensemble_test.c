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
	    "ensemble_convert_deployment with empty input fills defaults");
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
	 * When the YAML has no `namespace:` field, the function falls
	 * back to opts->namespace. The header comment line uses the
	 * literal "default" instead, but the body uses opts->namespace.
	 */
	ATF_CHECK(strstr(out, "namespace: ns1") != NULL);
	ATF_CHECK(strstr(out, "replicas: 1") != NULL);
	ATF_CHECK(strstr(out, "image: nginx:latest") != NULL);
	ATF_CHECK(strstr(out, "container: 80") != NULL);
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

ATF_TP_ADD_TCS(tp)
{
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

	return (atf_no_error());
}
