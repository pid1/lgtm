import { DurableObject } from "cloudflare:workers";

// Profiles added from the device, and the pairing codes that hand a new
// machine its hook token. Like keys.json, only token hashes are stored.
//
// Pairing is the one thing an unauthenticated caller can attempt, so it is
// kept narrow: a code is 8 characters of Crockford base32 (40 bits), lives
// for 10 minutes, works once, and too many wrong guesses cancel every open
// code. With at most MAX_FAILURES guesses against 2^40 codes, the chance of
// guessing one is about 1 in 100 billion.

const PAIR_TTL_MS = 10 * 60_000;
const MAX_FAILURES = 10;
const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

interface Pairing {
  profile: string;
  codeHash: string;
  expires: number;
}

export type PairStatus = "pending" | "paired" | "expired";

export class Registry extends DurableObject {
  // profile -> sha256(hook token)
  async hooks(): Promise<Record<string, string>> {
    const all = await this.ctx.storage.list<string>({ prefix: "hook:" });
    return Object.fromEntries([...all].map(([k, v]) => [k.slice(5), v]));
  }

  // Starts pairing for a profile, new or existing. Redeeming the code for an
  // existing profile rotates its token, so this is also how you re-pair.
  async startPairing(profile: string): Promise<{ code: string; expires: number }> {
    const bytes = crypto.getRandomValues(new Uint8Array(8));
    const raw = [...bytes].map((b) => ALPHABET[b & 31]).join("");
    const expires = Date.now() + PAIR_TTL_MS;
    await this.ctx.storage.put(`pair:${profile}`, { profile, codeHash: await sha256(raw), expires } satisfies Pairing);
    // A fresh code resets the guess budget.
    await this.ctx.storage.put("pair:failures", 0);
    return { code: `${raw.slice(0, 4)}-${raw.slice(4)}`, expires };
  }

  async redeem(code: string): Promise<{ profile: string; token: string } | null> {
    const h = await sha256(normalize(code));
    const open = await this.ctx.storage.list<Pairing>({ prefix: "pair:" });
    let match: Pairing | null = null;
    for (const [key, p] of open) {
      if (key === "pair:failures") continue;
      if (p.expires > Date.now() && safeEqual(p.codeHash, h)) match = p;
    }
    if (!match) {
      const failures = ((await this.ctx.storage.get<number>("pair:failures")) ?? 0) + 1;
      await this.ctx.storage.put("pair:failures", failures);
      if (failures >= MAX_FAILURES) await this.cancelAll();
      return null;
    }
    const token = [...crypto.getRandomValues(new Uint8Array(32))].map((b) => b.toString(16).padStart(2, "0")).join("");
    await this.ctx.storage.put(`hook:${match.profile}`, await sha256(token));
    await this.ctx.storage.delete(`pair:${match.profile}`);
    await this.ctx.storage.put(`paired:${match.codeHash}`, Date.now());
    return { profile: match.profile, token };
  }

  // The device polls this while it shows the code.
  async pairStatus(profile: string, code: string): Promise<PairStatus> {
    const h = await sha256(normalize(code));
    if (await this.ctx.storage.get(`paired:${h}`)) return "paired";
    const p = await this.ctx.storage.get<Pairing>(`pair:${profile}`);
    return p && p.codeHash === h && p.expires > Date.now() ? "pending" : "expired";
  }

  async remove(profile: string): Promise<void> {
    await this.ctx.storage.delete([`hook:${profile}`, `pair:${profile}`]);
  }

  private async cancelAll(): Promise<void> {
    const open = await this.ctx.storage.list({ prefix: "pair:" });
    await this.ctx.storage.delete([...open.keys()]);
  }
}

// Accepts what people actually type: any case, with or without the dash, and
// the usual look-alikes for the letters Crockford leaves out.
function normalize(code: string): string {
  return code.toUpperCase().replace(/[\s-]/g, "").replace(/O/g, "0").replace(/[IL]/g, "1");
}

async function sha256(s: string): Promise<string> {
  const d = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(d)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function safeEqual(a: string, b: string): boolean {
  const x = new TextEncoder().encode(a), y = new TextEncoder().encode(b);
  return x.length === y.length && crypto.subtle.timingSafeEqual(x, y);
}
