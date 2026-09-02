<!--
SPDX-License-Identifier: BSD-2-Clause
Copyright (c) 2026 REVYTECH, Inc.
-->
# Registry interop test fixture

Dev/CI tooling to exercise `ocifbsd pull` and `ocifbsd push` against a real
Docker Registry v2 **without internet access to Docker Hub** — useful on
isolated lab networks. This is not part of the runtime or its self-contained
base build; it is a test fixture.

## Pieces

- `minireg.py` — a minimal Docker Registry v2 (pull + push) in Python 3. It
  serves a [skopeo](https://github.com/containers/skopeo) `dir:` layout for
  pulls and accepts the standard chunked blob-upload + manifest flow for
  pushes (`POST` upload → `PUT` data `202` → `PUT ?digest` `201` →
  `PUT manifest` `201`). No auth, single namespace, files on disk. **Test use
  only.**
- `roundtrip.sh` — runs pull → push → wipe → pull-back → verify on the ocifbsd
  host.

## Usage

On a host the ocifbsd machine can reach (needs `python3`; `skopeo` to seed):

```sh
# seed a small image into a dir: layout
mkdir -p reg
skopeo copy --override-os linux --override-arch amd64 \
    docker://docker.io/library/hello-world:latest dir:reg/hello

# serve it (binds 0.0.0.0:5000)
python3 minireg.py reg 5000
```

On the ocifbsd host, mark the registry insecure and run the round-trip:

```sh
printf '%s %s - - http\n' 192.168.1.154:5000 192.168.1.154:5000 \
    >> /etc/ocifbsd/registries.conf
OCIFBSD=/path/to/ocifbsd \
    ./roundtrip.sh 192.168.1.154:5000 library/hello-world latest
```

A `PASS:` line means the image survived a full push/pull round-trip through the
registry and the rootfs was reconstructed.

## What it proves

`ocifbsd` resolves a reference, fetches and digest-verifies the manifest,
config, and layers, unpacks the layers into the store (`pull`), and uploads the
layers, config blob, and manifest back over the registry-v2 protocol (`push`) —
honoring insecure (plain-HTTP) registries. Verified end-to-end on a FreeBSD
16-CURRENT lab host.
