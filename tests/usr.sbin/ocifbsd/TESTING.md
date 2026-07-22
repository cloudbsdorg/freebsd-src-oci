# ocifbsd test matrix

How we exercise **all OCI work shipped so far** (Phase 1 lifecycle + Phase 2 images).

## Layers

| Layer | Tool | What it proves |
|-------|------|----------------|
| Unit (C ATF) | `kyua test` programs under this dir | Pure helpers, parsers, digests, unpack |
| CLI / failure | `cli_test` | Binary flags, bad refs, missing image/store |
| Integration (root) | `lifecycle_test` | create → start → kill → delete jail |
| Stress | `tools/ocifbsd-stress.sh` | dry-run loops, rmi churn, optional live pull |
| Regression | `tools/ocifbsd-regression.sh` | build + kyua + stress + artifacts |

## Unit programs

| Program | Surface |
|---------|---------|
| `image_ref_test` | `parse_reference`, paths, digests, stress 1000× |
| `image_parse_test` | `parse_manifest`, `parse_config`, `verify_layer`, `registry_init` |
| `unpack_layer_test` | real gzip tar extract, dirname safety, missing tar |
| `whiteout_test` | `.wh.*` name helpers |
| `oci2jail_test` | OCI runtime config → jail params |
| `hooks_test` / `utils_test` | hooks + container id helpers |
| `parser_test` / `k8s_test` | convert helpers |

## Failure cases (CLI)

- empty / invalid image reference  
- pull unreachable registry (DNS fail)  
- rmi missing image  
- create/run `--image` when store not ready  
- create missing bundle  
- start/state/kill/delete unknown id  
- unknown subcommand  

## Stress

```sh
# on FreeBSD lab host
sh tools/ocifbsd-stress.sh
ITER=500 LIVE_PULL=1 sh tools/ocifbsd-stress.sh
```

Artifacts: `artifacts/stress/<gitsha>/RESULT.txt`

## Quality checks already applied

- Bearer tokens must use `asprintf` (Hub JWTs >2KB)  
- `dirname(3)` must copy path before open (unpack)  
- curl header callbacks must not mutate curl buffers  

## Gaps / next

- ZFS-backed store unit tests (dataset create/destroy)  
- push path still scaffold  
- multi-layer whiteout *apply* end-to-end (filesystem)  
- concurrent pull stress under rate limits  
