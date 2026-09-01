# ocifbsd manual-page website

A small static website that presents the `ocifbsd` manual pages as linked,
styled HTML. It is generated from the manual-page sources in the ocifbsd tree,
so it always reflects the committed documentation.

## Build

```sh
sh generate.sh [output-dir]      # default: ./ocifbsd-man-site
```

`generate.sh` renders every section-8 and section-3 manual page under the
ocifbsd tree with `mandoc -T html` (base system; no external tools), then adds
the curated `index.html` landing page and `man.css` stylesheet. New manual
pages are discovered automatically — there is no page list to maintain. The
vendored `contrib/` copies are skipped.

## Files

- `index.html` — curated landing page: overview, feature highlights, a
  theme-aware inline SVG architecture diagram, and a linked index of every page.
- `man.css` — stylesheet shared by the landing page and the rendered manual
  pages; supports light and dark color schemes via `prefers-color-scheme`.
- `generate.sh` — the generator.

The output is a self-contained directory of static files: open `index.html`
directly in a browser, or serve the directory with any web server.
