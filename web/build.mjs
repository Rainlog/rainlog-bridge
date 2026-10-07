// Build the bridge config UI into ONE self-contained, minified HTML file that
// the firmware embeds (main/web/index.html). Steps:
//   1. bundle + minify src/app.ts  (esbuild, IIFE, no globals needed)
//   2. minify src/style.css        (esbuild css transform)
//   3. inline the favicon as a data URI
//   4. splice all three into src/index.html, then minify the whole document
//   5. gzip each page with 100 Zopfli iterations for firmware embedding
//
// The generated file is committed so the Docker ESP-IDF build never needs Node;
// re-run `npm run build` here whenever the src/ files change.

import { build, transform } from 'esbuild';
import { minify } from 'html-minifier-terser';
import { gzipAsync } from '@gfx/zopfli';
import { gunzipSync } from 'node:zlib';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const src = (p) => resolve(here, 'src', p);
for (const radioMHz of [0, 433]) {
  const outFile = resolve(
    here,
    '..',
    'main',
    'web',
    ...(radioMHz ? [`radio${radioMHz}`] : []),
    'index.html',
  );

  const js = (
    await build({
      entryPoints: [src('app.ts')],
      define: { RADIO_MHZ: String(radioMHz) },
      bundle: true,
      loader: { '.svg': 'text' },
      minify: true,
      format: 'iife',
      target: 'es2018',
      write: false,
    })
  ).outputFiles[0].text.trim();

  const css = (
    await transform(readFileSync(src('style.css'), 'utf8'), {
      loader: 'css',
      minify: true,
    })
  ).code.trim();

  const favicon =
    'data:image/png;base64,' +
    readFileSync(src('favicon.png')).toString('base64');

  // Splice patterns are quote-tolerant so prettier may reformat src/index.html
  // (it quotes attributes) without breaking the build.
  let html = readFileSync(src('index.html'), 'utf8');
  if (!radioMHz)
    html = html.replace(/<!-- RADIO_BEGIN -->[\s\S]*?<!-- RADIO_END -->/g, '');
  html = html.replace(
    /<link\s+rel=["']?stylesheet["']?[^>]*>/i,
    `<style>${css}</style>`,
  );
  // Both uses: the <link rel=icon> in head and the logo <img> in the header.
  html = html.replace(/["']?\.\/favicon\.png["']?/g, `"${favicon}"`);
  html = html.replace(
    /<script[^>]*src=["']?\.\/app\.ts["']?[^>]*><\/script>/i,
    `<script>${js}</script>`,
  );

  // Fail loudly if a splice missed (e.g. a markup tweak renamed a placeholder),
  // rather than shipping a page that pulls /style.css or /app.ts the firmware no
  // longer serves.
  for (const [needle, what] of [
    ['./style.css', 'CSS link'],
    ['./app.ts', 'script src'],
    ['./favicon.png', 'favicon src'],
  ]) {
    if (html.includes(needle)) {
      throw new Error(`build: ${what} (${needle}) was not inlined`);
    }
  }

  html = await minify(html, {
    collapseWhitespace: true,
    removeComments: true,
    removeAttributeQuotes: true,
    collapseBooleanAttributes: true,
    // app.ts / style.css are already minified by esbuild; don't double-process.
    minifyJS: false,
    minifyCSS: false,
  });

  mkdirSync(dirname(outFile), { recursive: true });
  const page = Buffer.from(html + '\n');
  const compressed = await gzipAsync(page, { numiterations: 100 });
  if (!gunzipSync(compressed).equals(page))
    throw new Error(`build: gzip round-trip failed for ${outFile}`);
  writeFileSync(outFile, page);
  writeFileSync(outFile + '.gz', compressed);
  console.log(`wrote ${outFile} (${page.length} bytes, ${compressed.length} gzip bytes, 100 Zopfli iterations)`);
}
