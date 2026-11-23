# mkfn-site_generator_speed

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/aditisstillalive/mkfn-site_generator_speed/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/aditisstillalive/mkfn-site_generator_speed/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0: the same output tree as makefun-site's `build(input)` for every case, byte
for byte, in a fraction of its CPU time. An entry for make.fun's
[Build a static site faster](https://make.fun/bounty/site-generator-speed-01-run-20261122) bounty.

## How it runs

As the rules ask, `main.ts` reads every case from `cases.json`, answers each with `solve.js`'s `run(input)`, and
writes `answers.json`. The speed comes from what `run` builds with:

- **`native/site.c`** is makefun-site's pipeline (front matter, permalinks, its Markdown subset, collections, tags,
  its template dialect, the report) ported to freestanding C, with no libc, and built into `site.node`, a Node-API
  addon (`native/build.sh`, x86-64 Linux). `solve.js` loads it with `process.dlopen`.
- **Reading.** `readCases` (in the addon) maps `cases.json` and parses it itself into the cases' JavaScript values,
  frozen as they are made, each input remembering the parsed value it came from. ASCII strings are handed to V8 as
  external strings over the mapped file, without a copy.
- **`run(input)`** asks the addon's `buildOne` for the answer: it builds from the input's parsed value (or, for an
  input made elsewhere, from the object itself) and returns the answer object, its strings external strings decoded
  from the JSON text the build writes. That text is kept with the answer (until the next build) so that it can be
  written as it is. Where the addon does not take a case on, `run` builds it in plain JavaScript (makefun-site's
  rules, nothing imported).
- **Writing.** Each answer goes to `answers.json` as soon as `run` returns it: as its kept JSON text where it has
  one, else as `JSON.stringify`'s. When the last is written and the file closed, the process ends.
- Inside the build, text stays UTF-8 and is scanned with SSE2: JSON strings, inline Markdown (one bitmask of special
  bytes per paragraph), HTML and JSON escapes. Markdown is rendered straight into its JSON-escaped form. Template
  runs that read only `site`, `tags` and `recent` are rendered once per site and include depth, and a `for` body
  that reads only its element (a post's card) once per element.
- One rule needs Unicode data the C port does not carry: a slug's NFKD. For text outside ASCII the addon asks
  JavaScript (`String.prototype.normalize`) once per distinct run of such characters.
- Exactness first: anything the port does not take on (a character outside the Basic Multilingual Plane, JavaScript
  whitespace outside ASCII, a lone surrogate, a duplicate template name...) sends that case to the JavaScript build,
  and a `cases.json` the addon's parser leaves goes to `JSON.parse`. Without the addon, everything runs in JavaScript.
- `main.ts` is one line, `import './run.js';`: Node strips TypeScript from `main.ts` before it runs, and the more
  text it holds the more start-up time that takes, which is part of the timed run.

`vendor/makefun-site/` is the reference program (make.fun's own, MIT, with its LICENSE and NOTICE), kept for
comparison. Both paths were checked against it for identical answers on the practice set and on 870 sites drawn by
the competition's generator from other seeds, and two runs write the same bytes.

To rebuild the addon: `native/build.sh` (gcc, x86-64; needs `node_api.h`, e.g. from `/usr/include/node`).

## Run it

```sh
MAKEFUN_INPUT_DIR=<dir with cases.json> MAKEFUN_OUTPUT_DIR=<out dir> node main.ts
```

---

<sub>Entered on [make.fun](https://make.fun/?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
