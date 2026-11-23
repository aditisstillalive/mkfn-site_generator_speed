# mkfn-site_generator_speed

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/aditisstillalive/mkfn-site_generator_speed/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/aditisstillalive/mkfn-site_generator_speed/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0: the same output tree as makefun-site's `build(input)` for every case, byte
for byte, in a fraction of its CPU time. An entry for make.fun's
[Build a static site faster](https://make.fun/bounty/site-generator-speed-01-run-20261122) bounty.

## How it runs

- **`native/site.c`** is makefun-site's pipeline (front matter, permalinks, its Markdown subset, collections, tags,
  its template dialect, the report) ported to freestanding C, with no libc, and built into `site.node`, a Node-API
  addon (`native/build.sh`, x86-64 Linux). `run.js` loads it with `process.dlopen` and the addon does the rest:
  it maps `cases.json`, parses each case's JSON itself, builds the site, and writes each answer to `answers.json`
  already serialized as JSON. No `JSON.parse` of the input, no JavaScript object tree, no `JSON.stringify`.
- Inside, text stays UTF-8 and is scanned with SSE2: JSON strings, inline Markdown (one bitmask of special bytes
  per paragraph), HTML and JSON escapes. Markdown is rendered straight into its JSON-escaped form, so a page's
  content is copied, not escaped again. Template runs that read only `site`, `tags` and `recent` are rendered once
  per site and include depth, and a `for` body that reads only its element (a post's card) once per element.
- One rule needs Unicode data the C port does not carry: a slug's NFKD. For text outside ASCII the addon asks
  JavaScript (`String.prototype.normalize`) once per distinct run of such characters.
- Exactness first: anything the port does not take on (a character outside the Basic Multilingual Plane, JavaScript
  whitespace outside ASCII, a lone surrogate, a duplicate template name...) makes the addon give the whole set back,
  and `solve.js`'s `run(input)` (makefun-site's rules in plain JavaScript, imports nothing) answers every case. The
  same happens if the addon cannot load.
- `main.ts` is one line, `import './run.js';`: Node strips TypeScript from `main.ts` before it runs, and the more
  text it holds the more start-up time that takes, which is part of the timed run. `run.js` imports nothing (an
  import costs start-up time too) and ends the process as soon as `answers.json` is written and closed.

`vendor/makefun-site/` is the reference program (make.fun's own, MIT, with its LICENSE and NOTICE), kept for
comparison. Both paths were checked against it for identical answers on the practice set and on 690 sites drawn by
the competition's generator from other seeds, and two runs write the same bytes.

To rebuild the addon: `native/build.sh` (gcc, x86-64; needs `node_api.h`, e.g. from `/usr/include/node`).

## Run it

```sh
MAKEFUN_INPUT_DIR=<dir with cases.json> MAKEFUN_OUTPUT_DIR=<out dir> node main.ts
```

---

<sub>Entered on [make.fun](https://make.fun/?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
