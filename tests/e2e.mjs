// Drives the real ZeroZip window over the DevTools protocol.
//   node tests/e2e.mjs PORT OUTDIR
// ZeroZip must be running with WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--remote-debugging-port=PORT
// and a zip passed on its command line.
import { writeFileSync } from "node:fs";
import { join } from "node:path";

const [port, outDir] = process.argv.slice(2);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let page;
for (let i = 0; i < 60 && !page; i++) {
  try {
    const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
    page = list.find((t) => t.type === "page" && t.url.startsWith("https://zerozip.example/"));
  } catch {}
  if (!page) await sleep(500);
}
if (!page) { console.log("no page"); process.exit(1); }

const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener("open", r));
let id = 0;
const waiting = new Map();
ws.addEventListener("message", (e) => {
  const m = JSON.parse(e.data);
  if (m.id && waiting.has(m.id)) { waiting.get(m.id)(m); waiting.delete(m.id); }
});
const call = (method, params = {}) => new Promise((r) => { const n = ++id; waiting.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });
const js = async (expr) => (await call("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true })).result?.result?.value;
const shot = async (name) => {
  const r = await call("Page.captureScreenshot", { format: "png" });
  writeFileSync(join(outDir, name + ".png"), Buffer.from(r.result.data, "base64"));
};
const until = async (expr, ms = 120000) => {
  const t = Date.now();
  while (Date.now() - t < ms) { if (await js(expr)) return true; await sleep(200); }
  return false;
};

if (!(await until("S.view === 'analysis'", 30000))) { console.log("never reached analysis, view=" + (await js("S.view"))); process.exit(1); }
await sleep(1800);
await shot("1-analysis");
console.log("analysis", JSON.stringify(await js("({fits:S.a.fits, files:S.a.files, dest:S.a.dest, minFree:S.a.minFree})")));

await js("document.querySelector('#start').click()");
await sleep(700);
await shot("2-confirm");
await js("document.querySelector('#modal [data-act=go]').click()");
await until("S.view === 'running' && S.last && S.last.done > 2", 30000);
await sleep(1200);
await shot("3-running");

// pause, check it holds, resume
await js("document.querySelector('#pause').click()");
await sleep(800);
const a = await js("S.last.bytesDone");
await sleep(1200);
const b = await js("S.last.bytesDone");
console.log("paused holds:", a === b, "status:", await js("document.querySelector('#status').textContent"));
await js("document.querySelector('#pause').click()");

if (process.argv.includes("--stop")) {
  // stop part way through, then resume from the end screen
  await sleep(1500);
  await js("document.querySelector('#stop').click()");
  await sleep(500);
  await js("document.querySelector('#modal [data-act=stopnow]').click()");
  if (!(await until("S.view === 'end' && S.endKind === 'stopped'", 60000))) { console.log("stop failed, view=" + (await js("S.view + ' ' + S.endKind"))); process.exit(1); }
  await sleep(1200);
  await shot("5-stopped");
  console.log("stopped:", await js("document.querySelector('#end .sub').textContent"));
  await js("document.querySelector('#end [data-act=resume]').click()");
  if (!(await until("S.view === 'analysis' && S.a.resuming", 30000))) { console.log("resume analysis failed"); process.exit(1); }
  await sleep(1800);
  await shot("6-resume");
  console.log("resume:", JSON.stringify(await js("({present:S.a.present, files:S.a.files, zipBytes:S.a.zipBytes, banner:document.querySelector('#banner').textContent.trim()})")));
  await js("document.querySelector('#start').click()");
  await sleep(600);
  await js("document.querySelector('#modal [data-act=go]').click()");
}

if (!(await until("S.view === 'end' && S.endKind !== 'stopped'", 600000))) { console.log("never finished"); process.exit(1); }
await sleep(1500);
await shot("4-end");
console.log("end", await js("S.endKind"), await js("document.querySelector('#end h2').textContent"));
ws.close();
