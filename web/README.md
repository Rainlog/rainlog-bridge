# Rainlog Bridge config web UI

The SoftAP setup page (TypeScript + CSS + favicon), built into a single
self-contained, minified `../main/web/index.html` that the firmware embeds and
serves at `/`.

## Build

```sh
npm install      # once
npm run build    # regenerates ../main/web/index.html
npm run typecheck
```

`build.mjs` bundles + minifies `src/app.ts` (esbuild), minifies `src/style.css`,
inlines the favicon as a `data:` URI, splices all three into `src/index.html`,
and minifies the document.

## Why a single static file

The firmware serves `/` as static bytes (no server-side templating). The page
fetches current settings from the firmware's `/config` JSON endpoint at load and
fills the form client-side. Secrets are never returned — only "is a value set"
hints for the password placeholders.

The generated `index.html` is **committed**: the firmware build runs in the
pinned ESP-IDF Docker image, which has no Node. Re-run `npm run build` and commit
the regenerated file whenever you change anything under `src/`.
