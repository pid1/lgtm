// Turns a tool call into a decision card for a 3.5" screen: a headline, a few
// facts worth knowing before saying yes (including anything risky), and the
// full detail for when you want to read it. The device pages through the
// detail in chunks, so it isn't capped here beyond a sanity limit.

export interface Card {
  headline: string;
  facts: string[];
  body: string[];
}

const MAX_BODY_LINES = 20_000;
const CONTEXT = 3;

export function summarize(tool: string, input: Record<string, unknown>, cwd: string): Card {
  const s = (k: string) => (typeof input[k] === "string" ? (input[k] as string) : "");
  const facts: string[] = [];
  let headline: string;
  let body: string[];

  switch (tool) {
    case "Bash": {
      const cmd = s("command");
      headline = s("description") || `Run ${firstLine(cmd)}`;
      facts.push(...bashFlags(cmd));
      body = prefix("$ ", cmd);
      break;
    }
    case "Edit": {
      const path = s("file_path");
      const d = diff(s("old_string").split("\n"), s("new_string").split("\n"));
      headline = `Edit ${basename(path)}`;
      facts.push(`+${d.added} -${d.removed} lines · ${relative(path, cwd)}`);
      if (input.replace_all === true) facts.push("replaces every occurrence");
      facts.push(...pathFlags(path, cwd));
      body = d.lines;
      break;
    }
    case "Write": {
      const path = s("file_path");
      const lines = s("content").split("\n");
      headline = `Write ${basename(path)}`;
      facts.push(`${lines.length} lines · ${relative(path, cwd)}`, "creates or replaces the whole file");
      facts.push(...pathFlags(path, cwd));
      body = prefix("+ ", s("content"));
      break;
    }
    case "WebFetch": {
      const url = s("url");
      headline = `Fetch ${host(url)}`;
      facts.push(url);
      body = [url, "", ...s("prompt").split("\n")];
      break;
    }
    case "WebSearch":
      headline = `Search the web`;
      facts.push(s("query"));
      body = [s("query")];
      break;
    default:
      headline = tool.startsWith("mcp__") ? `Use ${tool.split("__").slice(1).join(" · ")}` : `Use ${tool}`;
      body = JSON.stringify(input, null, 1).split("\n");
  }

  // The device protocol is line-based; stray \r or tabs would show as garbage.
  body = body.map(clean);
  if (body.length > MAX_BODY_LINES) {
    body = [...body.slice(0, MAX_BODY_LINES), `... ${body.length - MAX_BODY_LINES} more lines not sent`];
  }
  return { headline: oneLine(headline, 100), facts: facts.map((f) => oneLine(f, 200)), body };
}

// Things worth a second look before approving a shell command.
function bashFlags(cmd: string): string[] {
  const flags: string[] = [];
  if (/\brm\s+(-\w*[rRf]|--recursive|--force)/.test(cmd)) flags.push("! deletes files");
  if (/\bsudo\b/.test(cmd)) flags.push("! runs as root");
  if (/\bgit\s+(push|reset\s+--hard|clean\s+-\w*f)/.test(cmd)) flags.push("! changes git history or remotes");
  if (/\b(wrangler\s+deploy|npm\s+publish|terraform\s+apply|kubectl\s+(apply|delete))\b/.test(cmd))
    flags.push("! deploys or publishes");
  if (/\b(curl|wget|ssh|scp|rsync)\b/.test(cmd)) flags.push("uses the network");
  const lines = cmd.split("\n").length;
  if (lines > 1) flags.push(`${lines} lines`);
  return flags;
}

// Facts starting with "! " are warnings; the device shows them in red.
function pathFlags(path: string, cwd: string): string[] {
  if (cwd && path.startsWith("/") && !path.startsWith(cwd.replace(/\/?$/, "/"))) return ["! outside this project"];
  return [];
}

// Line diff with a few lines of context around each change; long unchanged
// runs collapse to a marker. Falls back to all-removed/all-added when the
// inputs are too big for the quadratic table.
function diff(a: string[], b: string[]): { lines: string[]; added: number; removed: number } {
  let pre = 0;
  while (pre < a.length && pre < b.length && a[pre] === b[pre]) pre++;
  let suf = 0;
  while (suf < a.length - pre && suf < b.length - pre && a[a.length - 1 - suf] === b[b.length - 1 - suf]) suf++;
  const am = a.slice(pre, a.length - suf);
  const bm = b.slice(pre, b.length - suf);

  const ops: [" " | "-" | "+", string][] = a.slice(0, pre).map((l) => [" ", l] as [" ", string]);
  if (am.length * bm.length <= 4_000_000) {
    const n = am.length, m = bm.length;
    const t = Array.from({ length: n + 1 }, () => new Uint32Array(m + 1));
    for (let i = n - 1; i >= 0; i--)
      for (let j = m - 1; j >= 0; j--) t[i][j] = am[i] === bm[j] ? t[i + 1][j + 1] + 1 : Math.max(t[i + 1][j], t[i][j + 1]);
    let i = 0, j = 0;
    while (i < n || j < m) {
      if (i < n && j < m && am[i] === bm[j]) (ops.push([" ", am[i]]), i++, j++);
      else if (j < m && (i === n || t[i][j + 1] >= t[i + 1][j])) (ops.push(["+", bm[j]]), j++);
      else (ops.push(["-", am[i]]), i++);
    }
  } else {
    ops.push(...am.map((l) => ["-", l] as ["-", string]), ...bm.map((l) => ["+", l] as ["+", string]));
  }
  ops.push(...a.slice(a.length - suf).map((l) => [" ", l] as [" ", string]));

  const near = ops.map(() => false);
  ops.forEach(([op], k) => {
    if (op === " ") return;
    for (let x = Math.max(0, k - CONTEXT); x <= Math.min(ops.length - 1, k + CONTEXT); x++) near[x] = true;
  });
  const lines: string[] = [];
  let skipped = 0;
  ops.forEach(([op, l], k) => {
    if (!near[k]) return void skipped++;
    if (skipped) (lines.push(`  ... ${skipped} unchanged lines`), (skipped = 0));
    lines.push(`${op} ${l}`);
  });
  if (skipped) lines.push(`  ... ${skipped} unchanged lines`);
  return {
    lines,
    added: ops.filter(([op]) => op === "+").length,
    removed: ops.filter(([op]) => op === "-").length,
  };
}

function prefix(p: string, body: string): string[] {
  return body.split("\n").map((l) => p + l);
}

function clean(l: string): string {
  return l.replace(/\r/g, "").replace(/\t/g, "  ");
}

export function oneLine(s: string, max: number): string {
  const flat = clean(s).replace(/\s*\n\s*/g, " ").trim();
  return flat.length > max ? flat.slice(0, max - 1) + "..." : flat;
}

function firstLine(s: string): string {
  const l = s.split("\n")[0];
  return s.includes("\n") ? `${l} ...` : l;
}

export function basename(p: string): string {
  return p.split("/").filter(Boolean).pop() ?? "";
}

function relative(path: string, cwd: string): string {
  const base = cwd.replace(/\/?$/, "/");
  return cwd && path.startsWith(base) ? path.slice(base.length) : path;
}

function host(url: string): string {
  try {
    return new URL(url).host;
  } catch {
    return url;
  }
}
