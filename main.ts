// The harness runs `node /bot/main.ts` (node22). It reads every case from /input/cases.json (a JSON array of
// {id, input}), answers each one, and writes /output/answers.json: a JSON array of the answers, one per case and in the
// cases' order. Outside a container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and /output.
//
// The answers come from site.node (native/site.c: makefun-site's build in C), which reads cases.json's bytes and
// writes answers.json itself, case by case. Where a case holds anything the C port leaves (see native/site.c), or the
// addon cannot load, every case is answered by solve.js's run(input), makefun-site's build in JavaScript, which is only
// imported then, and the file is written again. Nothing is imported on the way through (each import costs start-up
// time, which the entry is timed on): node's modules come from process.getBuiltinModule.

const fs = process.getBuiltinModule('node:fs');
const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

/** A run of characters outside ASCII as makefun-site's slug reads it: NFKD, its combining marks dropped. */
const plainOf = (run: string): string => run.normalize('NFKD').replace(/\p{M}/gu, '');

const casesFile = `${INPUT_DIR}/cases.json`;
const answersFile = `${OUTPUT_DIR}/answers.json`;
let done = false;
try {
  // the addon maps cases.json and writes answers.json itself
  const addon = { exports: {} as { build?: (cases: string, plainOf: (run: string) => string, answers: string) => boolean } };
  process.dlopen(addon, new URL('./site.node', import.meta.url).pathname);
  done = addon.exports.build?.(casesFile, plainOf, answersFile) === true;
} catch {
  done = false;
}

if (!done) {
  // the addon did not load, or left a case: every answer from run(), the file written again from its start
  const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };
  const cases = JSON.parse(fs.readFileSync(casesFile, 'utf8')) as { id: string; input: unknown }[];
  fs.writeFileSync(answersFile, JSON.stringify(cases.map((c) => run(c.input))));
}
