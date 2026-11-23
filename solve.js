// A faster drop-in for makefun-site 1.0.0's build (vendor/makefun-site/, make.fun's own reference, MIT): run(input)
// returns the same output tree, byte for byte. The rules are makefun-site's own (its README.md and code); this module
// runs them with fewer allocations: Markdown and its inline syntax scanned by char codes, the HTML escape done in
// runs instead of per character, templates compiled once per build to JavaScript functions, and the parts of a
// template that read only the site-wide values (site, tags, recent) rendered once per build and reused.

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const hasOwn = Object.hasOwn;

// ---------------------------------------------------------------- text

/** The escape of each char code below 63 that the table names, else undefined. */
const ESC = [];
ESC[38] = '&amp;';
ESC[60] = '&lt;';
ESC[62] = '&gt;';
ESC[34] = '&quot;';
ESC[39] = '&#39;';

/** `s` with & < > " and ' escaped. */
function esc(s) {
  const n = s.length;
  let i = 0;
  for (; i < n; i++) {
    const c = s.charCodeAt(i);
    if (c < 63 && ESC[c] !== undefined) break;
  }
  if (i === n) return s;
  let out = '';
  let start = 0;
  for (; i < n; i++) {
    const c = s.charCodeAt(i);
    if (c < 63) {
      const r = ESC[c];
      if (r !== undefined) {
        out += s.slice(start, i) + r;
        start = i + 1;
      }
    }
  }
  return start === 0 ? out : out + s.slice(start);
}

const SLUG_MAX = 64;
const NON_ASCII = /[^\x00-\x7f]/;

/** makefun-site's slug of `text` (text.js slugify). */
function slugify(text) {
  if (NON_ASCII.test(text)) return slugifySlow(text);
  let out = '';
  let gap = false;
  const n = text.length;
  for (let i = 0; i < n; i++) {
    let c = text.charCodeAt(i);
    if (c >= 65 && c <= 90) c += 32;
    else if (!((c >= 97 && c <= 122) || (c >= 48 && c <= 57))) {
      gap = out.length > 0;
      continue;
    }
    if (gap) {
      out += '-';
      gap = false;
    }
    out += String.fromCharCode(c);
    if (out.length > SLUG_MAX) break;
  }
  if (out.length > SLUG_MAX) out = out.slice(0, SLUG_MAX);
  if (out.charCodeAt(out.length - 1) === 45) out = out.replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

/** The slug rule as text.js runs it, for text outside ASCII. */
function slugifySlow(text) {
  const plain = text.normalize('NFKD').replace(/\p{M}/gu, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) {
      gap = out.length > 0;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  out = out.slice(0, SLUG_MAX).replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

function longDate(date) {
  const [y, m, d] = date.split('-');
  return `${Number(d)} ${MONTHS[Number(m) - 1]} ${y}`;
}

/** Whether char code `c` is JavaScript whitespace (what \s matches). */
function isWs(c) {
  if (c <= 32) return c === 32 || (c >= 9 && c <= 13);
  if (c < 0xa0) return false;
  return c === 0xa0 || c === 0x1680 || (c >= 0x2000 && c <= 0x200a) || c === 0x2028 || c === 0x2029 || c === 0x202f || c === 0x205f || c === 0x3000 || c === 0xfeff;
}

/** The count of runs of non-whitespace in `text` (index.js wordsOf). */
function wordsOf(text) {
  let count = 0;
  let inWord = false;
  const n = text.length;
  for (let i = 0; i < n; i++) {
    const c = text.charCodeAt(i);
    if (c > 32 && c < 0xa0) {
      if (!inWord) {
        count++;
        inWord = true;
      }
    } else if (isWs(c)) inWord = false;
    else if (!inWord) {
      count++;
      inWord = true;
    }
  }
  return count;
}

// ---------------------------------------------------------------- front matter (frontmatter.js, as it is)

const FM_KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];
const FM_LINE = /^([a-z_]+):(?: (.*))?$/;
const FM_DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const FM_NAME = /^[a-z0-9-]+$/;
const FM_SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const FM_TAG = /^[^"[\]]+$/;
const BLANK_RE = /^[ \t]*$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

function stringOf(raw) {
  if (raw.startsWith('"')) {
    if (raw.length < 2 || !raw.endsWith('"')) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const n = raw[i + 1];
        if ((n !== '"' && n !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += n;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.startsWith('[')) return { error: 'a bare string opens on [' };
  return { value: raw };
}

function valueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = stringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      const m = FM_DATE.exec(raw);
      const y = m ? Number(m[1]) : 0;
      const mo = m ? Number(m[2]) : 0;
      const d = m ? Number(m[3]) : 0;
      if (!m || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      return { value: raw };
    }
    case 'tags': {
      if (!raw.startsWith('[') || !raw.endsWith(']')) return { error: 'key "tags": not a list [a, b]' };
      const inner = raw.slice(1, -1);
      if (inner.trim() === '') return { value: [] };
      const tags = inner.split(',').map((t) => t.trim());
      if (tags.some((t) => t === '' || !FM_TAG.test(t))) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout':
      return FM_NAME.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
    case 'slug':
      return FM_SLUG.test(raw) && raw.length <= 64 ? { value: raw } : { error: 'key "slug": not a slug' };
    default:
      return { error: `unknown key "${key}"` };
  }
}

/** frontmatter.js parseFrontMatter, finding the two "---" lines without splitting the whole text. */
function parseFrontMatter(text) {
  // lines[0] must be exactly "---".
  if (!(text.startsWith('---') && (text.length === 3 || text.charCodeAt(3) === 10))) return { error: 'no front matter' };
  // The first later line that is exactly "---".
  let closeStart = -1;
  let closeEnd = -1;
  let p = 3;
  while (p < text.length) {
    const q = text.indexOf('\n---', p);
    if (q === -1) break;
    const after = q + 4;
    if (after === text.length || text.charCodeAt(after) === 10) {
      closeStart = q + 1;
      closeEnd = after;
      break;
    }
    p = q + 1;
  }
  if (closeStart === -1) return { error: 'the front matter is not closed' };
  const lines = closeStart - 1 > 4 ? text.slice(4, closeStart - 1).split('\n') : closeStart - 1 === 3 ? [] : [text.slice(4, closeStart - 1)];
  const data = {};
  for (let k = 0; k < lines.length; k++) {
    const line = lines[k];
    if (BLANK_RE.test(line) || line.startsWith('#')) continue;
    const m = FM_LINE.exec(line);
    if (!m) return { error: `line ${k + 2} is not "key: value"` };
    const key = m[1];
    const raw = (m[2] ?? '').replace(/[ \t]+$/, '');
    if (!FM_KEYS.includes(key)) return { error: `unknown key "${key}"` };
    if (hasOwn(data, key)) return { error: `key "${key}" is given twice` };
    if (raw === '') return { error: `key "${key}" has no value` };
    const v = valueOf(key, raw);
    if ('error' in v) return v;
    data[key] = v.value;
  }
  if (!hasOwn(data, 'title')) return { error: 'no title' };
  return { data, body: closeEnd === text.length ? '' : text.slice(closeEnd + 1) };
}

// ---------------------------------------------------------------- Markdown (markdown.js, scanned by char codes)

const FENCE_OPEN = /^```([A-Za-z0-9_+-]*)[ \t]*$/;
const FENCE_CLOSE = /^```[ \t]*$/;
const HEADING = /^(#{1,6})[ \t]+(.*?)[ \t]*$/;
const HR = /^(?:---|\*\*\*)[ \t]*$/;
const BULLET = /^[-*] (.*)$/;
const ORDERED = /^(\d{1,9})\. (.*)$/;
const CONTINUED = /^ {2,}\S/;
const HAS_WS = /\s/;

function isBlank(line) {
  for (let i = 0; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

const isFenceOpen = (line, c0) => c0 === 96 && FENCE_OPEN.test(line);

function headingOf(line, c0) {
  if (c0 !== 35) return null;
  const m = HEADING.exec(line);
  return m === null || m[2] === '' ? null : m;
}

const isHr = (line, c0) => (c0 === 45 || c0 === 42) && HR.test(line);

/** markdown.js endsParagraph. */
function endsParagraph(line) {
  const c0 = line.charCodeAt(0);
  if (c0 === 32 || c0 === 9 || line.length === 0) {
    if (isBlank(line)) return true;
  }
  return isFenceOpen(line, c0) || headingOf(line, c0) !== null || isHr(line, c0);
}

/** A line with spaces and tabs at either end dropped. */
function trimLine(line) {
  let a = 0;
  let b = line.length;
  while (a < b) {
    const c = line.charCodeAt(a);
    if (c !== 32 && c !== 9) break;
    a++;
  }
  while (b > a) {
    const c = line.charCodeAt(b - 1);
    if (c !== 32 && c !== 9) break;
    b--;
  }
  return a === 0 && b === line.length ? line : line.slice(a, b);
}

/** The char codes a backslash escapes. */
const ESCAPABLE = new Uint8Array(128);
for (const ch of '\\`*_[]()#+-.!>') ESCAPABLE[ch.charCodeAt(0)] = 1;
const escapable = (c) => c < 128 && ESCAPABLE[c] === 1;

/** The first "**" in s[from, to) outside a backslash escape, or -1. */
function findDouble(s, from, to) {
  for (let j = from; j <= to - 2; j++) {
    const c = s.charCodeAt(j);
    if (c === 92) {
      if (j + 1 < to && escapable(s.charCodeAt(j + 1))) j++;
      continue;
    }
    if (c === 42 && s.charCodeAt(j + 1) === 42) return j;
  }
  return -1;
}

/** The first "]" in s[from, to) outside a backslash escape, or -1. */
function findBracket(s, from, to) {
  for (let j = from; j < to; j++) {
    const c = s.charCodeAt(j);
    if (c === 92) {
      if (j + 1 < to && escapable(s.charCodeAt(j + 1))) j++;
      continue;
    }
    if (c === 93) return j;
  }
  return -1;
}

/** The first single * in s[from, to) (one whose run is not **), outside an escape, or -1. */
function findSingleStar(s, from, to) {
  for (let j = from; j < to; j++) {
    const c = s.charCodeAt(j);
    if (c === 92) {
      if (j + 1 < to && escapable(s.charCodeAt(j + 1))) j++;
      continue;
    }
    if (c !== 42) continue;
    if (j + 1 < to && s.charCodeAt(j + 1) === 42) {
      j++;
      continue;
    }
    return j;
  }
  return -1;
}

/** s[from, to) rendered inline (markdown.js inline); `link(target)` is a link's href. */
function inline(s, from, to, link) {
  let out = '';
  let start = from;
  let i = from;
  while (i < to) {
    const c = s.charCodeAt(i);
    if (c > 96) {
      i++;
      continue;
    }
    switch (c) {
      case 92: {
        if (i + 1 < to) {
          const n = s.charCodeAt(i + 1);
          if (escapable(n)) {
            out += s.slice(start, i) + (n === 62 ? '&gt;' : s[i + 1]);
            i += 2;
            start = i;
            continue;
          }
        }
        i++;
        continue;
      }
      case 96: {
        if (start < i) out += s.slice(start, i);
        const j = s.indexOf('`', i + 1);
        if (j > i + 1 && j < to) {
          out += '<code>' + esc(s.slice(i + 1, j)) + '</code>';
          i = j + 1;
        } else {
          out += '`';
          i++;
        }
        start = i;
        continue;
      }
      case 42: {
        if (start < i) out += s.slice(start, i);
        if (i + 1 < to && s.charCodeAt(i + 1) === 42) {
          const j = findDouble(s, i + 2, to);
          if (j > i + 2) {
            out += '<strong>' + inline(s, i + 2, j, link) + '</strong>';
            i = j + 2;
          } else {
            out += '**';
            i += 2;
          }
        } else {
          const j = findSingleStar(s, i + 1, to);
          if (j > i + 1) {
            out += '<em>' + inline(s, i + 1, j, link) + '</em>';
            i = j + 1;
          } else {
            out += '*';
            i++;
          }
        }
        start = i;
        continue;
      }
      case 91: {
        if (start < i) out += s.slice(start, i);
        const k = findBracket(s, i + 1, to);
        let m = -1;
        if (k !== -1 && k + 1 < to && s.charCodeAt(k + 1) === 40) {
          m = s.indexOf(')', k + 2);
          if (m >= to) m = -1;
        }
        const target = m === -1 ? '' : s.slice(k + 2, m);
        if (target !== '' && !HAS_WS.test(target)) {
          out += '<a href="' + esc(link(target)) + '">' + inline(s, i + 1, k, link) + '</a>';
          i = m + 1;
        } else {
          out += '[';
          i++;
        }
        start = i;
        continue;
      }
      case 38:
      case 60:
      case 62:
      case 34:
      case 39:
        out += s.slice(start, i) + ESC[c];
        i++;
        start = i;
        continue;
      default:
        i++;
    }
  }
  if (start < to) out += start === 0 && to === s.length ? s : s.slice(start, to);
  return out;
}

const inlineAll = (s, link) => inline(s, 0, s.length, link);

/** markdown.js renderMarkdown, lexing and rendering in one pass: { html, toc, excerpt }. */
function renderMarkdown(src, link) {
  const ids = new Set();
  const toc = [];
  let excerpt = null;
  let html = '';
  const lines = src.split('\n');
  const n = lines.length;
  let i = 0;
  while (i < n) {
    const line = lines[i];
    const c0 = line.charCodeAt(0);
    if ((c0 === 32 || c0 === 9 || line.length === 0) && isBlank(line)) {
      i++;
      continue;
    }
    if (c0 === 96) {
      const fence = FENCE_OPEN.exec(line);
      if (fence !== null) {
        i++;
        let body = '';
        while (i < n) {
          const l = lines[i];
          if (l.charCodeAt(0) === 96 && FENCE_CLOSE.test(l)) break;
          body += esc(l) + '\n';
          i++;
        }
        i++;
        const lang = fence[1];
        html += (lang === '' ? '<pre><code>' : `<pre><code class="language-${esc(lang)}">`) + body + '</code></pre>\n';
        continue;
      }
    }
    if (c0 === 35) {
      const h = headingOf(line, c0);
      if (h !== null) {
        const text = h[2];
        const depth = h[1].length;
        const id = uniqueSlug(slugify(text), ids);
        const ih = inlineAll(text, link);
        html += `<h${depth} id="${id}">${ih}</h${depth}>\n`;
        if (depth === 2 || depth === 3) toc.push({ level: depth, id, html: ih });
        i++;
        continue;
      }
    }
    if ((c0 === 45 || c0 === 42) && HR.test(line)) {
      html += '<hr>\n';
      i++;
      continue;
    }
    if (c0 === 62) {
      let text = '';
      let any = false;
      while (i < n) {
        const l = lines[i];
        if (l.charCodeAt(0) !== 62) break;
        const t = trimLine(l.slice(l.charCodeAt(1) === 32 ? 2 : 1));
        if (t !== '') {
          text = any ? text + '\n' + t : t;
          any = true;
        }
        i++;
      }
      html += any ? `<blockquote>\n<p>${inlineAll(text, link)}</p>\n</blockquote>\n` : '<blockquote>\n</blockquote>\n';
      continue;
    }
    let isList = false;
    let isOrdered = false;
    let first = null;
    if (c0 === 45 || c0 === 42) {
      first = BULLET.exec(line);
      isList = first !== null;
    }
    if (!isList && c0 >= 48 && c0 <= 57) {
      first = ORDERED.exec(line);
      isList = isOrdered = first !== null;
    }
    if (isList) {
      const marker = isOrdered ? ORDERED : BULLET;
      let items = '';
      let item = null;
      while (i < n) {
        const l = lines[i];
        const lc = l.charCodeAt(0);
        const m = (isOrdered ? lc >= 48 && lc <= 57 : lc === 45 || lc === 42) ? marker.exec(l) : null;
        if (m !== null) {
          if (item !== null) items += '<li>' + inlineAll(item, link) + '</li>\n';
          item = trimLine(isOrdered ? m[2] : m[1]);
          i++;
        } else if (lc === 32 && l.charCodeAt(1) === 32 && CONTINUED.test(l)) {
          item += '\n' + trimLine(l);
          i++;
        } else break;
      }
      items += '<li>' + inlineAll(item, link) + '</li>\n';
      const start = isOrdered ? Number(first[1]) : 1;
      html += (isOrdered ? (start !== 1 ? `<ol start="${start}">\n` : '<ol>\n') : '<ul>\n') + items + (isOrdered ? '</ol>\n' : '</ul>\n');
      continue;
    }
    let text = trimLine(line);
    i++;
    while (i < n && !endsParagraph(lines[i])) text += '\n' + trimLine(lines[i++]);
    const ih = inlineAll(text, link);
    if (excerpt === null) excerpt = ih;
    html += '<p>' + ih + '</p>\n';
  }
  return { html, toc, excerpt: excerpt ?? '' };
}

// ---------------------------------------------------------------- templates (template.js, compiled)

const INCLUDE_DEPTH = 8;
const T_PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const T_FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const T_IF = /^if (not )?(\S+)$/;
const T_INCLUDE = /^include "([a-z0-9-]+)"$/;

class TemplateError extends Error {}

function pathOf(name, text) {
  if (!T_PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) {
      text += src.slice(i);
      break;
    }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    const standalone = BLANK_RE.test(src.slice(lineStart, open)) && BLANK_RE.test(src.slice(end, lineEnd));
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = T_FOR.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = T_IF.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = T_INCLUDE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

// Runtime helpers of compiled templates. A scope is either the root { vars, sh: false } or a for's scope
// { n, v, i, len, p, sh } (sh: whether it or a scope above it binds site, tags or recent).

/** The loop record of a for scope. */
const loopOf = (i, len) => ({ index: i + 1, first: i === 0, last: i === len - 1, length: len });

/** The first name of a path, looked up through the scope chain (template.js lookup). */
function R(sc, name) {
  while (sc.vars === undefined) {
    if (name === 'loop') return loopOf(sc.i, sc.len);
    if (sc.n === name) return sc.v;
    sc = sc.p;
  }
  const vars = sc.vars;
  return hasOwn(vars, name) ? vars[name] : undefined;
}

/** An own property of an object value, else undefined. */
const G = (v, k) => (v !== null && typeof v === 'object' && hasOwn(v, k) ? v[k] : undefined);

function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}

function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

const STATIC_NAMES = new Set(['site', 'tags', 'recent']);

/** The first names `nodes` read from scopes outside them (`free`: each template's set, for its includes). */
function freeNames(nodes, bound, acc, free, index) {
  for (const n of nodes) {
    switch (n.kind) {
      case 'var':
      case 'raw':
        if (!bound.has(n.path[0])) acc.add(n.path[0]);
        break;
      case 'if':
        if (!bound.has(n.path[0])) acc.add(n.path[0]);
        freeNames(n.then, bound, acc, free, index);
        if (n.else !== null) freeNames(n.else, bound, acc, free, index);
        break;
      case 'for': {
        if (!bound.has(n.path[0])) acc.add(n.path[0]);
        const inner = new Set(bound);
        inner.add(n.name);
        inner.add('loop');
        freeNames(n.body, inner, acc, free, index);
        break;
      }
      case 'include': {
        const k = index.get(n.name);
        if (k !== undefined) for (const x of free[k]) if (!bound.has(x)) acc.add(x);
        break;
      }
    }
  }
  return acc;
}

/** Whether `nodes` hold an include, at any depth inside this template. */
function hasInclude(nodes) {
  for (const n of nodes) {
    if (n.kind === 'include') return true;
    if (n.kind === 'for' && hasInclude(n.body)) return true;
    if (n.kind === 'if' && (hasInclude(n.then) || (n.else !== null && hasInclude(n.else)))) return true;
  }
  return false;
}

/** Every parsed template compiled to a function (scope, depth) -> text, by name. */
function compileTemplates(templates) {
  const names = [...templates.keys()];
  const index = new Map(names.map((n, k) => [n, k]));
  const nodesOf = names.map((n) => templates.get(n));
  // Each template's free names, to a fixpoint over its includes.
  const free = names.map(() => new Set());
  for (let changed = true; changed; ) {
    changed = false;
    for (let k = 0; k < names.length; k++) {
      const next = freeNames(nodesOf[k], new Set(), new Set(), free, index);
      if (next.size !== free[k].size) {
        free[k] = next;
        changed = true;
      }
    }
  }
  let chunks = 0;
  let uid = 0;

  /** The JS expression of a path's value; `frames` are this template's enclosing fors, innermost last. */
  function valueExpr(path, frames, scv) {
    const first = path[0];
    let base = null;
    let rest = 1;
    for (let f = frames.length - 1; f >= 0; f--) {
      const fr = frames[f];
      if (first === 'loop') {
        if (path.length === 1) base = `loopOf(${fr.i},${fr.n})`;
        else {
          const k = path[1];
          rest = 2;
          base = k === 'index' ? `(${fr.i}+1)` : k === 'first' ? `(${fr.i}===0)` : k === 'last' ? `(${fr.i}===${fr.n}-1)` : k === 'length' ? fr.n : 'undefined';
        }
        break;
      }
      if (fr.name === first) {
        base = fr.v;
        break;
      }
    }
    if (base === null) base = `R(${scv},${JSON.stringify(first)})`;
    for (let k = rest; k < path.length; k++) base = `G(${base},${JSON.stringify(path[k])})`;
    return base;
  }

  /** Whether a path's value is statically known to be a number (loop.index, loop.length). */
  const numericPath = (path, frames) => frames.length > 0 && path.length === 2 && path[0] === 'loop' && (path[1] === 'index' || path[1] === 'length');

  function genNodes(nodes, frames, scv, o, cache) {
    if (!cache) return genNodes2(nodes, frames, scv, o, false);
    let code = '';
    let k = 0;
    while (k < nodes.length) {
      // A run of nodes reading only site, tags and recent (none bound here): rendered once per build and depth.
      let e = k;
      let work = false;
      const boundHere = new Set();
      for (const fr of frames) {
        boundHere.add(fr.name);
        boundHere.add('loop');
      }
      while (e < nodes.length) {
        const fn = freeNames([nodes[e]], new Set(), new Set(), free, index);
        let ok = true;
        for (const x of fn) if (!STATIC_NAMES.has(x) || boundHere.has(x)) ok = false;
        if (!ok) break;
        if (nodes[e].kind !== 'text') work = true;
        e++;
      }
      if (work) {
        const id = chunks++;
        const run = nodes.slice(k, e);
        const o2 = `c${uid++}`;
        code += `if(!${scv}.sh){const key=${id * 16}+d;let ${o2}=C[key];if(${o2}===undefined){${o2}='';${genNodes2(run, frames, scv, o2, false)}C[key]=${o2};}${o}+=${o2};}else{${genNodes2(run, frames, scv, o, false)}}`;
        k = e;
        continue;
      }
      if (e === k) e = k + 1;
      code += genNodes2(nodes.slice(k, e), frames, scv, o, true);
      k = e;
    }
    return code;
  }

  function genNodes2(nodes, frames, scv, o, cache) {
    let code = '';
    for (const n of nodes) {
      switch (n.kind) {
        case 'text':
          code += `${o}+=${JSON.stringify(n.text)};`;
          break;
        case 'var':
          if (numericPath(n.path, frames)) code += `${o}+=${valueExpr(n.path, frames, scv)};`;
          else code += `${o}+=esc(textOf(${valueExpr(n.path, frames, scv)}));`;
          break;
        case 'raw':
          code += `${o}+=textOf(${valueExpr(n.path, frames, scv)});`;
          break;
        case 'if': {
          code += `if(truthy(${valueExpr(n.path, frames, scv)})${n.not ? '===false' : ''}){${genNodes(n.then, frames, scv, o, cache)}}`;
          if (n.else !== null) code += `else{${genNodes(n.else, frames, scv, o, cache)}}`;
          break;
        }
        case 'for': {
          const u = uid++;
          const fr = { name: n.name, i: `i${u}`, n: `n${u}`, v: `v${u}` };
          const xs = `xs${u}`;
          const needScope = hasInclude(n.body);
          const inner = needScope ? `s${u}` : scv;
          const sh = STATIC_NAMES.has(n.name) ? 'true' : `${scv}.sh`;
          code += `{const ${xs}=${valueExpr(n.path, frames, scv)};if(Array.isArray(${xs})){const ${fr.n}=${xs}.length;for(let ${fr.i}=0;${fr.i}<${fr.n};${fr.i}++){const ${fr.v}=${xs}[${fr.i}];`;
          if (needScope) code += `const ${inner}={vars:undefined,n:${JSON.stringify(n.name)},v:${fr.v},i:${fr.i},len:${fr.n},p:${scv},sh:${sh}};`;
          code += genNodes(n.body, [...frames, fr], inner, o, cache);
          code += '}}}';
          break;
        }
        case 'include': {
          const k = index.get(n.name);
          if (k !== undefined) code += `if(d<${INCLUDE_DEPTH})${o}+=F[${k}](${scv},d+1);`;
          break;
        }
      }
    }
    return code;
  }

  const bodies = nodesOf.map((nodes) => `function(sc,d){let o='';${genNodes(nodes, [], 'sc', 'o', true)}return o;}`);
  const factory = new Function('R', 'G', 'esc', 'textOf', 'truthy', 'loopOf', 'F', 'C', `return [${bodies.join(',\n')}];`);
  const F = [];
  const C = [];
  const fns = factory(R, G, esc, textOf, truthy, loopOf, F, C);
  for (const f of fns) F.push(f);
  return new Map(names.map((n, k) => [n, F[k]]));
}

// ---------------------------------------------------------------- the build (index.js)

const REPORT_FILE = 'build-report.txt';
const RESERVED = ['posts', 'tags', 'page'];
const SCHEME = /^[A-Za-z][A-Za-z0-9+.-]*:/;

function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(seg);
  }
  return parts.join('/');
}

const dirOf = (path) => {
  const k = path.lastIndexOf('/');
  return k === -1 ? '' : path.slice(0, k);
};

export function run(input) {
  // Templates, in code-unit order of their names; one that does not parse is reported.
  const parsed = new Map();
  const reportLines = [];
  for (const name of Object.keys(input.templates).sort(byCodeUnits)) {
    try {
      parsed.set(name, parseTemplate(name, input.templates[name]));
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      reportLines.push(err.message);
    }
  }
  const base = input.site.base_url.replace(/\/+$/, '');

  // 1. Sources and statics.
  const files = [...input.files].sort((x, y) => byCodeUnits(x.path, y.path));
  let sources = [];
  const statics = [];
  for (const f of files) {
    if (f.path.endsWith('.md')) {
      const c = f.content;
      sources.push({ path: f.path, text: c.indexOf('\r') === -1 ? c : c.replace(/\r\n?/g, '\n') });
    } else statics.push(f);
  }

  // 2. Front matter.
  const kept = [];
  for (const s of sources) {
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) {
      reportLines.push(`refused ${s.path}: ${fm.error}`);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    const own = s.path.startsWith('tags/') ? 'tags' : s.path.startsWith('page/') ? 'page' : undefined;
    if (own !== undefined) {
      reportLines.push(`refused ${s.path}: ${own}/ is the generator's own`);
      continue;
    }
    if (s.post && !hasOwn(s.data, 'date')) {
      reportLines.push(`refused ${s.path}: a post needs a date`);
      continue;
    }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!parsed.has(layout)) {
      reportLines.push(`refused ${s.path}: layout "${layout}" is no template that parses`);
      continue;
    }
    s.layout = layout;
    if (s.data.draft === true) {
      reportLines.push(`draft ${s.path}`);
      continue;
    }
    kept.push(s);
  }
  sources = kept;

  // 3. Permalinks.
  const taken = new Map();
  const bySource = new Map();
  for (const s of sources) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (set === undefined) taken.set(dir, (set = new Set(dir === '' ? RESERVED : [])));
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), set);
    s.slug = slug;
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    s.url = `${base}/${s.outDir}/`;
    bySource.set(s.path, s);
  }

  // 4. Markdown.
  const staticPaths = new Set(statics.map((f) => f.path));
  const broken = new Set();
  for (const s of sources) {
    const dir = dirOf(s.path);
    const link = (target) => {
      const c0 = target.charCodeAt(0);
      if (c0 === 47 || c0 === 35 || SCHEME.test(target)) return target;
      const hash = target.indexOf('#');
      const rel = hash === -1 ? target : target.slice(0, hash);
      const frag = hash === -1 ? '' : target.slice(hash);
      const path = rel === '' ? null : resolvePath(dir, rel);
      if (path !== null) {
        const t = bySource.get(path);
        if (t !== undefined) return t.url + frag;
        if (staticPaths.has(path)) return `${base}/${path}${frag}`;
      }
      broken.add(`broken-link ${s.path} -> ${target}`);
      return target;
    };
    const md = renderMarkdown(s.body, link);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  // 5. Collections.
  const posts = sources.filter((s) => s.post);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  for (let k = 0; k < posts.length; k++) {
    posts[k].newer = k === 0 ? null : posts[k - 1];
    posts[k].older = k === posts.length - 1 ? null : posts[k + 1];
  }

  // 6. Tags.
  const bySlug = new Map();
  const ownTags = (s) => {
    const seen = new Set();
    const out = [];
    for (const name of s.data.tags ?? []) {
      const slug = slugify(name);
      if (seen.has(slug)) continue;
      seen.add(slug);
      out.push({ name, slug });
    }
    return out;
  };
  for (const p of posts) {
    p.tagRefs = [];
    for (const { name, slug } of ownTags(p)) {
      let tag = bySlug.get(slug);
      if (tag === undefined) bySlug.set(slug, (tag = { name, slug, url: `${base}/tags/${slug}/`, posts: [] }));
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (const s of sources) if (!s.post) s.tagRefs = ownTags(s).map(({ name, slug }) => bySlug.get(slug) ?? { name, slug, url: '', posts: [] });
  const tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));

  // 7. Layouts.
  const dates = new Map();
  const dateLong = (d) => {
    let v = dates.get(d);
    if (v === undefined) dates.set(d, (v = longDate(d)));
    return v;
  };
  const linkOf = (s) => (s === null ? null : { title: s.data.title, url: s.url, date: s.data.date ?? '', date_long: s.data.date === undefined ? '' : dateLong(s.data.date) });
  const cardOf = (s) => ({
    title: s.data.title,
    url: s.url,
    date: s.data.date ?? '',
    date_long: s.data.date === undefined ? '' : dateLong(s.data.date),
    summary: s.data.summary ?? '',
    excerpt: s.excerpt,
    reading_minutes: Math.max(1, Math.ceil(wordsOf(s.body) / 200)),
    tags: s.tagRefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
  });
  const cards = new Map(posts.map((p) => [p, cardOf(p)]));
  const pageOf = (s) => {
    const card = cards.get(s) ?? cardOf(s);
    return {
      title: card.title,
      url: card.url,
      date: card.date,
      date_long: card.date_long,
      summary: card.summary,
      excerpt: card.excerpt,
      reading_minutes: card.reading_minutes,
      tags: card.tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
      slug: s.slug,
      content: s.html,
      toc: s.toc.map((h) => ({ level: h.level, id: h.id, html: h.html })),
      newer: linkOf(s.newer ?? null),
      older: linkOf(s.older ?? null),
      is_post: s.post,
    };
  };

  const compiled = compileTemplates(parsed);
  const site = { title: input.site.title, base_url: base };
  const allTags = tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recent = posts.slice(0, input.site.recent).map((p) => cards.get(p));
  const render = (name, vars) => {
    const f = compiled.get(name);
    return f === undefined ? null : f({ vars, sh: false }, 0);
  };
  const output = {};
  for (const s of sources) output[`${s.outDir}/index.html`] = render(s.layout, { site, tags: allTags, recent, page: pageOf(s) });
  const per = input.site.per_page;
  const pages = Math.max(1, Math.ceil(posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${base}/` : `${base}/page/${k}/`);
  for (let k = 1; k <= pages; k++) {
    const html = render('index', {
      site,
      tags: allTags,
      recent,
      posts: posts.slice((k - 1) * per, k * per).map((p) => cards.get(p)),
      pager: { number: k, total: pages, url: indexUrl(k), newer_url: k === 1 ? '' : indexUrl(k - 1), older_url: k === pages ? '' : indexUrl(k + 1) },
    });
    if (html !== null) output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
  }
  for (const t of tags) {
    const html = render('tag', { site, tags: allTags, recent, tag: { name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: t.posts.map((p) => cards.get(p)) } });
    if (html !== null) output[`tags/${t.slug}/index.html`] = html;
  }
  const tagsHtml = render('tags', { site, tags: allTags, recent });
  if (tagsHtml !== null) output['tags/index.html'] = tagsHtml;
  for (const f of statics) {
    if (hasOwn(output, f.path)) reportLines.push(`shadowed ${f.path}: a built page has its path`);
    else output[f.path] = f.content;
  }

  // 8. Report.
  const lines = [...reportLines, ...broken].sort(byCodeUnits);
  lines.push(`built ${sources.length - posts.length} pages, ${posts.length} posts, ${tags.length} tags and ${statics.length} static files`);
  output[REPORT_FILE] = `${lines.join('\n')}\n`;
  return output;
}
