#!/usr/bin/env node
// Claude Code PermissionRequest hook. Sends the pending tool call to the lgtm
// Worker and waits for the handheld to answer.
//
// Every failure path exits 0 with no output, which hands the decision back to
// the normal terminal prompt. The hook never allows anything on its own.
//
// Config comes from the environment, or from ~/.config/lgtm/config as
// KEY=VALUE lines: LGTM_URL, LGTM_TOKEN, and optionally LGTM_WAIT (seconds,
// default 120). Keep LGTM_WAIT under the hook's `timeout` in settings.json.
//
// `lgtm-hook.mjs pair <code> [url]` trades a pairing code from the device for
// this machine's hook token and writes that config file.

import { closeSync, fstatSync, mkdirSync, openSync, readFileSync, readSync, writeFileSync } from "node:fs";
import { homedir, hostname } from "node:os";
import { fileURLToPath } from "node:url";

const POLL_MS = 1000;
const CONFIG = `${homedir()}/.config/lgtm/config`;

if (process.argv[2] === "pair") await pair(process.argv[3], process.argv[4]);

const cfg = { ...readConfigFile(), ...pick(process.env, ["LGTM_URL", "LGTM_TOKEN", "LGTM_WAIT"]) };
if (!cfg.LGTM_URL || !cfg.LGTM_TOKEN || process.env.LGTM_DISABLE) process.exit(0);

const base = cfg.LGTM_URL.replace(/\/+$/, "");
const headers = { authorization: `Bearer ${cfg.LGTM_TOKEN}`, "content-type": "application/json" };
const deadline = Date.now() + Number(cfg.LGTM_WAIT || 120) * 1000;

let id;
try {
  const input = JSON.parse(readFileSync(0, "utf8"));
  const host = hostname().replace(/\.local$/, "");
  const body = JSON.stringify({ ...input, host, intent: intent(input.transcript_path) });
  const res = await fetch(`${base}/v1/requests`, { method: "POST", headers, body, signal: AbortSignal.timeout(5000) });
  // 409 means no device has polled recently: go straight to the terminal.
  if (res.status !== 201) process.exit(0);
  ({ id } = await res.json());

  while (Date.now() < deadline) {
    await new Promise((r) => setTimeout(r, POLL_MS));
    const r = await fetch(`${base}/v1/requests/${id}`, { headers, signal: AbortSignal.timeout(5000) }).catch(() => null);
    if (!r) continue; // transient network error; keep waiting
    if (r.status === 404) process.exit(0);
    const body = await r.json();
    if (body.status !== "decided") continue;
    if (body.behavior === "allow") emit({ behavior: "allow" });
    if (body.behavior === "deny") emit({ behavior: "deny", message: body.message || "Denied from lgtm." });
    process.exit(0); // "defer": hand back to the terminal
  }
} catch {
  // fall through to cleanup
}
await cleanup();
process.exit(0);

function emit(decision) {
  process.stdout.write(JSON.stringify({ hookSpecificOutput: { hookEventName: "PermissionRequest", decision } }));
  process.exit(0);
}

async function cleanup() {
  // Pull the request off the device so it doesn't answer a prompt that's gone.
  if (!id) return;
  await fetch(`${base}/v1/requests/${id}`, { method: "DELETE", headers, signal: AbortSignal.timeout(3000) }).catch(() => {});
}

// What Claude last said before this tool call ("Now I'll update the README…"),
// so the device can show why, not just what. Only text since the last human
// message counts; anything older would describe a different step.
function intent(path) {
  if (!path) return "";
  try {
    const fd = openSync(path, "r");
    const size = fstatSync(fd).size;
    const len = Math.min(size, 256 * 1024);
    const buf = Buffer.alloc(len);
    readSync(fd, buf, 0, len, size - len);
    closeSync(fd);
    const lines = buf.toString("utf8").split("\n").reverse();
    for (const line of lines) {
      let e;
      try {
        e = JSON.parse(line);
      } catch {
        continue; // blank, or cut in half by the tail read
      }
      const content = e.message?.content;
      if (e.type === "user") {
        const human = typeof content === "string" || (Array.isArray(content) && content.some((c) => c.type === "text"));
        if (human) return "";
      }
      if (e.type === "assistant" && Array.isArray(content)) {
        const text = content.filter((c) => c.type === "text").map((c) => c.text).join(" ").trim();
        if (text) return text.slice(-600);
      }
    }
  } catch {
    // No transcript is fine; the card just has no "why".
  }
  return "";
}

async function pair(code, url) {
  const base = (url || readConfigFile().LGTM_URL || "").replace(/\/+$/, "");
  if (!code || !/^https?:\/\//.test(base)) {
    console.error("usage: lgtm-hook.mjs pair <code> <https://lgtm.example.workers.dev>");
    process.exit(2);
  }
  const res = await fetch(`${base}/v1/pair`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ code }),
  }).catch((e) => (console.error(`can't reach ${base}: ${e.message}`), process.exit(1)));
  if (!res.ok) {
    console.error("That code didn't work. Codes last 10 minutes and work once; start a new one on the device.");
    process.exit(1);
  }
  const { profile, token } = await res.json();
  mkdirSync(`${homedir()}/.config/lgtm`, { recursive: true, mode: 0o700 });
  writeFileSync(CONFIG, `LGTM_URL=${base}\nLGTM_TOKEN=${token}\n`, { mode: 0o600 });
  const self = fileURLToPath(import.meta.url);
  console.log(`Paired as "${profile}". Wrote ${CONFIG}.

If this machine doesn't have the hook yet, add to ~/.claude/settings.json:

  "hooks": {
    "PermissionRequest": [
      { "matcher": "*", "hooks": [{ "type": "command", "command": "${self}", "timeout": 150 }] }
    ]
  }`);
  process.exit(0);
}

function readConfigFile() {
  try {
    const out = {};
    for (const line of readFileSync(CONFIG, "utf8").split("\n")) {
      const m = line.match(/^\s*([A-Z_]+)\s*=\s*(.*?)\s*$/);
      if (m) out[m[1]] = m[2];
    }
    return out;
  } catch {
    return {};
  }
}

function pick(obj, keys) {
  return Object.fromEntries(keys.filter((k) => obj[k]).map((k) => [k, obj[k]]));
}
