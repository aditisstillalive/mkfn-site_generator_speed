// What main.ts runs (main.ts only imports this file: Node strips TypeScript from main.ts, and the less text it holds,
// the less start-up time that costs). It reads every case from cases.json, answers each with solve.js's run(), and
// writes answers.json, each answer as it comes. site.node (native/site.c) does the reading and the writing where it
// loads, and run() builds each case with it where it can (README.md). Outside a container MAKEFUN_INPUT_DIR and
// MAKEFUN_OUTPUT_DIR stand in for /input and /output.
import { run, readCases, openAnswers, writeAnswer, closeAnswers } from './solve.js';

const casesPath = (process.env.MAKEFUN_INPUT_DIR ?? '/input') + '/cases.json';
const answersPath = (process.env.MAKEFUN_OUTPUT_DIR ?? '/output') + '/answers.json';
const fs = process.getBuiltinModule('node:fs');
const cases = readCases(casesPath) ?? JSON.parse(fs.readFileSync(casesPath, 'utf8'));
let written = openAnswers(answersPath);
if (written) {
  for (const c of cases) if (!writeAnswer(run(c.input))) written = false;
  written = closeAnswers(written) && written;
}
if (!written) fs.writeFileSync(answersPath, JSON.stringify(cases.map((c) => run(c.input))));
