// The harness runs `node /bot/main.ts`. site.node (native/site.c, makefun-site's build in C) reads cases.json and
// writes answers.json itself, then ends the process; a case it leaves, or an addon that cannot load, sends every case
// to solve.js's run() (README.md). Plain JavaScript, no imports: type syntax and imports cost start-up time.
const fs = process.getBuiltinModule('node:fs');
const cases = (process.env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const answers = (process.env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';
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
  const { run } = await import(new URL('./solve.js', import.meta.url).href);
  fs.writeFileSync(answers, JSON.stringify(JSON.parse(fs.readFileSync(cases, 'utf8')).map((c) => run(c.input))));
}
