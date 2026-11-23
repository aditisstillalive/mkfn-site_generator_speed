// What main.ts runs (main.ts only imports this file: Node strips TypeScript from main.ts, and the less text it holds,
// the less start-up time that costs). site.node (native/site.c, makefun-site's build in C) reads cases.json and writes
// answers.json itself, then ends the process. A case it leaves, or an addon that cannot load, sends every case to
// solve.js's run(), makefun-site's build in JavaScript (README.md). No imports on the way through: they cost start-up
// time too. Outside a container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and /output.
const cases = (process.env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const answers = (process.env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';
/** A run of characters outside ASCII as makefun-site's slug reads it: NFKD, its combining marks dropped. */
const plainOf = (run) => run.normalize('NFKD').replace(/\p{M}/gu, '');
let done = false;
try {
  const addon = { exports: {} };
  process.dlopen(addon, new URL('./site.node', import.meta.url).pathname);
  done = addon.exports.build(cases, plainOf, answers, true) === true;
} catch {
  done = false;
}
if (!done) {
  const fs = process.getBuiltinModule('node:fs');
  const { run } = await import(new URL('./solve.js', import.meta.url).href);
  fs.writeFileSync(answers, JSON.stringify(JSON.parse(fs.readFileSync(cases, 'utf8')).map((c) => run(c.input))));
}
