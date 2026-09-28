import { DurableObject } from "cloudflare:workers";
import { summarize, oneLine } from "./summarize.js";
import keys from "../keys.json";
import { Registry } from "./registry.js";

export { Registry };

export interface Env {
  INBOX: DurableObjectNamespace<Inbox>;
  REGISTRY: DurableObjectNamespace<Registry>;
}

// Tokens are never stored here, only their SHA-256. Hook tokens map to the one
// profile they may submit to; device tokens may read and decide for every
// profile. `node scripts/key.mjs` manages keys.json; profiles paired from the
// device live in the Registry and take precedence over keys.json.
interface Keys {
  hooks: Record<string, string>; // profile -> sha256(token)
  devices: Record<string, string>; // device name -> sha256(token)
}

// A profile whose inbox no device has polled within this window is treated as
// unwatched, and its hook falls straight through to the terminal prompt.
// That's what makes switching profiles on the device instant: the profiles
// it stops polling go stale and their prompts stay local.
const DEVICE_FRESH_MS = 15_000;

// Requests nobody answered are dropped after this. The hook gives up well
// before it, so this only cleans up after hooks that died mid-wait.
const REQUEST_TTL_MS = 15 * 60_000;

// The detail body is stored and sent in chunks of this many lines, so a huge
// diff never has to fit in one storage value or one response.
const CHUNK = 200;

type Behavior = "allow" | "deny" | "defer";

interface Pending {
  id: string;
  created: number;
  tool: string;
  project: string;
  host: string; // machine the hook ran on
  headline: string;
  intent: string; // what Claude said it was doing, from the transcript
  facts: string[];
  lines: number; // total detail lines, stored in `body:<id>:<chunk>`
  decision?: { behavior: Behavior; message?: string };
}

type Caller = { kind: "hook"; profile: string } | { kind: "device"; name: string };

export default {
  async fetch(req: Request, env: Env): Promise<Response> {
    const url = new URL(req.url);
    const path = url.pathname;
    const pairing = req.method === "POST" && path === "/v1/pair";
    // Turn away anonymous traffic before it costs a Durable Object call.
    if (!pairing && !/^Bearer \S+$/.test(req.headers.get("authorization") ?? "")) return text("unauthorized\n", 401);
    const registry = env.REGISTRY.get(env.REGISTRY.idFromName("registry"));
    const hooks = { ...(keys as Keys).hooks, ...(await registry.hooks()) };

    // The only route open without a token: trading a pairing code shown on
    // the device for a hook token. See registry.ts for the guessing limits.
    if (pairing) {
      const body = (await req.json().catch(() => null)) as { code?: unknown } | null;
      const r = typeof body?.code === "string" ? await registry.redeem(body.code) : null;
      return r ? Response.json(r) : Response.json({ error: "invalid or expired code" }, { status: 403 });
    }

    // Everything else is authenticated before it runs. Unauthenticated callers
    // learn nothing, not even which paths exist.
    const caller = await identify(req, hooks, (keys as Keys).devices);
    if (!caller) return text("unauthorized\n", 401);
    const inbox = (profile: string) => env.INBOX.get(env.INBOX.idFromName(profile));

    if (caller.kind === "hook") {
      const box = inbox(caller.profile);
      if (req.method === "POST" && path === "/v1/requests") {
        const input = (await req.json().catch(() => null)) as HookInput | null;
        if (!input) return Response.json({ error: "bad json" }, { status: 400 });
        const id = await box.create(input);
        if (!id) return Response.json({ error: "no device" }, { status: 409 });
        return Response.json({ id }, { status: 201 });
      }
      const one = path.match(/^\/v1\/requests\/([0-9a-f-]{36})$/);
      if (one && req.method === "GET") {
        const r = await box.status(one[1]);
        return Response.json(r, { status: r.status === "gone" ? 404 : 200 });
      }
      if (one && req.method === "DELETE") {
        await box.cancel(one[1]);
        return new Response(null, { status: 204 });
      }
      return text("not found\n", 404);
    }

    // --- device side (plain text, so the C client needs no JSON parser) ---

    const profiles = Object.keys(hooks).sort();

    if (req.method === "GET" && path === "/v1/device/profiles") {
      return text(profiles.join("\n") + "\n");
    }

    // Add or re-pair a profile: returns a pairing code for the new machine.
    if (req.method === "POST" && path === "/v1/device/pair") {
      const name = String((await req.formData()).get("profile") ?? "");
      if (!/^[a-z0-9-]{1,32}$/.test(name) || name === "all") return text("bad name\n", 400);
      const { code, expires } = await registry.startPairing(name);
      return text(`${code}\n${Math.round((expires - Date.now()) / 1000)}\n`);
    }

    if (req.method === "GET" && path === "/v1/device/pair") {
      const status = await registry.pairStatus(url.searchParams.get("profile") ?? "", url.searchParams.get("code") ?? "");
      return text(`${status}\n`);
    }

    if (req.method === "POST" && path === "/v1/device/remove") {
      const name = String((await req.formData()).get("profile") ?? "");
      if (name in (keys as Keys).hooks) return text("defined in keys.json; remove it there\n", 409);
      await registry.remove(name);
      // Its requests go too. Their hooks can't clean up after themselves any
      // more (the token is gone), and a profile re-paired under the same name
      // shouldn't inherit them.
      await inbox(name).purge();
      return text("ok\n");
    }

    if (req.method === "GET" && path === "/v1/device/next") {
      const want = url.searchParams.get("profile") || "all";
      const watched = want === "all" ? profiles : profiles.filter((p) => p === want);
      if (!watched.length) return text("unknown profile\n", 404);
      const heads = await Promise.all(watched.map(async (p) => ({ p, ...(await inbox(p).next()) })));
      const count = heads.reduce((n, h) => n + h.count, 0);
      const oldest = heads.filter((h) => h.head).sort((a, b) => a.head!.created - b.head!.created)[0];
      if (!oldest) return text("0\n");
      const h = oldest.head!;
      const first = await inbox(oldest.p).chunk(h.id, 0);
      // One field per line: depth, id, profile, tool, project, host,
      // headline, intent, total detail lines, fact count, the facts, then the
      // first chunk of detail.
      return text(
        [count, h.id, oldest.p, h.tool, h.project, h.host, h.headline, h.intent, h.lines, h.facts.length, ...h.facts, ...first]
          .join("\n") + "\n",
      );
    }

    if (req.method === "GET" && path === "/v1/device/body") {
      const profile = url.searchParams.get("profile") ?? "";
      const n = Number(url.searchParams.get("chunk"));
      if (!profiles.includes(profile) || !Number.isInteger(n) || n < 0) return text("bad request\n", 400);
      const lines = await inbox(profile).chunk(url.searchParams.get("id") ?? "", n);
      return text(lines.join("\n") + "\n");
    }

    if (req.method === "POST" && path === "/v1/device/decide") {
      const form = await req.formData();
      const profile = String(form.get("profile") ?? "");
      const id = String(form.get("id") ?? "");
      const behavior = String(form.get("behavior") ?? "") as Behavior;
      if (!profiles.includes(profile)) return text("unknown profile\n", 404);
      if (!["allow", "deny", "defer"].includes(behavior)) return text("bad behavior\n", 400);
      const message = behavior === "deny" ? String(form.get("message") || "Denied from lgtm.") : undefined;
      const ok = await inbox(profile).decide(id, { behavior, message });
      return ok ? text("ok\n") : text("gone\n", 404);
    }

    return text("not found\n", 404);
  },
} satisfies ExportedHandler<Env>;

async function identify(
  req: Request,
  hooks: Record<string, string>,
  devices: Record<string, string>,
): Promise<Caller | null> {
  const m = (req.headers.get("authorization") ?? "").match(/^Bearer (\S+)$/);
  if (!m) return null;
  const got = new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(m[1])));
  // Check every key rather than returning on the first match, so timing
  // doesn't say which table or entry was close.
  let found: Caller | null = null;
  for (const [profile, hex] of Object.entries(hooks)) {
    if (same(got, hex)) found = { kind: "hook", profile };
  }
  for (const [name, hex] of Object.entries(devices)) {
    if (same(got, hex)) found = { kind: "device", name };
  }
  return found;
}

function same(got: Uint8Array, hex: string): boolean {
  const want = new Uint8Array((hex.match(/../g) ?? []).map((b) => parseInt(b, 16)));
  return want.length === got.length && crypto.subtle.timingSafeEqual(got, want);
}

// One inbox per profile.
export class Inbox extends DurableObject<Env> {
  async create(input: HookInput): Promise<string | null> {
    await this.prune();
    const lastSeen = (await this.ctx.storage.get<number>("device:lastSeen")) ?? 0;
    if (Date.now() - lastSeen > DEVICE_FRESH_MS) return null;
    const card = summarize(input.tool_name ?? "", input.tool_input ?? {}, input.cwd ?? "");
    const p: Pending = {
      id: crypto.randomUUID(),
      created: Date.now(),
      tool: input.tool_name ?? "?",
      project: basename(input.cwd ?? ""),
      host: oneLine(input.host ?? "", 40),
      headline: card.headline,
      intent: oneLine(input.intent ?? "", 600),
      facts: card.facts,
      lines: card.body.length,
    };
    const writes: Record<string, unknown> = { [`req:${p.id}`]: p };
    for (let c = 0; c * CHUNK < card.body.length; c++) writes[`body:${p.id}:${c}`] = card.body.slice(c * CHUNK, (c + 1) * CHUNK);
    // put() takes at most 128 keys per call.
    const entries = Object.entries(writes);
    for (let i = 0; i < entries.length; i += 128) await this.ctx.storage.put(Object.fromEntries(entries.slice(i, i + 128)));
    return p.id;
  }

  async status(id: string): Promise<{ status: "gone" | "pending" | "decided"; behavior?: Behavior; message?: string }> {
    const key = `req:${id}`;
    const p = await this.ctx.storage.get<Pending>(key);
    if (!p) return { status: "gone" };
    if (!p.decision) return { status: "pending" };
    // The hook has its answer; nothing else needs this request.
    await this.remove(id);
    return { status: "decided", ...p.decision };
  }

  async cancel(id: string): Promise<void> {
    await this.remove(id);
  }

  async chunk(id: string, n: number): Promise<string[]> {
    return (await this.ctx.storage.get<string[]>(`body:${id}:${n}`)) ?? [];
  }

  async purge(): Promise<void> {
    await this.ctx.storage.deleteAll();
  }

  private async remove(id: string): Promise<void> {
    const body = await this.ctx.storage.list({ prefix: `body:${id}:` });
    const keys = [`req:${id}`, ...body.keys()];
    for (let i = 0; i < keys.length; i += 128) await this.ctx.storage.delete(keys.slice(i, i + 128));
  }

  // Called by the device; also marks this profile as watched.
  async next(): Promise<{ count: number; head?: Pending }> {
    await this.ctx.storage.put("device:lastSeen", Date.now());
    await this.prune();
    const all = await this.ctx.storage.list<Pending>({ prefix: "req:" });
    const open = [...all.values()].filter((p) => !p.decision).sort((a, b) => a.created - b.created);
    return { count: open.length, head: open[0] };
  }

  async decide(id: string, decision: NonNullable<Pending["decision"]>): Promise<boolean> {
    await this.ctx.storage.put("device:lastSeen", Date.now());
    const key = `req:${id}`;
    const p = await this.ctx.storage.get<Pending>(key);
    if (!p) return false;
    p.decision = decision;
    await this.ctx.storage.put(key, p);
    return true;
  }

  private async prune(): Promise<void> {
    const all = await this.ctx.storage.list<Pending>({ prefix: "req:" });
    for (const [, p] of all) if (Date.now() - p.created > REQUEST_TTL_MS) await this.remove(p.id);
  }
}

interface HookInput {
  intent?: string; // added by lgtm-hook from the transcript
  host?: string; // added by lgtm-hook
  tool_name?: string;
  tool_input?: Record<string, unknown>;
  cwd?: string;
}

function basename(p: string): string {
  return p.split("/").filter(Boolean).pop() ?? "";
}

function text(body: string, status = 200): Response {
  return new Response(body, { status, headers: { "content-type": "text/plain; charset=utf-8" } });
}
