#!/usr/bin/env node
// Manages keys.json. The raw token is printed once and never written to disk
// here; only its SHA-256 goes into keys.json, which is safe to commit.
//
//   node scripts/key.mjs hook <profile>     new hook token for a profile (rotates if it exists)
//   node scripts/key.mjs device <name>      new device token
//   node scripts/key.mjs revoke hook|device <name>
//   node scripts/key.mjs list
//
// Changes take effect on the next `npm run deploy`.

import { createHash, randomBytes } from "node:crypto";
import { readFileSync, writeFileSync } from "node:fs";

const FILE = new URL("../keys.json", import.meta.url);
const keys = JSON.parse(readFileSync(FILE, "utf8"));
const [cmd, a, b] = process.argv.slice(2);
const table = (kind) => ({ hook: keys.hooks, device: keys.devices })[kind];
const valid = (name) => /^[a-z0-9-]{1,32}$/.test(name ?? "");

if ((cmd === "hook" || cmd === "device") && valid(a)) {
  const token = randomBytes(32).toString("hex");
  table(cmd)[a] = createHash("sha256").update(token).digest("hex");
  save();
  console.log(`${cmd} "${a}" token (shown once, store it now):\n\n  ${token}\n`);
  console.log("Deploy to apply: npm run deploy");
} else if (cmd === "revoke" && table(a) && valid(b)) {
  if (!(b in table(a))) fail(`no ${a} named "${b}"`);
  delete table(a)[b];
  save();
  console.log(`revoked ${a} "${b}". Deploy to apply: npm run deploy`);
} else if (cmd === "list") {
  console.log("hooks (profiles):", Object.keys(keys.hooks).join(", ") || "none");
  console.log("devices:", Object.keys(keys.devices).join(", ") || "none");
} else {
  fail("usage: key.mjs hook <profile> | device <name> | revoke hook|device <name> | list  (names: a-z 0-9 -)");
}

function save() {
  writeFileSync(FILE, JSON.stringify(keys, null, 2) + "\n");
}

function fail(msg) {
  console.error(msg);
  process.exit(1);
}
