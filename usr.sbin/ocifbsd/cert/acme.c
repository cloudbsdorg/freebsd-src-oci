/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ACME (RFC 8555) client for ocifbsd certificate management.
 *
 * This is a native C ACME client built on libcurl (transport) and OpenSSL
 * (account key, JWS/ES256 signing, CSR). It implements the account-management
 * foundation — directory discovery, replay-nonce handling, ES256 JWS signing
 * with an EC P-256 account key, and new-account registration — against any
 * RFC 8555 server (Let's Encrypt in production, or a local step-ca / Pebble
 * for testing). The order -> challenge -> finalize -> download flow is layered
 * on top in acme_certificate_request (see below).
 *
 * For testing against a CA whose root is not in the system trust store, set
 * OCIFBSD_ACME_CAINFO to a PEM bundle for that root; production Let's Encrypt
 * uses the system trust store.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <curl/curl.h>
#include <json.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "cert.h"

/* ------------------------------------------------------------------ */
/* Module state                                                       */
/* ------------------------------------------------------------------ */

static struct acme_ctx {
	int		 initialized;
	char		 directory_url[512];
	char		 url_new_nonce[512];
	char		 url_new_account[512];
	char		 url_new_order[512];
	char		 url_revoke[512];
	char		*cainfo;	/* PEM bundle to trust, or NULL */
	char		*account_key_path;	/* ACME account key PEM path */
	char		*cert_key_path;	/* issued-cert private key PEM path */
	char		*challenges_dir;
	char		*cert_path;
	char		*chain_path;
	EVP_PKEY	*account_key;	/* EC P-256 */
	char		*account_kid;	/* account URL after registration */
	char		*nonce;		/* current Replay-Nonce */
} actx;

/* ------------------------------------------------------------------ */
/* base64url + hashing                                                */
/* ------------------------------------------------------------------ */

/* base64url(no padding) of buf; returns a malloc'd C string, or NULL. */
static char *
b64url(const unsigned char *buf, size_t len)
{
	static const char t[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t olen = ((len + 2) / 3) * 4;
	char *out = malloc(olen + 1);
	size_t i, o = 0;

	if (out == NULL)
		return (NULL);
	for (i = 0; i < len; i += 3) {
		unsigned v = buf[i] << 16;
		int n = 1;

		if (i + 1 < len) { v |= buf[i + 1] << 8; n = 2; }
		if (i + 2 < len) { v |= buf[i + 2]; n = 3; }
		out[o++] = t[(v >> 18) & 0x3f];
		out[o++] = t[(v >> 12) & 0x3f];
		if (n >= 2)
			out[o++] = t[(v >> 6) & 0x3f];
		if (n >= 3)
			out[o++] = t[v & 0x3f];
	}
	out[o] = '\0';
	return (out);
}

static char *
b64url_str(const char *s)
{
	return (b64url((const unsigned char *)s, strlen(s)));
}

/* ------------------------------------------------------------------ */
/* HTTP transport (libcurl)                                           */
/* ------------------------------------------------------------------ */

struct http_buf {
	char	*data;
	size_t	 len;
};

struct http_result {
	long		 code;
	struct http_buf	 body;
	char		*location;	/* Location header, if any */
};

/* Hard cap on a single ACME response body — an order/authz/cert reply is a few
 * KB; refuse a runaway (or hostile) response rather than growing without bound. */
#define ACME_MAX_BODY  (8 * 1024 * 1024)

static size_t
body_cb(char *ptr, size_t size, size_t nmemb, void *userp)
{
	struct http_buf *b = userp;
	size_t add = size * nmemb;
	char *n;

	if (b->len + add + 1 > ACME_MAX_BODY)
		return (0);             /* abort the transfer */
	n = realloc(b->data, b->len + add + 1);
	if (n == NULL)
		return (0);
	b->data = n;
	memcpy(b->data + b->len, ptr, add);
	b->len += add;
	b->data[b->len] = '\0';
	return (add);
}

/* Capture the Replay-Nonce and Location headers as they arrive. */
static size_t
header_cb(char *ptr, size_t size, size_t nmemb, void *userp)
{
	struct http_result *r = userp;
	size_t n = size * nmemb;
	const char *nonce_h = "replay-nonce:";
	const char *loc_h = "location:";
	char line[1024];
	size_t l = n < sizeof(line) - 1 ? n : sizeof(line) - 1;

	memcpy(line, ptr, l);
	line[l] = '\0';
	if (strncasecmp(line, nonce_h, strlen(nonce_h)) == 0) {
		char *v = line + strlen(nonce_h);
		size_t vl;

		while (*v == ' ' || *v == '\t')
			v++;
		vl = strcspn(v, "\r\n");
		v[vl] = '\0';
		free(actx.nonce);
		actx.nonce = strdup(v);
	} else if (strncasecmp(line, loc_h, strlen(loc_h)) == 0) {
		char *v = line + strlen(loc_h);
		size_t vl;

		while (*v == ' ' || *v == '\t')
			v++;
		vl = strcspn(v, "\r\n");
		v[vl] = '\0';
		free(r->location);
		r->location = strdup(v);
	}
	return (n);
}

static void
http_result_free(struct http_result *r)
{
	free(r->body.data);
	free(r->location);
	r->body.data = NULL;
	r->body.len = 0;
	r->location = NULL;
}

/*
 * Perform an HTTP request. If jose is non-NULL the request is a POST of a
 * JWS (application/jose+json); otherwise it is a GET. Response body, status,
 * Location, and Replay-Nonce (into actx.nonce) are captured. Returns 0 on a
 * completed transfer (any status), -1 on transport failure.
 */
static int
http_do(const char *url, const char *jose, struct http_result *out)
{
	CURL *curl;
	CURLcode rc;
	struct curl_slist *hdrs = NULL;

	memset(out, 0, sizeof(*out));
	curl = curl_easy_init();
	if (curl == NULL)
		return (-1);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out->body);
	curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
	curl_easy_setopt(curl, CURLOPT_HEADERDATA, out);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "ocifbsd-acme/1.0");
	if (actx.cainfo != NULL)
		curl_easy_setopt(curl, CURLOPT_CAINFO, actx.cainfo);
	if (jose != NULL) {
		hdrs = curl_slist_append(hdrs,
		    "Content-Type: application/jose+json");
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jose);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
		    (long)strlen(jose));
	}
	rc = curl_easy_perform(curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->code);
	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	if (rc != CURLE_OK) {
		http_result_free(out);
		return (-1);
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* Account key + JWS (ES256)                                          */
/* ------------------------------------------------------------------ */

/* Extract the 32-byte affine X and Y of an EC P-256 public key. */
static int
ec_pub_xy(EVP_PKEY *k, unsigned char x[32], unsigned char y[32])
{
	BIGNUM *bx = NULL, *by = NULL;
	int ret = -1;

	if (EVP_PKEY_get_bn_param(k, OSSL_PKEY_PARAM_EC_PUB_X, &bx) != 1 ||
	    EVP_PKEY_get_bn_param(k, OSSL_PKEY_PARAM_EC_PUB_Y, &by) != 1)
		goto out;
	if (BN_bn2binpad(bx, x, 32) != 32 || BN_bn2binpad(by, y, 32) != 32)
		goto out;
	ret = 0;
out:
	BN_free(bx);
	BN_free(by);
	return (ret);
}

/* Canonical JWK for the account key (RFC 7638 member order for thumbprint
 * and for the JWS "jwk" header). Returns malloc'd JSON, or NULL. */
static char *
account_jwk_json(void)
{
	unsigned char x[32], y[32];
	char *bx, *by, *out;

	if (ec_pub_xy(actx.account_key, x, y) != 0)
		return (NULL);
	bx = b64url(x, 32);
	by = b64url(y, 32);
	if (bx == NULL || by == NULL) {
		free(bx);
		free(by);
		return (NULL);
	}
	if (asprintf(&out, "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\","
	    "\"y\":\"%s\"}", bx, by) < 0)
		out = NULL;
	free(bx);
	free(by);
	return (out);
}

/* RFC 7638 JWK thumbprint (base64url(SHA256(canonical jwk))). */
static char *
account_thumbprint(void)
{
	char *jwk = account_jwk_json();
	unsigned char h[SHA256_DIGEST_LENGTH];
	char *tp;

	if (jwk == NULL)
		return (NULL);
	SHA256((const unsigned char *)jwk, strlen(jwk), h);
	tp = b64url(h, sizeof(h));
	free(jwk);
	return (tp);
}

/* Convert a DER ECDSA-Sig to the raw 64-byte R||S JWS signature. */
static char *
ecdsa_der_to_jws(const unsigned char *der, size_t derlen)
{
	const unsigned char *p = der;
	ECDSA_SIG *sig = d2i_ECDSA_SIG(NULL, &p, (long)derlen);
	const BIGNUM *r, *s;
	unsigned char raw[64];
	char *out;

	if (sig == NULL)
		return (NULL);
	ECDSA_SIG_get0(sig, &r, &s);
	if (BN_bn2binpad(r, raw, 32) != 32 ||
	    BN_bn2binpad(s, raw + 32, 32) != 32) {
		ECDSA_SIG_free(sig);
		return (NULL);
	}
	out = b64url(raw, sizeof(raw));
	ECDSA_SIG_free(sig);
	return (out);
}

/*
 * Build a flattened JWS for an ACME request. protected header carries alg
 * ES256, url, nonce, and either the jwk (pre-account) or the kid (account
 * URL). payload is the JSON to sign, or NULL for a POST-as-GET (empty
 * payload). Returns malloc'd JSON, or NULL.
 */
static char *
jws_build(const char *url, const char *payload)
{
	char *prot_json = NULL, *prot_b64 = NULL, *pay_b64 = NULL;
	char *signing = NULL, *sig_b64 = NULL, *jose = NULL;
	unsigned char *der = NULL;
	size_t derlen = 0;
	EVP_MD_CTX *md = NULL;

	if (actx.nonce == NULL)
		return (NULL);

	/* protected header */
	if (actx.account_kid != NULL) {
		if (asprintf(&prot_json, "{\"alg\":\"ES256\",\"kid\":\"%s\","
		    "\"nonce\":\"%s\",\"url\":\"%s\"}",
		    actx.account_kid, actx.nonce, url) < 0)
			return (NULL);
	} else {
		char *jwk = account_jwk_json();

		if (jwk == NULL)
			return (NULL);
		if (asprintf(&prot_json, "{\"alg\":\"ES256\",\"jwk\":%s,"
		    "\"nonce\":\"%s\",\"url\":\"%s\"}",
		    jwk, actx.nonce, url) < 0) {
			free(jwk);
			return (NULL);
		}
		free(jwk);
	}
	prot_b64 = b64url_str(prot_json);
	pay_b64 = payload != NULL ? b64url_str(payload) : strdup("");
	if (prot_b64 == NULL || pay_b64 == NULL)
		goto out;
	if (asprintf(&signing, "%s.%s", prot_b64, pay_b64) < 0) {
		signing = NULL;
		goto out;
	}

	/* ES256 signature over "protected.payload" */
	md = EVP_MD_CTX_new();
	if (md == NULL)
		goto out;
	if (EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL,
	    actx.account_key) != 1)
		goto out;
	if (EVP_DigestSign(md, NULL, &derlen,
	    (const unsigned char *)signing, strlen(signing)) != 1)
		goto out;
	der = malloc(derlen);
	if (der == NULL)
		goto out;
	if (EVP_DigestSign(md, der, &derlen,
	    (const unsigned char *)signing, strlen(signing)) != 1)
		goto out;
	sig_b64 = ecdsa_der_to_jws(der, derlen);
	if (sig_b64 == NULL)
		goto out;

	if (asprintf(&jose, "{\"protected\":\"%s\",\"payload\":\"%s\","
	    "\"signature\":\"%s\"}", prot_b64, pay_b64, sig_b64) < 0)
		jose = NULL;
out:
	EVP_MD_CTX_free(md);
	free(der);
	free(prot_json);
	free(prot_b64);
	free(pay_b64);
	free(signing);
	free(sig_b64);
	return (jose);
}

/* Sign and POST a JWS to url; retries once on a badNonce. */
static int
acme_post(const char *url, const char *payload, struct http_result *out)
{
	char *jose;
	int attempt;

	for (attempt = 0; attempt < 2; attempt++) {
		jose = jws_build(url, payload);
		if (jose == NULL)
			return (-1);
		if (http_do(url, jose, out) != 0) {
			free(jose);
			return (-1);
		}
		free(jose);
		/* A stale nonce is reported as 400 urn:...:badNonce; the fresh
		 * Replay-Nonce from this very response is already cached, so a
		 * single retry succeeds. */
		if (out->code == 400 && out->body.data != NULL &&
		    strstr(out->body.data, "badNonce") != NULL) {
			http_result_free(out);
			continue;
		}
		return (0);
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* Directory + nonce                                                  */
/* ------------------------------------------------------------------ */

/* Pull a "key":"url" string value out of the directory JSON (small, flat). */
static void
dir_field(const char *json, const char *key, char *dst, size_t dstlen)
{
	char pat[64];
	const char *p, *q;

	dst[0] = '\0';
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(json, pat);
	if (p == NULL)
		return;
	p = strchr(p + strlen(pat), ':');
	if (p == NULL)
		return;
	p = strchr(p, '"');
	if (p == NULL)
		return;
	p++;
	q = strchr(p, '"');
	if (q == NULL || (size_t)(q - p) >= dstlen)
		return;
	memcpy(dst, p, q - p);
	dst[q - p] = '\0';
}

static int
acme_fetch_directory(void)
{
	struct http_result r;

	if (http_do(actx.directory_url, NULL, &r) != 0)
		return (-1);
	if (r.code != 200 || r.body.data == NULL) {
		http_result_free(&r);
		return (-1);
	}
	dir_field(r.body.data, "newNonce", actx.url_new_nonce,
	    sizeof(actx.url_new_nonce));
	dir_field(r.body.data, "newAccount", actx.url_new_account,
	    sizeof(actx.url_new_account));
	dir_field(r.body.data, "newOrder", actx.url_new_order,
	    sizeof(actx.url_new_order));
	dir_field(r.body.data, "revokeCert", actx.url_revoke,
	    sizeof(actx.url_revoke));
	http_result_free(&r);
	return (actx.url_new_account[0] != '\0' &&
	    actx.url_new_nonce[0] != '\0' ? 0 : -1);
}

static int
acme_fetch_nonce(void)
{
	struct http_result r;

	if (http_do(actx.url_new_nonce, NULL, &r) != 0)
		return (-1);
	http_result_free(&r);
	return (actx.nonce != NULL ? 0 : -1);
}

/* ------------------------------------------------------------------ */
/* Account key persistence                                            */
/* ------------------------------------------------------------------ */

static int
account_key_load_or_create(const char *path)
{
	FILE *f;

	if (path != NULL && (f = fopen(path, "r")) != NULL) {
		actx.account_key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
		fclose(f);
		if (actx.account_key != NULL)
			return (0);
	}
	/* Generate a fresh EC P-256 account key. */
	actx.account_key = EVP_EC_gen("P-256");
	if (actx.account_key == NULL)
		return (-1);
	if (path != NULL) {
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

		if (fd >= 0) {
			f = fdopen(fd, "w");
			if (f != NULL) {
				PEM_write_PrivateKey(f, actx.account_key, NULL,
				    NULL, 0, NULL, NULL);
				fclose(f);
			} else {
				close(fd);
			}
		}
	}
	return (0);
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int
acme_init(struct acme_config *config)
{
	const char *cainfo;

	if (config == NULL)
		return (-1);
	memset(&actx, 0, sizeof(actx));
	snprintf(actx.directory_url, sizeof(actx.directory_url), "%s",
	    config->server_url);
	if (config->challenges_dir != NULL)
		actx.challenges_dir = strdup(config->challenges_dir);
	if (config->certificate_path != NULL)
		actx.cert_path = strdup(config->certificate_path);
	if (config->chain_path != NULL)
		actx.chain_path = strdup(config->chain_path);
	if (config->private_key_path != NULL)
		actx.cert_key_path = strdup(config->private_key_path);
	cainfo = getenv("OCIFBSD_ACME_CAINFO");
	if (cainfo != NULL && cainfo[0] != '\0')
		actx.cainfo = strdup(cainfo);

	/*
	 * The ACME account key is distinct from the certificate key. Its path
	 * is OCIFBSD_ACME_ACCOUNT_KEY when set, else "<cert-key>.acct" so a
	 * stable account identity persists alongside the cert material.
	 */
	{
		const char *ak = getenv("OCIFBSD_ACME_ACCOUNT_KEY");

		if (ak != NULL && ak[0] != '\0')
			actx.account_key_path = strdup(ak);
		else if (actx.cert_key_path != NULL)
			asprintf(&actx.account_key_path, "%s.acct",
			    actx.cert_key_path);
	}

	if (account_key_load_or_create(actx.account_key_path) != 0)
		return (-1);
	actx.initialized = 1;
	return (0);
}

int
acme_account_register(const char *email)
{
	struct http_result r;
	char *payload = NULL;
	int ret = -1;

	if (!actx.initialized)
		return (-1);
	if (acme_fetch_directory() != 0)
		return (-1);
	if (acme_fetch_nonce() != 0)
		return (-1);

	if (email != NULL && email[0] != '\0') {
		if (asprintf(&payload, "{\"termsOfServiceAgreed\":true,"
		    "\"contact\":[\"mailto:%s\"]}", email) < 0)
			return (-1);
	} else {
		payload = strdup("{\"termsOfServiceAgreed\":true}");
		if (payload == NULL)
			return (-1);
	}

	if (acme_post(actx.url_new_account, payload, &r) != 0)
		goto out;
	/* 201 Created (new) or 200 OK (existing) both return the account URL
	 * in Location. */
	if ((r.code == 201 || r.code == 200) && r.location != NULL) {
		free(actx.account_kid);
		actx.account_kid = strdup(r.location);
		ret = 0;
	}
	http_result_free(&r);
out:
	free(payload);
	return (ret);
}

int
acme_account_status(void)
{
	return (actx.account_kid != NULL ? 0 : -1);
}

/*
 * Return the current account URL ("kid"), or NULL if not yet registered.
 * Exposed for tests and callers that need to confirm registration.
 */
const char *
acme_account_kid(void)
{
	return (actx.account_kid);
}

/* ------------------------------------------------------------------ */
/* Order -> challenge -> finalize -> download                         */
/* ------------------------------------------------------------------ */

/* Copy a string field out of a json object into dst; 0 if present. */
static int
jstr(struct json_object *o, const char *key, char *dst, size_t dstlen)
{
	struct json_object *v;

	if (o == NULL || !json_object_object_get_ex(o, key, &v))
		return (-1);
	snprintf(dst, dstlen, "%s", json_object_get_string(v));
	return (0);
}

/* Write the HTTP-01 key authorization to the webroot challenge path. */
static int
write_challenge(const char *token)
{
	char dir[PATH_MAX], path[PATH_MAX];
	char *tp, *keyauth = NULL;
	int fd, ret = -1;

	if (actx.challenges_dir == NULL)
		return (-1);
	tp = account_thumbprint();
	if (tp == NULL)
		return (-1);
	if (asprintf(&keyauth, "%s.%s", token, tp) < 0) {
		free(tp);
		return (-1);
	}
	free(tp);
	/* Standard webroot layout: <dir>/.well-known/acme-challenge/<token>. */
	snprintf(dir, sizeof(dir), "%s/.well-known", actx.challenges_dir);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.well-known/acme-challenge",
	    actx.challenges_dir);
	mkdir(dir, 0755);
	if (strchr(token, '/') != NULL) {	/* token is server-chosen b64url */
		free(keyauth);
		return (-1);
	}
	snprintf(path, sizeof(path), "%s/%s", dir, token);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, keyauth, strlen(keyauth)) == (ssize_t)strlen(keyauth))
			ret = 0;
		close(fd);
	}
	free(keyauth);
	return (ret);
}

/* Poll a resource (POST-as-GET) until its "status" is want or a terminal
 * "invalid"; returns 0 when want is reached, -1 otherwise. */
static int
poll_status(const char *url, const char *want, int max_tries)
{
	struct http_result r;
	int i;

	for (i = 0; i < max_tries; i++) {
		struct json_object *o;
		char status[32] = "";

		if (acme_post(url, NULL, &r) != 0)
			return (-1);
		o = r.body.data ? json_tokener_parse(r.body.data) : NULL;
		if (o != NULL) {
			jstr(o, "status", status, sizeof(status));
			json_object_put(o);
		}
		http_result_free(&r);
		if (strcmp(status, want) == 0)
			return (0);
		if (strcmp(status, "invalid") == 0)
			return (-1);
		sleep(1);
	}
	return (-1);
}

/* Build a CSR for domain with an EC P-256 key, write the key to
 * cert_key_path (0600), and return the base64url(DER) CSR. */
static char *
make_csr(const char *domain)
{
	EVP_PKEY *key = NULL;
	X509_REQ *req = NULL;
	X509_NAME *name = NULL;
	STACK_OF(X509_EXTENSION) *exts = NULL;
	X509_EXTENSION *san = NULL;
	unsigned char *der = NULL;
	char sanstr[300];
	char *out = NULL;
	int derlen;

	key = EVP_EC_gen("P-256");
	if (key == NULL)
		return (NULL);
	req = X509_REQ_new();
	if (req == NULL)
		goto out;
	X509_REQ_set_version(req, 0);
	name = X509_REQ_get_subject_name(req);
	X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
	    (const unsigned char *)domain, -1, -1, 0);
	/* subjectAltName is what modern validators require. */
	snprintf(sanstr, sizeof(sanstr), "DNS:%s", domain);
	san = X509V3_EXT_conf_nid(NULL, NULL, NID_subject_alt_name, sanstr);
	if (san == NULL)
		goto out;
	exts = sk_X509_EXTENSION_new_null();
	sk_X509_EXTENSION_push(exts, san);
	X509_REQ_add_extensions(req, exts);
	if (X509_REQ_set_pubkey(req, key) != 1 ||
	    X509_REQ_sign(req, key, EVP_sha256()) == 0)
		goto out;
	derlen = i2d_X509_REQ(req, &der);
	if (derlen <= 0)
		goto out;
	out = b64url(der, derlen);

	/* Persist the certificate private key. */
	if (out != NULL && actx.cert_key_path != NULL) {
		int fd = open(actx.cert_key_path,
		    O_WRONLY | O_CREAT | O_TRUNC, 0600);

		if (fd >= 0) {
			FILE *f = fdopen(fd, "w");

			if (f != NULL) {
				PEM_write_PrivateKey(f, key, NULL, NULL, 0,
				    NULL, NULL);
				fclose(f);
			} else {
				close(fd);
			}
		}
	}
out:
	OPENSSL_free(der);
	sk_X509_EXTENSION_free(exts);
	X509_EXTENSION_free(san);
	X509_REQ_free(req);
	EVP_PKEY_free(key);
	return (out);
}

static int
write_file_0644(const char *path, const char *data, size_t len)
{
	int fd, ret = -1;

	if (path == NULL)
		return (0);	/* nothing to write is not an error */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, data, len) == (ssize_t)len)
			ret = 0;
		close(fd);
	}
	return (ret);
}

int
acme_certificate_request(const char *domain, const char *challenge_dir)
{
	struct http_result r;
	struct json_object *o = NULL, *auths = NULL, *chals = NULL;
	char order_url[512] = "", finalize_url[512] = "", authz_url[512] = "";
	char chal_url[512] = "", token[256] = "", cert_url[512] = "";
	char *payload = NULL, *csr = NULL;
	int ret = -1, i, n;

	if (!actx.initialized || domain == NULL)
		return (-1);
	if (challenge_dir != NULL) {
		free(actx.challenges_dir);
		actx.challenges_dir = strdup(challenge_dir);
	}
	if (actx.account_kid == NULL && acme_account_register(NULL) != 0)
		return (-1);
	if (actx.nonce == NULL && acme_fetch_nonce() != 0)
		return (-1);

	/* 1. Create the order. */
	if (asprintf(&payload, "{\"identifiers\":[{\"type\":\"dns\","
	    "\"value\":\"%s\"}]}", domain) < 0)
		return (-1);
	if (acme_post(actx.url_new_order, payload, &r) != 0)
		goto out;
	if (r.code != 201 || r.body.data == NULL) {
		http_result_free(&r);
		goto out;
	}
	if (r.location != NULL)
		snprintf(order_url, sizeof(order_url), "%s", r.location);
	o = json_tokener_parse(r.body.data);
	http_result_free(&r);
	if (o == NULL)
		goto out;
	jstr(o, "finalize", finalize_url, sizeof(finalize_url));
	if (json_object_object_get_ex(o, "authorizations", &auths) &&
	    json_object_array_length(auths) > 0)
		snprintf(authz_url, sizeof(authz_url), "%s",
		    json_object_get_string(json_object_array_get_idx(auths, 0)));
	json_object_put(o);
	o = NULL;
	if (authz_url[0] == '\0' || finalize_url[0] == '\0')
		goto out;

	/* 2. Fetch the authorization, pick the http-01 challenge. */
	if (acme_post(authz_url, NULL, &r) != 0)
		goto out;
	o = r.body.data ? json_tokener_parse(r.body.data) : NULL;
	http_result_free(&r);
	if (o == NULL || !json_object_object_get_ex(o, "challenges", &chals))
		goto out;
	n = json_object_array_length(chals);
	for (i = 0; i < n; i++) {
		struct json_object *c = json_object_array_get_idx(chals, i);
		char type[32] = "";

		jstr(c, "type", type, sizeof(type));
		if (strcmp(type, "http-01") == 0) {
			jstr(c, "url", chal_url, sizeof(chal_url));
			jstr(c, "token", token, sizeof(token));
			break;
		}
	}
	json_object_put(o);
	o = NULL;
	if (chal_url[0] == '\0' || token[0] == '\0')
		goto out;

	/* 3. Publish the key authorization at the webroot. */
	if (write_challenge(token) != 0)
		goto out;

	/* 4. Tell the CA to validate. */
	if (acme_post(chal_url, "{}", &r) != 0)
		goto out;
	http_result_free(&r);

	/* 5. Wait for the authorization to go valid. */
	if (poll_status(authz_url, "valid", 30) != 0)
		goto out;

	/* 6. Finalize with a CSR. */
	csr = make_csr(domain);
	if (csr == NULL)
		goto out;
	free(payload);
	if (asprintf(&payload, "{\"csr\":\"%s\"}", csr) < 0) {
		payload = NULL;
		goto out;
	}
	if (acme_post(finalize_url, payload, &r) != 0)
		goto out;
	http_result_free(&r);

	/* 7. Wait for the order to go valid, then read the certificate URL. */
	if (order_url[0] == '\0' || poll_status(order_url, "valid", 30) != 0)
		goto out;
	if (acme_post(order_url, NULL, &r) != 0)
		goto out;
	o = r.body.data ? json_tokener_parse(r.body.data) : NULL;
	http_result_free(&r);
	if (o == NULL)
		goto out;
	jstr(o, "certificate", cert_url, sizeof(cert_url));
	json_object_put(o);
	o = NULL;
	if (cert_url[0] == '\0')
		goto out;

	/* 8. Download the issued certificate chain (PEM). */
	if (acme_post(cert_url, NULL, &r) != 0)
		goto out;
	if (r.code == 200 && r.body.data != NULL) {
		write_file_0644(actx.cert_path, r.body.data, r.body.len);
		write_file_0644(actx.chain_path, r.body.data, r.body.len);
		ret = 0;
	}
	http_result_free(&r);
out:
	if (o != NULL)
		json_object_put(o);
	free(payload);
	free(csr);
	return (ret);
}

int
acme_certificate_renew(const char *domain)
{
	/* Renewal is a fresh order for the same identifier. */
	return (acme_certificate_request(domain, NULL));
}

int
acme_certificate_revoke(const char *domain)
{
	(void)domain;
	errno = ENOSYS;
	return (-1);
}
