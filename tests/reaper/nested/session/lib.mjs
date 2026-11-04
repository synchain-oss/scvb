// SPDX-License-Identifier: GPL-3.0-or-later
// Helpers for the session scripts: the file RPC to REAPER (ext/scvb-nested.lua) and SCVB's real bridge over WebView2's
// remote debugging port. The bridge calls go through the same native functions the plugin's own page uses, so a
// project saved at the end of a session holds what a user's clicks would have left in it.
import {
    appendFileSync,
    existsSync,
    readFileSync,
    renameSync,
    writeFileSync,
} from "node:fs";
import { join } from "node:path";

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function parseArgs(argv = process.argv.slice(2)) {
    const out = { rest: [] };
    for (let i = 0; i < argv.length; i++) {
        if (argv[i].startsWith("--") && i + 1 < argv.length) {
            out[argv[i].slice(2)] = argv[++i];
        } else {
            out.rest.push(argv[i]);
        }
    }
    return out;
}

export function log(...parts) {
    const line = parts
        .map((p) => (typeof p === "string" ? p : JSON.stringify(p)))
        .join(" ");
    console.log(line);
    if (process.env.SCVB_SESSION_LOG)
        appendFileSync(process.env.SCVB_SESSION_LOG, line + "\n");
}

export function finish(outcomeFile, status, detail) {
    writeFileSync(outcomeFile, JSON.stringify({ status, detail }));
    log("outcome", status, detail);
}

export class Reaper {
    constructor(dir) {
        this.dir = dir;
        this.n = 1;
    }
    async ready(timeoutMs = 120000) {
        const t0 = Date.now();
        while (!existsSync(join(this.dir, "ready"))) {
            if (Date.now() - t0 > timeoutMs)
                throw new Error("REAPER did not get ready");
            await sleep(100);
        }
    }
    async cmd(...parts) {
        const n = this.n++;
        const tmp = join(this.dir, `cmd-${n}.part`);
        writeFileSync(tmp, parts.join("\t"));
        renameSync(tmp, join(this.dir, `cmd-${n}.txt`));
        const res = join(this.dir, `res-${n}.txt`);
        const t0 = Date.now();
        while (!existsSync(res)) {
            if (Date.now() - t0 > 60000)
                throw new Error(`no answer to ${parts.join(" ")}`);
            await sleep(20);
        }
        const text = readFileSync(res, "utf8");
        const nl = text.indexOf("\n");
        const status = text.slice(0, nl);
        const body = text.slice(nl + 1);
        if (status !== "ok") throw new Error(`${parts.join(" ")}: ${body}`);
        return body;
    }
}

async function targets(port) {
    try {
        const r = await fetch(`http://127.0.0.1:${port}/json/list`);
        return (await r.json()).filter(
            (t) =>
                t.webSocketDebuggerUrl &&
                /\/(input|output|monitor)\/index\.html/.test(t.url),
        );
    } catch {
        return [];
    }
}

export class Page {
    constructor(ws, target) {
        this.ws = ws;
        this.target = target;
        this.id = 0;
        this.pending = new Map();
        ws.addEventListener("message", (ev) => {
            const m = JSON.parse(ev.data);
            if (m.id && this.pending.has(m.id)) {
                this.pending.get(m.id)(m);
                this.pending.delete(m.id);
            }
        });
    }
    send(method, params = {}) {
        const id = ++this.id;
        return new Promise((res) => {
            this.pending.set(id, res);
            this.ws.send(JSON.stringify({ id, method, params }));
        });
    }
    async eval(expression) {
        const m = await this.send("Runtime.evaluate", {
            expression,
            awaitPromise: true,
            returnByValue: true,
        });
        if (m.result?.exceptionDetails) {
            throw new Error(
                m.result.exceptionDetails.exception?.description ||
                    m.result.exceptionDetails.text,
            );
        }
        return m.result?.result?.value;
    }
    // A native function of the plugin's bridge, through JUCE's own helper module (the instance the page uses).
    async call(name, ...args) {
        const a = JSON.stringify(args);
        return this.eval(`(async () => {
            const m = await import(new URL("../js/juce/index.js", location.href).href);
            return await m.getNativeFunction(${JSON.stringify(name)})(...${a});
        })()`);
    }
    // Starts collecting a bridge event in the page; latest() reads the newest payload.
    async watch(evt) {
        await this.eval(`(() => {
            window.__t = window.__t || {};
            if (!window.__t[${JSON.stringify(evt)}]) {
                window.__t[${JSON.stringify(evt)}] = { n: 0 };
                window.__JUCE__.backend.addEventListener(${JSON.stringify(evt)}, (p) => {
                    const s = window.__t[${JSON.stringify(evt)}];
                    s.n++;
                    s.last = p;
                });
            }
        })()`);
    }
    latest(evt) {
        return this.eval(
            `window.__t && window.__t[${JSON.stringify(evt)}] ? window.__t[${JSON.stringify(evt)}].last : null`,
        );
    }
    close() {
        this.ws.close();
    }
}

async function connect(target) {
    const ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((res, rej) => {
        ws.addEventListener("open", res, { once: true });
        ws.addEventListener(
            "error",
            (e) => rej(new Error("websocket: " + (e.message || "failed"))),
            { once: true },
        );
    });
    const page = new Page(ws, target);
    await page.send("Runtime.enable");
    // The page's module graph must be up before its bridge helper can be imported.
    for (let i = 0; i < 100; i++) {
        const ok = await page.eval(
            `!!(window.__JUCE__ && window.__JUCE__.backend && document.readyState === "complete")`,
        );
        if (ok) break;
        await sleep(100);
    }
    return page;
}

// Opens the FX window and connects to the page that appears for it. known: ids of pages already open.
// A project saved with FX windows open reopens them by itself, so several pages can be new at once. With isWanted the
// page is picked by what its own bridge reports, not by the order the browser lists them in.
export async function openFx(
    reaper,
    port,
    track,
    fx,
    known = new Set(),
    timeoutMs = 60000,
    isWanted = null,
) {
    await reaper.cmd("show", track, fx);
    const t0 = Date.now();
    for (;;) {
        for (const target of (await targets(port)).filter(
            (t) => !known.has(t.id),
        )) {
            const page = await connect(target);
            if (!isWanted || (await isWanted(page))) return page;
            page.close();
        }
        if (Date.now() - t0 > timeoutMs)
            throw new Error(`no page appeared for track ${track} fx ${fx}`);
        await sleep(200);
    }
}

export async function closeFx(reaper, page, track, fx, port) {
    const id = page.target.id;
    page.close();
    await reaper.cmd("hide", track, fx);
    const t0 = Date.now();
    while (Date.now() - t0 < 15000) {
        if (!(await targets(port)).some((t) => t.id === id)) return;
        await sleep(200);
    }
}
