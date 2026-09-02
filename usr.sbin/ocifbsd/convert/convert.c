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
 * Config converter - main implementation
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include "convert.h"

/*
 * Conversion result strings
 */
const char *
convert_result_str(convert_result_t result)
{
	switch (result) {
	case CONVERT_SUCCESS:
		return ("Success");
	case CONVERT_SYNTAX_ERROR:
		return ("Syntax error");
	case CONVERT_UNSUPPORTED_FEATURE:
		return ("Unsupported feature");
	case CONVERT_AMBIGUOUS_MAPPING:
		return ("Ambiguous mapping");
	case CONVERT_VALIDATION_ERROR:
		return ("Validation error");
	case CONVERT_FILE_ERROR:
		return ("File error");
	case CONVERT_MEMORY_ERROR:
		return ("Memory error");
	default:
		return ("Unknown error");
	}
}

/*
 * Add a warning to the context
 */
void
convert_add_warning(struct convert_context *ctx, int line,
	convert_result_t code, const char *fmt, ...)
{
	struct convert_warning *w;
	char buf[512];
	va_list ap;

	if (ctx == NULL)
		return;

	if (ctx->nwarnings >= ctx->warning_capacity) {
		int new_cap = ctx->warning_capacity ?
		    ctx->warning_capacity * 2 : 16;
		struct convert_warning *grown = realloc(ctx->warnings,
		    new_cap * sizeof(struct convert_warning));
		/*
		 * On realloc failure keep the existing array and capacity
		 * rather than clobbering ctx->warnings with NULL (which would
		 * leak the old block and lose every warning recorded so far).
		 */
		if (grown == NULL)
			return;
		ctx->warnings = grown;
		ctx->warning_capacity = new_cap;
	}

	w = &ctx->warnings[ctx->nwarnings];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	strlcpy(w->message, buf, sizeof(w->message));
	w->line = line;
	w->code = code;
	ctx->nwarnings++;
}

/*
 * Get warnings from context
 */
int
convert_get_warnings(struct convert_context *ctx,
	struct convert_warning **warnings, int *count)
{
	if (ctx == NULL || warnings == NULL || count == NULL)
		return (-1);

	*warnings = ctx->warnings;
	*count = ctx->nwarnings;
	return (0);
}

/*
 * Free warnings
 */
void
convert_free_warnings(struct convert_warning *warnings, int count)
{
	/* Warnings are owned by the context, nothing to free here */
}

/*
 * Detect source type from file extension and content
 */
static convert_source_type_t
detect_source_type(const char *filename, const char *content)
{
	const char *ext;

	if (filename != NULL) {
		ext = strrchr(filename, '.');
		if (ext != NULL) {
			if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0)
				return (CONVERT_ENSEMBLE_YAML);
			if (strcmp(ext, ".json") == 0) {
				/* Check if it's ensemble or compose */
				if (strstr(content, "\"apiVersion\"") != NULL)
					return (CONVERT_ENSEMBLE_JSON);
				if (strstr(content, "\"version\"") != NULL &&
				    strstr(content, "\"services\"") != NULL)
					return (CONVERT_ENSEMBLE_SERVICES);
				return (CONVERT_ENSEMBLE_JSON);
			}
		}
	}

	/* Try to detect from content */
	if (strstr(content, "apiVersion:") != NULL)
		return (CONVERT_ENSEMBLE_YAML);
	if (strstr(content, "services:") != NULL)
		return (CONVERT_ENSEMBLE_SERVICES);

	return (CONVERT_ENSEMBLE_YAML);
}

/*
 * Convert file
 */
int
convert_file(const char *input_path, const char *output_path,
	struct convert_options *opts)
{
	FILE *fp;
	char *input;
	char *output;
	size_t len;
	struct stat st;
	int ret;

	if (input_path == NULL) {
		errno = EINVAL;
		return (CONVERT_FILE_ERROR);
	}

	/* Read input file */
	fp = fopen(input_path, "r");
	if (fp == NULL)
		return (CONVERT_FILE_ERROR);

	if (fstat(fileno(fp), &st) != 0) {
		fclose(fp);
		return (CONVERT_FILE_ERROR);
	}

	input = malloc(st.st_size + 1);
	if (input == NULL) {
		fclose(fp);
		return (CONVERT_MEMORY_ERROR);
	}

	len = fread(input, 1, st.st_size, fp);
	input[len] = '\0';
	fclose(fp);

	/* Convert */
	ret = convert_buffer(input, len, &output, opts);
	free(input);

	if (ret != CONVERT_SUCCESS)
		return (ret);

	/* Write output */
	if (output_path != NULL && !opts->dry_run) {
		fp = fopen(output_path, "w");
		if (fp == NULL) {
			free(output);
			return (CONVERT_FILE_ERROR);
		}
		fputs(output, fp);
		fclose(fp);
	} else if (!opts->dry_run) {
		puts(output);
	}

	free(output);
	return (CONVERT_SUCCESS);
}

/*
 * Convert from stdin
 */
int
convert_stdin(const char *output_path, struct convert_options *opts)
{
	char *input;
	char *output;
	size_t cap = 4096;
	size_t len = 0;
	int ret;

	input = malloc(cap);
	if (input == NULL)
		return (CONVERT_MEMORY_ERROR);

	/*
	 * Read all of stdin, growing the buffer as needed. The previous loop
	 * broke once the fixed 4 KiB buffer filled, silently truncating any
	 * manifest larger than 4095 bytes (real ensemble manifests routinely
	 * exceed that).
	 */
	for (;;) {
		size_t n = fread(input + len, 1, cap - len - 1, stdin);
		len += n;
		if (n == 0)
			break;		/* EOF or error */
		if (len >= cap - 1) {
			char *grown = realloc(input, cap * 2);
			if (grown == NULL) {
				free(input);
				return (CONVERT_MEMORY_ERROR);
			}
			input = grown;
			cap *= 2;
		}
	}
	input[len] = '\0';

	ret = convert_buffer(input, len, &output, opts);
	free(input);

	if (ret != CONVERT_SUCCESS)
		return (ret);

	if (output_path != NULL && !opts->dry_run) {
		FILE *fp = fopen(output_path, "w");
		if (fp == NULL) {
			free(output);
			return (CONVERT_FILE_ERROR);
		}
		fputs(output, fp);
		fclose(fp);
	} else if (!opts->dry_run) {
		puts(output);
	}

	free(output);
	return (CONVERT_SUCCESS);
}

/*
 * Convert buffer
 */
int
convert_buffer(const char *input, size_t len, char **output,
	struct convert_options *opts)
{
	convert_source_type_t type;
	int ret;
	char *result = NULL;

	if (input == NULL || output == NULL) {
		errno = EINVAL;
		return (CONVERT_SYNTAX_ERROR);
	}

	/* Detect source type */
	type = detect_source_type(NULL, input);

	switch (type) {
	case CONVERT_ENSEMBLE_YAML:
	case CONVERT_ENSEMBLE_JSON:
		ret = ensemble_convert_multi(input, &result, opts);
		break;
	case CONVERT_ENSEMBLE_SERVICES:
		ret = ensemble_services_convert(input, &result, opts);
		break;
	default:
		ret = CONVERT_UNSUPPORTED_FEATURE;
	}

	*output = result;
	return (ret);
}

/*
 * CLI main
 */
int
main(int argc, char **argv)
{
	const char *input_file = NULL;
	const char *output_file = NULL;
	struct convert_options opts = {
		.output_type = CONVERT_NATIVE_SIMPLIFIED,
		.annotate = false,
		.compact = false,
		.validate = true,
		.dry_run = false,
		.interactive = false,
		.namespace = "default",
		.include_secrets = true,
		.include_configs = true
	};
	int ret;

	static struct option longopts[] = {
		{ "output", required_argument, NULL, 'o' },
		{ "format", required_argument, NULL, 'f' },
		{ "annotate", no_argument, NULL, 'a' },
		{ "compact", no_argument, NULL, 'c' },
		{ "validate", no_argument, NULL, 'v' },
		{ "dry-run", no_argument, NULL, 'n' },
		{ "namespace", required_argument, NULL, 'N' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	int ch;
	while ((ch = getopt_long(argc, argv, "o:f:acvnN:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'o':
			output_file = optarg;
			break;
		case 'f':
			if (strcmp(optarg, "yaml") == 0)
				opts.output_type = CONVERT_NATIVE_YAML;
			else if (strcmp(optarg, "json") == 0)
				opts.output_type = CONVERT_NATIVE_JSON;
			else if (strcmp(optarg, "simple") == 0)
				opts.output_type = CONVERT_NATIVE_SIMPLIFIED;
			break;
		case 'a':
			opts.annotate = true;
			break;
		case 'c':
			opts.compact = true;
			break;
		case 'v':
			opts.validate = true;
			break;
		case 'n':
			opts.dry_run = true;
			break;
		case 'N':
			opts.namespace = optarg;
			break;
		case 'h':
		default:
			fprintf(stderr, "Usage: %s [options] [input-file]\n", argv[0]);
			fprintf(stderr, "Options:\n");
			fprintf(stderr, "  -o, --output FILE      Output file (default: stdout)\n");
			fprintf(stderr, "  -f, --format FORMAT   Output format: yaml, json, simple\n");
			fprintf(stderr, "  -a, --annotate        Add conversion comments\n");
			fprintf(stderr, "  -c, --compact         Compact output\n");
			fprintf(stderr, "  -v, --validate        Validate output\n");
			fprintf(stderr, "  -n, --dry-run         Don't write output\n");
			fprintf(stderr, "  -N, --namespace NS    Default namespace\n");
			fprintf(stderr, "  -h, --help            Show this help\n");
			return (ch == 'h' ? 0 : 1);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc > 0)
		input_file = argv[0];

	if (input_file != NULL) {
		ret = convert_file(input_file, output_file, &opts);
	} else {
		ret = convert_stdin(output_file, &opts);
	}

	if (ret != CONVERT_SUCCESS) {
		fprintf(stderr, "Error: %s\n", convert_result_str(ret));
		return (1);
	}

	return (0);
}
