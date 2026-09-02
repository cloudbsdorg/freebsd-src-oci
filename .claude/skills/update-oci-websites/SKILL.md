---
name: update-oci-websites
description: Update the live CloudBSD OCI WordPress sites (ocisingle + ocicluster), keep all content current with the platform, and verify every change renders correctly. Use whenever ocifbsd features, deployment, or test results change, or when asked to edit the sites.
---

# Updating the CloudBSD OCI websites

Two live, public, cyberpunk-themed WordPress sites document the ocifbsd platform
and run **entirely inside it**. Any change to the platform (new feature, test
result, topology change) must be reflected on **both** sites and then verified.
Content is never "done" — keep it current.

## The two sites (always update BOTH)

| Site        | Host / node        | URL                          | Skin  |
|-------------|--------------------|------------------------------|-------|
| single VM   | `root@192.168.1.240` | https://ocisingle.cloudbsd.org  | cyan  |
| cluster     | `root@192.168.1.241` (+ replicas .242/.243) | https://ocicluster.cloudbsd.org | magenta |

WordPress runs in a jail on each node; nginx serves from a **separate** nginx
jail. The ingress is freedev007 (TLS + SNI). Cloudflare proxies the public
names (orange cloud) — so the public sites are dual-stack v4/v6 via CF.

## Editing pages / posts (wp-cli via jexec)

```sh
wj=$(jls jid path 2>/dev/null | awk '/local.wordpress/{print $1}' | head -1)
wp_path=$(jls -j $wj path 2>/dev/null)
WP="wp --path=/usr/local/www/wordpress --allow-root"
# list:   jexec $wj $WP post list --post_type=post   --fields=ID,post_title
#         jexec $wj $WP post list --post_type=page   --fields=ID,post_title
# update: write HTML to a file, copy it INTO the jail, then update by ID:
cp /tmp/body.html ${wp_path}/tmp/body.html
jexec $wj $WP post update <ID> /tmp/body.html
# create page: jexec $wj $WP post create /tmp/body.html --post_type=page \
#              --post_status=publish --post_title="Title"
```
Publishing a Page auto-adds it to the nav (theme uses a page-list).

## Content rules (learned the hard way)

- **wpautop mangles inline `<script>`/`<style>`** — any custom HTML/JS/CSS block
  MUST be wrapped in `<!-- wp:html -->` … `<!-- /wp:html -->`. Prose uses
  `<!-- wp:paragraph -->`, headings `<!-- wp:heading -->`, code
  `<!-- wp:code --><pre class="wp-block-code"><code>…</code></pre><!-- /wp:code -->`.
- **JSON shown to humans is pretty-printed** (2-space indent, one key/line);
  only streaming JSONL stays compact.
- **Cyberpunk theme** is a must-use plugin (`wp-content/mu-plugins/`): the loader
  `oci-cyberpunk.php` + `cyberpunk.css`. The CSS must ALSO be copied into the
  **nginx** jail's webroot (`.../local/nginx/*/rootfs/usr/local/www/wordpress/wp-content/mu-plugins/cyberpunk.css`),
  because nginx serves static files from its own container — otherwise the
  stylesheet 404s. Bump the `?ver=` in the loader when the CSS changes.
- Cluster gets `oci-cluster-skin` (magenta) via a `body_class` filter on the
  `cluster` hostname; single stays cyan.
- The Machine Room dashboard + Cluster Map read live JSON feeds served as static
  files from the nginx webroot: per-node `oci-stats.json` and cluster-wide
  `oci-cluster.json` (written by the aggregator on freedev007,
  `/usr/local/bin/oci-cluster-map.sh`). The cluster "Container Jails" panel and
  map must use the **aggregate** feed so they are consistent across replicas,
  and show node + image + replica N-of-M per container.

## After editing the CLUSTER: purge the FastCGI cache on all 3 nodes

```sh
for ip in 241 242 243; do ssh root@192.168.1.$ip \
  'nj=$(jls jid path 2>/dev/null|awk "/local.nginx/{print \$1}"|head -1); \
   njp=$(jls -j $nj path 2>/dev/null); rm -rf ${njp}/var/tmp/wpcache/* 2>/dev/null'; done
```

## Verify EVERY change (do not skip)

1. **HTTP:** `curl -skL` (follow redirects — the front page 301s to `/`), check
   `http_code=200` and grep for expected new strings. Do this against BOTH sites.
2. **Render:** headless Chrome on **freedev002** (Firefox headless is too slow):
   ```sh
   ssh mlapointe@freedev002.cloudbsd.org 'rm -rf /tmp/cr; mkdir -p /tmp/cr; \
     timeout 70 chrome --headless --disable-gpu --no-sandbox --hide-scrollbars \
     --user-data-dir=/tmp/cr --window-size=1400,2400 \
     --screenshot=/tmp/shot.png "https://ocicluster.cloudbsd.org/<path>/"'
   ```
   then `scp` it back and actually look at it — confirm the theme, readability,
   code blocks, and that no panel is stuck "loading".
3. Confirm the two sites match (same content on 240 and 241) and that the blog
   Query Loop actually lists the posts.

## Keeping content current

Whenever the platform changes, update the relevant surface AND verify:
- new/changed feature → update **Examples** page and add or expand a **blog post**;
- topology/among-nodes change → update the **Cluster Map** and the Machine Room
  **Container Jails** aggregation;
- **load/stress test → always post the detailed results on the blog** (numbers,
  where it broke, what held);
- new pages must appear in nav on both sites.

Treat "publish + purge cache + curl-verify + screenshot-verify on both sites" as
the definition of done for any site edit.

## Full end-to-end (E2E) test

Run a complete E2E pass after any significant change, and as the final gate
before declaring work done. It exercises the whole stack — DNS → Cloudflare →
ingress → proxy → replica → PHP-FPM → MariaDB/Redis → rendered page — on BOTH
sites, over BOTH address families, and checks the platform's self-healing.

1. **Reachability, dual-stack:** for each site, `curl -skL` over `-4` and `-6`
   and assert `200`. (Public v6 is via Cloudflare's edge.)
2. **Every page and post:** enumerate with wp-cli
   (`post list --post_type=page` and `--post_type=post`), then fetch each
   permalink and assert `200`, non-trivial size, and that the Query Loop on
   `/blog/` actually lists the posts (TT5 renders them as
   `<li class="wp-block-post …">`, not `<article>`).
3. **Nav + no dead links:** confirm the header/footer nav links resolve (no
   `href="#"` placeholders) and each target returns `200`.
4. **Live data feeds:** fetch `oci-cluster.json` (assert all cluster nodes and
   their containers, node+image+replica present) and `oci-stats.json`; load the
   Machine Room + Cluster Map and confirm the tables populate (never stuck on
   "loading") and the numbers are consistent regardless of which replica served.
5. **Render:** headless-Chrome screenshot the front page, `/blog/`, one post,
   `/examples/`, and (cluster) `/cluster-map/` — and actually view each for
   theme, readability, and populated panels.
6. **Session migration:** confirm a session counter climbs across different
   nodes under one session id (Redis-backed).
7. **Failover / self-healing** (uses the `oci-failover-test` skill — power a
   VM off, never stop containers): with a node down, assert the site still
   serves `200` and the proxy short-circuits the dead backend (latency stays
   low after ~3 detections); power it back on and assert the supervisor
   auto-restores its containers + pod gateway and the proxy folds it back in.
8. **Load/stress:** ramp concurrency until something breaks, record the numbers,
   and **post the detailed results on the blog**. Turn Cloudflare's proxy OFF
   for the duration of a load test and back ON afterward (see the
   `load-test-toggle-cloudflare-proxy` memory).

A change is E2E-verified only when steps 1–7 pass on both sites and any load
test from step 8 is written up on the blog.
