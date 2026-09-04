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
 * Line- and indent-aware YAML reader for the manifest converters.
 *
 * The converters previously located a field by scanning the raw document for
 * the substring "<key>:". That cannot express WHERE a key lives, so
 * "metadata.name" and a container's "name" were the same query and the first
 * one in the file won; a key was matched inside a comment or inside a quoted
 * value; and nothing distinguished nesting levels at all.
 *
 * This parser builds the document's actual tree, so a lookup can name a path.
 * It is a deliberate SUBSET of YAML -- block mappings, block sequences,
 * scalars (bare, single- and double-quoted), and comments -- because that is
 * what the manifests being converted use. Constructs outside the subset are
 * rejected rather than half-understood: silently misreading a manifest is
 * worse than refusing it.
 *
 * Complexity: one pass over the input, O(n) in the document size, with an
 * explicit indent stack bounded by the nesting depth.
 */

#include <sys/param.h>

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "yaml.h"

#define	YAML_MAX_DEPTH	64

static struct yaml_node *
node_new(void)
{

	return (calloc(1, sizeof(struct yaml_node)));
}

static int
node_add_child(struct yaml_node *parent, struct yaml_node *child)
{
	struct yaml_node **grown;
	int newalloc;

	if (parent->nchildren >= parent->nalloc) {
		newalloc = parent->nalloc ? parent->nalloc * 2 : 8;
		grown = realloc(parent->children,
		    (size_t)newalloc * sizeof(*grown));
		if (grown == NULL)
			return (-1);
		parent->children = grown;
		parent->nalloc = newalloc;
	}
	parent->children[parent->nchildren++] = child;
	return (0);
}

void
yaml_free(struct yaml_node *n)
{
	int i;

	if (n == NULL)
		return;
	for (i = 0; i < n->nchildren; i++)
		yaml_free(n->children[i]);
	free(n->children);
	free(n->key);
	free(n->value);
	free(n);
}

/*
 * Copy a scalar, resolving quoting and stripping a trailing comment.
 *
 * A '#' only starts a comment when it is at the start of the scalar or
 * preceded by whitespace, and never inside quotes -- otherwise a legitimate
 * value such as an image tag or a colour would be truncated.
 */
static char *
scalar_dup(const char *s, size_t len)
{
	char *out;
	size_t i, o;
	char quote;

	/* Leading whitespace. */
	while (len > 0 && (*s == ' ' || *s == '\t')) {
		s++;
		len--;
	}

	if (len > 0 && (*s == '"' || *s == '\'')) {
		quote = *s;
		out = malloc(len + 1);
		if (out == NULL)
			return (NULL);
		o = 0;
		for (i = 1; i < len; i++) {
			if (s[i] == quote) {
				/* '' inside a single-quoted scalar is one '. */
				if (quote == '\'' && i + 1 < len &&
				    s[i + 1] == '\'') {
					out[o++] = '\'';
					i++;
					continue;
				}
				break;
			}
			if (quote == '"' && s[i] == '\\' && i + 1 < len) {
				i++;
				switch (s[i]) {
				case 'n': out[o++] = '\n'; break;
				case 't': out[o++] = '\t'; break;
				case 'r': out[o++] = '\r'; break;
				case '0': out[o++] = '\0'; break;
				default: out[o++] = s[i]; break;
				}
				continue;
			}
			out[o++] = s[i];
		}
		out[o] = '\0';
		return (out);
	}

	/* Bare scalar: ends at a comment or at the end of the line. */
	for (i = 0; i < len; i++) {
		if (s[i] == '#' && (i == 0 || s[i - 1] == ' ' ||
		    s[i - 1] == '\t')) {
			len = (i > 0) ? i - 1 : 0;
			break;
		}
	}
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t'))
		len--;

	out = malloc(len + 1);
	if (out == NULL)
		return (NULL);
	memcpy(out, s, len);
	out[len] = '\0';
	return (out);
}

/*
 * Split "key: value" at the separating colon.
 *
 * The colon must be followed by a space or end-of-line to separate a key --
 * "image: nginx:1.25" has two colons and only the first one separates. A key
 * may itself be quoted. Returns the offset of the colon, or -1 if the line is
 * not a mapping entry.
 */
static int
split_key(const char *s, size_t len)
{
	size_t i;
	char quote;

	i = 0;
	if (len > 0 && (s[0] == '"' || s[0] == '\'')) {
		quote = s[0];
		for (i = 1; i < len && s[i] != quote; i++)
			;
		if (i >= len)
			return (-1);		/* unterminated quoted key */
		i++;
	}
	for (; i < len; i++) {
		if (s[i] == '#' && (i == 0 || s[i - 1] == ' '))
			return (-1);		/* comment before any colon */
		if (s[i] != ':')
			continue;
		if (i + 1 == len || s[i + 1] == ' ' || s[i + 1] == '\t')
			return ((int)i);
	}
	return (-1);
}

struct frame {
	struct yaml_node	*node;
	int			 indent;
};

struct yaml_node *
yaml_parse(const char *text)
{
	struct yaml_node *root, *node, *parent;
	struct frame stack[YAML_MAX_DEPTH];
	int depth;
	const char *line, *next;

	if (text == NULL) {
		errno = EINVAL;
		return (NULL);
	}
	root = node_new();
	if (root == NULL) {
		errno = ENOMEM;
		return (NULL);
	}
	stack[0].node = root;
	stack[0].indent = -1;
	depth = 1;

	for (line = text; *line != '\0'; line = next) {
		const char *content, *eol;
		size_t len;
		int indent, colon;
		bool is_item = false;

		eol = strchr(line, '\n');
		next = (eol != NULL) ? eol + 1 : line + strlen(line);
		len = (size_t)((eol != NULL ? eol : next) - line);
		/* Tolerate CRLF. */
		if (len > 0 && line[len - 1] == '\r')
			len--;

		/* Indentation. A tab in the indent is invalid YAML. */
		indent = 0;
		while ((size_t)indent < len && line[indent] == ' ')
			indent++;
		if ((size_t)indent < len && line[indent] == '\t') {
			yaml_free(root);
			errno = EINVAL;
			return (NULL);
		}
		content = line + indent;
		len -= (size_t)indent;

		/* Blank line, comment line, or a document marker. */
		if (len == 0 || content[0] == '#')
			continue;
		if (len >= 3 && (strncmp(content, "---", 3) == 0 ||
		    strncmp(content, "...", 3) == 0))
			continue;

		/*
		 * Sequence item. The dash may sit at the parent key's own
		 * indent (the common style) or be indented under it; both are
		 * legal, so the parent is the deepest open frame strictly
		 * shallower than the dash, or a frame at exactly the dash's
		 * indent that is still waiting for children.
		 */
		if (content[0] == '-' && (len == 1 || content[1] == ' ')) {
			is_item = true;
			content += (len == 1) ? 1 : 2;
			len -= (len == 1) ? 1 : 2;
			while (depth > 1 &&
			    (stack[depth - 1].indent > indent ||
			    (stack[depth - 1].indent == indent &&
			    stack[depth - 1].node->key == NULL)))
				depth--;
		} else {
			while (depth > 1 && stack[depth - 1].indent >= indent)
				depth--;
		}
		parent = stack[depth - 1].node;

		if (is_item) {
			struct yaml_node *item;

			item = node_new();
			if (item == NULL || node_add_child(parent, item) != 0) {
				yaml_free(item);
				yaml_free(root);
				errno = ENOMEM;
				return (NULL);
			}
			parent->seq = true;
			if (depth >= YAML_MAX_DEPTH) {
				yaml_free(root);
				errno = EINVAL;
				return (NULL);
			}
			stack[depth].node = item;
			stack[depth].indent = indent;
			depth++;

			if (len == 0)
				continue;	/* "-" alone: nested block */
			colon = split_key(content, len);
			if (colon < 0) {
				/* Scalar sequence item. */
				item->value = scalar_dup(content, len);
				if (item->value == NULL) {
					yaml_free(root);
					errno = ENOMEM;
					return (NULL);
				}
				continue;
			}
			/*
			 * "- key: value": the mapping starts just after the
			 * dash, so its entries sit at indent + 2.
			 */
			parent = item;
			indent += 2;
		} else {
			colon = split_key(content, len);
			if (colon < 0) {
				/*
				 * Not a mapping entry, not a sequence item --
				 * a bare scalar, a flow collection, or a
				 * multi-line scalar body. Outside the subset.
				 */
				yaml_free(root);
				errno = EINVAL;
				return (NULL);
			}
		}

		node = node_new();
		if (node == NULL) {
			yaml_free(root);
			errno = ENOMEM;
			return (NULL);
		}
		node->key = scalar_dup(content, (size_t)colon);
		if (node->key == NULL) {
			yaml_free(node);
			yaml_free(root);
			errno = ENOMEM;
			return (NULL);
		}
		if ((size_t)colon + 1 < len) {
			const char *rest = content + colon + 1;
			size_t restlen = len - (size_t)colon - 1;
			size_t k;

			/*
			 * Does anything but whitespace and a comment follow?
			 * That question -- not the resolved scalar -- decides
			 * between "has a value" and "has children", because
			 * `key: ""` resolves to an empty scalar yet is an
			 * explicit empty VALUE, and reporting it as absent
			 * loses the distinction the author wrote down.
			 */
			for (k = 0; k < restlen; k++) {
				if (rest[k] == ' ' || rest[k] == '\t')
					continue;
				if (rest[k] == '#')
					k = restlen;	/* comment only */
				break;
			}
			if (k < restlen) {
				node->value = scalar_dup(rest, restlen);
				if (node->value == NULL) {
					yaml_free(node);
					yaml_free(root);
					errno = ENOMEM;
					return (NULL);
				}
			}
		}
		if (node_add_child(parent, node) != 0) {
			yaml_free(node);
			yaml_free(root);
			errno = ENOMEM;
			return (NULL);
		}
		if (node->value == NULL) {
			if (depth >= YAML_MAX_DEPTH) {
				yaml_free(root);
				errno = EINVAL;
				return (NULL);
			}
			stack[depth].node = node;
			stack[depth].indent = indent;
			depth++;
		}
	}

	return (root);
}

static bool
all_digits(const char *s)
{
	const char *p;

	if (*s == '\0')
		return (false);
	for (p = s; *p != '\0'; p++)
		if (!isdigit((unsigned char)*p))
			return (false);
	return (true);
}

struct yaml_node *
yaml_get(struct yaml_node *root, const char *path)
{
	struct yaml_node *cur;
	const char *p;

	if (root == NULL || path == NULL)
		return (NULL);

	cur = root;
	for (p = path; *p != '\0';) {
		char comp[128];
		const char *dot;
		size_t clen;
		int i, found;

		dot = strchr(p, '.');
		clen = (dot != NULL) ? (size_t)(dot - p) : strlen(p);
		if (clen == 0 || clen >= sizeof(comp))
			return (NULL);
		memcpy(comp, p, clen);
		comp[clen] = '\0';
		p += clen + (dot != NULL ? 1 : 0);

		if (all_digits(comp)) {
			i = atoi(comp);
			if (i < 0 || i >= cur->nchildren)
				return (NULL);
			cur = cur->children[i];
			continue;
		}
		found = 0;
		for (i = 0; i < cur->nchildren; i++) {
			if (cur->children[i]->key != NULL &&
			    strcmp(cur->children[i]->key, comp) == 0) {
				cur = cur->children[i];
				found = 1;
				break;
			}
		}
		if (!found)
			return (NULL);
	}
	return (cur);
}

const char *
yaml_get_scalar(struct yaml_node *root, const char *path)
{
	struct yaml_node *n;

	n = yaml_get(root, path);
	return (n != NULL ? n->value : NULL);
}

const char *
yaml_find_scalar(struct yaml_node *root, const char *key)
{
	int i;

	if (root == NULL || key == NULL)
		return (NULL);
	for (i = 0; i < root->nchildren; i++) {
		struct yaml_node *c = root->children[i];
		const char *deep;

		if (c->key != NULL && strcmp(c->key, key) == 0 &&
		    c->value != NULL)
			return (c->value);
		deep = yaml_find_scalar(c, key);
		if (deep != NULL)
			return (deep);
	}
	return (NULL);
}

/*
 * Strict string-to-integer conversion.
 *
 * The WHOLE scalar must be consumed: "3abc", "3.5", "3 4" and " " are all
 * errors, not 3. Overflow is detected via errno rather than inferred from the
 * result, and the caller's [min, max] is enforced -- so a port is checked
 * against 1-65535 at the point of parsing rather than trusted downstream.
 */
int
yaml_parse_int(const char *s, long min, long max, long *out, const char **why)
{
	char *end;
	long v;

	if (why != NULL)
		*why = NULL;
	if (s == NULL) {
		if (why != NULL)
			*why = "value is missing";
		return (-1);
	}
	/* Leading space is tolerated; anything else non-numeric is not. */
	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '\0') {
		if (why != NULL)
			*why = "value is empty";
		return (-1);
	}

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno == ERANGE) {
		if (why != NULL)
			*why = "value is out of range for an integer";
		return (-1);
	}
	if (end == s) {
		if (why != NULL)
			*why = "value is not a number";
		return (-1);
	}
	while (*end == ' ' || *end == '\t')
		end++;
	if (*end != '\0') {
		/*
		 * Trailing junk. This is the case atoi() hides: "8080x" and
		 * "3.5" silently become 8080 and 3.
		 */
		if (why != NULL)
			*why = "value has trailing characters";
		return (-1);
	}
	if (v < min || v > max) {
		if (why != NULL)
			*why = "value is outside the permitted range";
		return (-1);
	}
	if (out != NULL)
		*out = v;
	return (0);
}

int
yaml_get_int(struct yaml_node *root, const char *path, long min, long max,
    long *out, const char **why)
{
	struct yaml_node *n;

	n = yaml_get(root, path);
	if (n == NULL || n->value == NULL) {
		if (why != NULL)
			*why = "field is not present";
		return (-1);
	}
	return (yaml_parse_int(n->value, min, max, out, why));
}

const char *
yaml_quote(const char *value, char *buf, size_t buflen)
{
	size_t i, o, len;
	bool needs;

	if (buflen == 0)
		return ("");
	if (value == NULL || value[0] == '\0') {
		strlcpy(buf, "''", buflen);
		return (buf);
	}

	len = strlen(value);
	needs = false;
	/*
	 * Quote whenever the value could be read back as anything other than
	 * one plain scalar: structural punctuation, a comment marker, a
	 * leading indicator character, or surrounding space. Newlines are the
	 * dangerous case -- they end the scalar and let the value inject
	 * further document structure -- and quoting alone does not neutralise
	 * a raw newline, so those are escaped below.
	 */
	if (value[0] == ' ' || value[len - 1] == ' ')
		needs = true;
	for (i = 0; i < len && !needs; i++) {
		switch (value[i]) {
		case ':': case '#': case '\n': case '\r': case '\t':
		case '{': case '}': case '[': case ']': case ',':
		case '&': case '*': case '!': case '|': case '>':
		case '\'': case '"': case '%': case '@': case '`':
			needs = true;
			break;
		default:
			break;
		}
	}
	if (i == 0 && (value[0] == '-' || value[0] == '?'))
		needs = true;
	if (!needs) {
		strlcpy(buf, value, buflen);
		return (buf);
	}

	/*
	 * Single-quoted style: the only escape inside it is '' for a literal
	 * quote, so nothing else in the value can terminate it. A newline or
	 * carriage return cannot appear in a single-quoted scalar on one
	 * line, so render those as visible spaces rather than emitting a
	 * document that no longer parses.
	 */
	o = 0;
	if (o + 1 < buflen)
		buf[o++] = '\'';
	for (i = 0; i < len && o + 2 < buflen; i++) {
		char c = value[i];

		if (c == '\n' || c == '\r' || c == '\t')
			c = ' ';
		if (c == '\'') {
			if (o + 3 >= buflen)
				break;
			buf[o++] = '\'';
		}
		buf[o++] = c;
	}
	if (o + 1 < buflen)
		buf[o++] = '\'';
	buf[o] = '\0';
	return (buf);
}
