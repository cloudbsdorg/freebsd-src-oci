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
 * Config converter header
 */

#ifndef _OCIFBSD_CONVERT_H
#define _OCIFBSD_CONVERT_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Conversion source types
 */
typedef enum {
	CONVERT_ENSEMBLE_YAML = 0,
	CONVERT_ENSEMBLE_JSON,
	CONVERT_ENSEMBLE_SERVICES,
	CONVERT_ENSEMBLE_STACK
} convert_source_type_t;

/*
 * Conversion output types
 */
typedef enum {
	CONVERT_NATIVE_YAML = 0,
	CONVERT_NATIVE_JSON,
	CONVERT_NATIVE_SIMPLIFIED
} convert_output_type_t;

/*
 * Conversion options
 */
struct convert_options {
	convert_output_type_t output_type;
	bool			annotate;	/* Add comments explaining conversion */
	bool			compact;	/* Minimize output */
	bool			validate;	/* Validate output */
	bool			dry_run;	/* Don't write files */
	bool			interactive;	/* Ask about ambiguous mappings */
	const char		*output_file;
	const char		*namespace;
	bool			include_secrets;
	bool			include_configs;
};

/*
 * Conversion result
 */
typedef enum {
	CONVERT_SUCCESS = 0,
	CONVERT_SYNTAX_ERROR,
	CONVERT_UNSUPPORTED_FEATURE,
	CONVERT_AMBIGUOUS_MAPPING,
	CONVERT_VALIDATION_ERROR,
	CONVERT_FILE_ERROR,
	CONVERT_MEMORY_ERROR
} convert_result_t;

/*
 * Conversion warning
 */
struct convert_warning {
	char		message[512];
	int		line;
	convert_result_t code;
};

/*
 * Conversion context
 */
struct convert_context {
	convert_source_type_t source_type;
	convert_output_type_t output_type;
	struct convert_warning *warnings;
	int			nwarnings;
	int			warning_capacity;
	bool			verbose;
};

/*
 * Convert functions
 */
int	convert_file(const char *input_path, const char *output_path,
	    struct convert_options *opts);
int	convert_stdin(const char *output_path, struct convert_options *opts);
int	convert_buffer(const char *input, size_t len, char **output,
	    struct convert_options *opts);

/*
 * Ensemble conversion
 */
int	ensemble_convert_deployment(const char *yaml, char **output,
	    struct convert_options *opts);
int	ensemble_convert_service(const char *yaml, char **output,
	    struct convert_options *opts);
int	ensemble_convert_configmap(const char *yaml, char **output,
	    struct convert_options *opts);
int	ensemble_convert_secret(const char *yaml, char **output,
	    struct convert_options *opts);
int	ensemble_convert_ingress(const char *yaml, char **output,
	    struct convert_options *opts);
int	ensemble_convert_persistentvolumeclaim(const char *yaml, char **output,
	    struct convert_options *opts);
int	ensemble_convert_namespace(const char *yaml, char **output,
	    struct convert_options *opts);
int	ensemble_convert_multi(const char *yaml, char **output,
	    struct convert_options *opts);

/*
 * Ensemble conversion
 */
int	ensemble_services_convert(const char *compose, char **output,
	    struct convert_options *opts);
int	ensemble_services_convert_v1(const char *compose, char **output,
	    struct convert_options *opts);
int	ensemble_services_convert_v2(const char *compose, char **output,
	    struct convert_options *opts);
int	ensemble_services_convert_v3(const char *compose, char **output,
	    struct convert_options *opts);

/*
 * Parser utilities
 */
int	parse_yaml(const char *input, char **output, struct convert_options *opts);
int	parse_json(const char *input, char **output, struct convert_options *opts);
char	*yaml_escape(const char *str);
char	*json_escape(const char *str);

/*
 * Native format output
 */
char	*native_format_service(const char *name, const char *image,
	    int replicas, const char *ports, const char *volumes,
	    const char *environment, const char *networks,
	    const char *depends_on, struct convert_options *opts);
char	*native_format_network(const char *name, const char *driver,
	    const char *subnet, struct convert_options *opts);
char	*native_format_volume(const char *name, const char *driver,
	    const char *opts, struct convert_options *copts);
char	*native_format_stack(const char *name, const char *services,
	    const char *networks, const char *volumes,
	    struct convert_options *opts);

/*
 * Utility functions
 */
const char	*convert_result_str(convert_result_t result);
void	convert_add_warning(struct convert_context *ctx, int line,
	    convert_result_t code, const char *fmt, ...);
int	convert_get_warnings(struct convert_context *ctx,
	    struct convert_warning **warnings, int *count);
void	convert_free_warnings(struct convert_warning *warnings, int count);

#endif /* _OCIFBSD_CONVERT_H */
