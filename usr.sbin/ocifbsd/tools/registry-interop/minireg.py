#!/usr/bin/env python3
# Minimal Docker Registry v2 (pull + push) for exercising ocifbsd against a
# real registry on an isolated LAN. Not production: no auth, single in-memory
# repo namespace, files on disk. Serves a skopeo `dir:` layout for pull and
# accepts blob/manifest PUTs for push (stored under <base>/_pushed).
import hashlib
import http.server
import json
import os
import re
import sys
import uuid

BASE = sys.argv[1] if len(sys.argv) > 1 else "."
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5000
DIRLAYOUT = os.path.join(BASE, "hello")          # skopeo dir: layout for pull
PUSHED = os.path.join(BASE, "_pushed")           # push target
UPLOADS = os.path.join(BASE, "_uploads")
os.makedirs(PUSHED, exist_ok=True)
os.makedirs(UPLOADS, exist_ok=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return "sha256:" + h.hexdigest()


def load_pull_manifest():
    with open(os.path.join(DIRLAYOUT, "manifest.json"), "rb") as f:
        data = f.read()
    digest = "sha256:" + hashlib.sha256(data).hexdigest()
    mtype = json.loads(data).get(
        "mediaType", "application/vnd.oci.image.manifest.v1+json")
    return data, digest, mtype


class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        sys.stderr.write("reg: " + (a[0] % a[1:]) + "\n")

    def _send(self, code, body=b"", ctype=None, extra=None):
        self.send_response(code)
        if ctype:
            self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body and self.command != "HEAD":
            self.wfile.write(body)

    # ---- pull ----
    def do_GET(self):
        self._get_or_head()

    def do_HEAD(self):
        self._get_or_head()

    def _get_or_head(self):
        p = self.path.split("?", 1)[0]
        if p == "/v2/" or p == "/v2":
            return self._send(200, b"{}", "application/json")
        m = re.match(r"^/v2/(.+)/manifests/(.+)$", p)
        if m:
            # Prefer a manifest previously pushed to this tag (round-trip);
            # otherwise serve the skopeo dir: layout used to seed pulls.
            pushed = os.path.join(PUSHED, "manifest-" + m.group(2))
            if os.path.exists(pushed):
                with open(pushed, "rb") as f:
                    data = f.read()
                digest = "sha256:" + hashlib.sha256(data).hexdigest()
                mtype = json.loads(data).get(
                    "mediaType",
                    "application/vnd.oci.image.manifest.v1+json")
            else:
                data, digest, mtype = load_pull_manifest()
            return self._send(200, data, mtype,
                              {"Docker-Content-Digest": digest})
        m = re.match(r"^/v2/(.+)/blobs/(sha256:[0-9a-f]+)$", p)
        if m:
            hexd = m.group(2).split(":", 1)[1]
            # pull layout blobs are named by bare hex; pushed blobs too
            for cand in (os.path.join(DIRLAYOUT, hexd),
                         os.path.join(PUSHED, hexd)):
                if os.path.exists(cand):
                    with open(cand, "rb") as f:
                        body = f.read()
                    return self._send(
                        200, body, "application/octet-stream",
                        {"Docker-Content-Digest": m.group(2)})
            return self._send(404, b"blob unknown")
        return self._send(404, b"not found")

    # ---- push ----
    def _abs(self, path):
        host = self.headers.get("Host", "127.0.0.1:%d" % PORT)
        return "http://%s%s" % (host, path)

    def do_POST(self):
        p = self.path.split("?", 1)[0]
        m = re.match(r"^/v2/(.+)/blobs/uploads/?$", p)
        if m:
            uid = uuid.uuid4().hex
            open(os.path.join(UPLOADS, uid), "wb").close()
            # Location carries a query so the client can append "&digest=..."
            # on the finalizing PUT (Docker Registry v2 upload flow).
            loc = self._abs("/v2/%s/blobs/uploads/%s?_state=open" % (
                m.group(1), uid))
            return self._send(202, b"", None,
                              {"Location": loc,
                               "Docker-Upload-UUID": uid,
                               "Range": "0-0"})
        return self._send(404, b"not found")

    def do_DELETE(self):
        # Upload cancel or blob delete — acknowledge so the client proceeds.
        return self._send(202, b"")

    def _read_body(self):
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n) if n else b""

    def do_PATCH(self):
        p = self.path.split("?", 1)[0]
        m = re.match(r"^/v2/(.+)/blobs/uploads/([0-9a-f]+)$", p)
        if m:
            with open(os.path.join(UPLOADS, m.group(2)), "ab") as f:
                f.write(self._read_body())
            loc = self._abs("/v2/%s/blobs/uploads/%s" % (m.group(1),
                                                         m.group(2)))
            return self._send(202, b"", None,
                              {"Location": loc,
                               "Docker-Upload-UUID": m.group(2),
                               "Range": "0-0"})
        return self._send(404, b"not found")

    def do_PUT(self):
        p = self.path.split("?", 1)[0]
        q = self.path.split("?", 1)[1] if "?" in self.path else ""
        # blob upload PUT. Two shapes in the Docker Registry v2 session flow:
        #   - data PUT   (no digest in query): append the bytes, return 202.
        #   - finalize   (?...&digest=sha256:..): commit the blob, return 201.
        m = re.match(r"^/v2/(.+)/blobs/uploads/([0-9a-f]+)$", p)
        if m:
            body = self._read_body()
            up = os.path.join(UPLOADS, m.group(2))
            with open(up, "ab") as f:
                f.write(body)
            dm = re.search(r"digest=(sha256:[0-9a-f]+)", q)
            if dm is None:
                # data chunk accepted; upload still open
                loc = self._abs("/v2/%s/blobs/uploads/%s?_state=open" % (
                    m.group(1), m.group(2)))
                return self._send(202, b"", None,
                                  {"Location": loc,
                                   "Docker-Upload-UUID": m.group(2),
                                   "Range": "0-0"})
            digest = dm.group(1)
            hexd = digest.split(":", 1)[1]
            os.replace(up, os.path.join(PUSHED, hexd))
            return self._send(201, b"", None,
                              {"Docker-Content-Digest": digest,
                               "Location": "/v2/%s/blobs/%s" % (
                                   m.group(1), digest)})
        # manifest PUT
        m = re.match(r"^/v2/(.+)/manifests/(.+)$", p)
        if m:
            body = self._read_body()
            digest = "sha256:" + hashlib.sha256(body).hexdigest()
            with open(os.path.join(PUSHED, "manifest-" + m.group(2)),
                      "wb") as f:
                f.write(body)
            return self._send(201, b"", None,
                              {"Docker-Content-Digest": digest,
                               "Location": "/v2/%s/manifests/%s" % (
                                   m.group(1), m.group(2))})
        return self._send(404, b"not found")


if __name__ == "__main__":
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), H)
    sys.stderr.write("minireg on 0.0.0.0:%d base=%s\n" % (PORT, BASE))
    srv.serve_forever()
