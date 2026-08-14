// SPDX-License-Identifier: GPL-3.0-or-later
// bench-run.mjs —— S3 spike headless 基准 runner(本地/复现用;由 T26 的 PR 删除)。
//   起本地静态 server(spikes/s3/web/output)→ headless Edge(--headless=new)→ CDP 读 window.__benchResult。
//   产出页面自带测量(帧时分布/静态层重绘/内存/锐度/溢出)。用法:
//     node spikes/s3/bench-run.mjs [--port <http-port>] [--timeout-ms <ms>]
//   依赖:本机已装 Edge(或 WebView2 Runtime);Node >= 21(全局 fetch/WebSocket)。

import http from "node:http";
import fs from "node:fs";
import path from "node:path";
import os from "node:os";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const WEBROOT = path.join(__dirname, "web", "output");

const EDGE =
    process.env.EDGE ||
    "C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe";
const DEBUG_PORT = 9333 + Math.floor(Math.random() * 500);
const TIMEOUT_MS =
    Number(process.argv[process.argv.indexOf("--timeout-ms") + 1] || 0) ||
    60000;

const MIME = {
    ".html": "text/html",
    ".js": "text/javascript",
    ".mjs": "text/javascript",
    ".css": "text/css",
    ".json": "application/json",
    ".svg": "image/svg+xml",
};

function serve() {
    return http
        .createServer((req, res) => {
            let p = decodeURIComponent(new URL(req.url, "http://x").pathname);
            if (p === "/") p = "/index.html";
            const file = path.join(WEBROOT, p);
            if (
                !file.startsWith(WEBROOT) ||
                !fs.existsSync(file) ||
                !fs.statSync(file).isFile()
            ) {
                res.writeHead(404);
                res.end("not found");
                return;
            }
            res.writeHead(200, {
                "Content-Type":
                    MIME[path.extname(file)] || "application/octet-stream",
            });
            fs.createReadStream(file).pipe(res);
        })
        .listen(0, "127.0.0.1");
}

function sleep(ms) {
    return new Promise((r) => setTimeout(r, ms));
}

async function getPageWs(port) {
    const deadline = Date.now() + 15000;
    while (Date.now() < deadline) {
        try {
            const list = await (
                await fetch("http://127.0.0.1:" + port + "/json")
            ).json();
            const page = list.find(
                (t) => t.type === "page" && t.url.includes("index.html"),
            );
            if (page && page.webSocketDebuggerUrl)
                return page.webSocketDebuggerUrl;
        } catch {
            /* edge not up yet */
        }
        await sleep(300);
    }
    throw new Error("CDP page target not found within 15s");
}

function connectCdp(wsUrl) {
    return new Promise((resolve, reject) => {
        const ws = new WebSocket(wsUrl);
        const pending = new Map();
        let idc = 0;
        ws.onopen = () => {
            const send = (method, params) =>
                new Promise((res, rej) => {
                    const id = ++idc;
                    pending.set(id, { res, rej });
                    ws.send(JSON.stringify({ id, method, params }));
                });
            resolve({ ws, send, close: () => ws.close() });
        };
        ws.onerror = () => reject(new Error("ws error"));
        ws.onmessage = (ev) => {
            const msg = JSON.parse(ev.data);
            if (msg.id && pending.has(msg.id)) {
                const { res, rej } = pending.get(msg.id);
                pending.delete(msg.id);
                if (msg.error) rej(new Error(JSON.stringify(msg.error)));
                else res(msg.result);
            }
        };
    });
}

async function evalJson(cdp, expr) {
    const r = await cdp.send("Runtime.evaluate", {
        expression: expr,
        returnByValue: true,
    });
    return r && r.result && r.result.value !== undefined
        ? r.result.value
        : null;
}

async function main() {
    const server = serve();
    await new Promise((r) => server.once("listening", r));
    const httpPort = server.address().port;

    const userDir = fs.mkdtempSync(path.join(os.tmpdir(), "scvb-s3-edge-"));
    const url = "http://127.0.0.1:" + httpPort + "/index.html?bench=1";

    const edge = spawn(
        EDGE,
        [
            "--headless=new",
            "--disable-gpu",
            "--no-first-run",
            "--no-default-browser-check",
            "--hide-scrollbars",
            "--remote-debugging-port=" + DEBUG_PORT,
            "--user-data-dir=" + userDir,
            "--window-size=1180,780",
            url,
        ],
        { stdio: "ignore" },
    );

    let cdp;
    try {
        const wsUrl = await getPageWs(DEBUG_PORT);
        cdp = await connectCdp(wsUrl);
        await cdp.send("Runtime.enable", {});

        const deadline = Date.now() + TIMEOUT_MS;
        let result = null;
        while (Date.now() < deadline) {
            result = await evalJson(
                cdp,
                "window.__benchResult ? JSON.stringify(window.__benchResult) : null",
            );
            if (result) break;
            await sleep(2000);
        }
        if (!result) {
            console.error("TIMEOUT: page did not produce __benchResult");
            process.exitCode = 2;
        } else {
            console.log(result);
        }
    } finally {
        try {
            cdp && cdp.close();
        } catch {
            /* ignore */
        }
        edge.kill();
        server.close();
        try {
            fs.rmSync(userDir, { recursive: true, force: true });
        } catch {
            /* 临时目录可能被 Edge 锁住,尽力清理即可 */
        }
    }
}

main().catch((e) => {
    console.error("bench-run failed:", e.message);
    process.exit(1);
});
