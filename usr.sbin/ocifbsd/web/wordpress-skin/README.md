<!-- Copyright (c) 2026 REVYTECH, Inc. — Mark LaPointe <mark@revytechinc.com> -->
# CloudBSD OCI — professional WordPress skin

The enterprise-dark CloudBSD/RevyTech brand skin for the two OCI showcase sites,
`ocicluster.cloudbsd.org` (3-node HA) and `ocisingle.cloudbsd.org` (single node).
It replaces the earlier "cyberpunk" theme with a professional look modelled on
`cloudbsd.org` and `revytechinc.com`: deep slate-navy canvas (`#0f172a`), brand
blue `#00529b` with subtle cyan/blue glow accents, Outfit headings + Inter body,
sticky glass nav, and `rounded-2xl` glass cards.

## Files

- `oci-professional.php` — a WordPress **mu-plugin** (loads automatically). It
  enqueues Google Fonts (Inter + Outfit) and `professional.css` with a
  `filemtime`-based cache-busting version, adds the `oci-pro-skin` +
  `oci-cluster`/`oci-single` body classes, injects the top ribbon (the node
  badge — three pulsing dots for the cluster, one for the single node — plus the
  cross-site switcher), and renders the footer with a **viewer-local-time**
  "last updated" line and the **REVYTECH, Inc. / Mark LaPointe** attribution.
- `professional.css` — the skin itself. Uses `!important` deliberately to
  override both the twentytwentyfive block theme's light palette and the
  "Machine Room" dashboard component's own inline red/white styles.
- `nodedeploy.sh` — runs **on a node**; copies `professional.css` into both that
  node's WordPress **and** nginx pods (nginx serves the stylesheet as a static
  asset from its own filesystem — the two are separate containers), installs the
  mu-plugin into the WordPress pod, disables the old `oci-cyberpunk.php`, and
  clears the FastCGI cache.

## Deploy

From the vm-bhyve host, for each node (single: `192.168.1.240`; cluster:
`192.168.1.241`–`.243`), copy `professional.css`, `oci-professional.php`, and
`nodedeploy.sh` to the node's `/tmp`, then `sh /tmp/nodedeploy.sh`. Verify with a
headless-browser screenshot of each site (Playwright has no FreeBSD build; use
headless Chrome/Firefox), checking desktop and mobile.

The CSS must land in the **nginx** pod, not only the WordPress pod — otherwise
the `<link>` 404s to an HTML page and no styling applies.
