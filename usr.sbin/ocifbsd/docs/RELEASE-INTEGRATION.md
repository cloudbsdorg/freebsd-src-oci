<!--
SPDX-License-Identifier: BSD-2-Clause
Copyright (c) 2026 REVYTECH, Inc.
-->
# Integrating ocifbsd with the base-system OCI image build

Task 5.5. The FreeBSD source tree already builds base OCI container images via
`release/Makefile.oci` and `release/scripts/make-oci-image.sh` (the `static`,
`dynamic`, `runtime`, `notoolchain`, and `toolchain` images). This documents
how `ocifbsd` consumes those artifacts — the two halves meet at the OCI image
layout, so no format translation is needed.

## What the release build produces

`make-oci-image.sh` writes a standard **OCI image layout**, xz-compressed into
`container-image-<image>.txz`:

```
oci-layout                      {"imageLayoutVersion":"1.0.0"}
index.json                      manifests[] -> the image manifest, with
                                org.opencontainers.image.ref.name
blobs/sha256/<config-digest>    OCI image config (arch, os=freebsd, diff_ids)
blobs/sha256/<manifest-digest>  the manifest (config + one gzip'd rootfs layer)
blobs/sha256/<layer-digest>     rootfs.tar.gz
```

## How ocifbsd consumes it

`ocifbsd load` reads exactly this layout: `load_oci_archive` opens the archive,
reads `index.json`, follows it to the manifest, verifies the config and layer
blobs by digest, and unpacks the layer into the image store. So a
release-built image is imported with a single command:

```sh
# after `make -C release WITH_OCIIMAGES=YES oci-release`
ocifbsd load --name freebsd-runtime:15.1 \
    /path/to/container-image-runtime.txz
ocifbsd images          # the imported image appears in the store
ocifbsd run --image freebsd-runtime:15.1 ...
```

## Verified

On the FreeBSD 16 lab host, a `.txz` assembled with the same steps as
`make-oci-image.sh` (`oci-layout` + `index.json` + `blobs/sha256/{config,
manifest,layer}`, xz-compressed) was imported with `ocifbsd load`: the image
was listed by `ocifbsd images` and its rootfs was unpacked into the store with
the expected files. This confirms `ocifbsd` and `release/Makefile.oci`
interoperate through the OCI image layout with no glue required — the release
target is the producer, `ocifbsd load` is the consumer.

## Note

`make-oci-image.sh` itself requires a populated `/usr/obj` package repo from a
full `buildworld` + `make packages`, so producing the real base images is part
of a release build; the consumption path above is what ocifbsd contributes and
is what this document verifies.
