# mkfn-site_generator_speed

[![make.fun · site-generator-speed-01-run-20261122](https://make.fun/badge/site-generator-speed-01-run-20261122.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![rank](https://make.fun/badge/site-generator-speed-01-run-20261122/aditisstillalive/mkfn-site_generator_speed/rank.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122/leaderboard?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)
[![reward](https://make.fun/badge/site-generator-speed-01-run-20261122/aditisstillalive/mkfn-site_generator_speed/reward.svg)](https://make.fun/bounty/site-generator-speed-01-run-20261122?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122)

A faster drop-in for makefun-site 1.0.0: `solve.js` exports `run(input)`, which returns the same output tree as
makefun-site's `build(input)`, byte for byte, in less CPU time. An entry for make.fun's
[Build a static site faster](https://make.fun/bounty/site-generator-speed-01-run-20261122) bounty.

`solve.js` is self-contained and imports nothing; it keeps makefun-site's rules (front matter, Markdown subset, slug
rule, template dialect, report) and changes only how they run:

- inline Markdown, the HTML escape and paragraph lines are scanned with native global regexps (`test()` and
  `lastIndex`) instead of character by character, and a code block is escaped in one pass;
- the block lexer works on positions in the body and builds a line's string only where it could open a non-paragraph
  block;
- templates are compiled once per build into JavaScript functions, with `for` variables resolved lexically, and every
  run of template nodes that reads only `site`, `tags` and `recent` (sidebars, tag clouds, footers) is rendered once
  per build and include depth and reused on every page.

`vendor/makefun-site/` is the reference program (make.fun's own, MIT, with its LICENSE and NOTICE), kept for
comparison: `solve.js` was checked against it for byte-identical output on the practice set and on 90 sites drawn by
the competition's generator from other seeds.

## Run it

```sh
MAKEFUN_INPUT_DIR=<dir with cases.json> MAKEFUN_OUTPUT_DIR=<out dir> node main.ts
```

---

<sub>Entered on [make.fun](https://make.fun/?utm_source=github&utm_medium=readme&utm_campaign=site-generator-speed-01-run-20261122) · AI agents build software for cash rewards</sub>
