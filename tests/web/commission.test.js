// The connection panel in jsdom: DER file -> PEM, POST carries the anti-forgery header, token never prefilled.
const { JSDOM } = require("jsdom");
const fs = require("fs");
const { TextDecoder } = require("util");
const [html, metrics, pemPath] = [fs.readFileSync(process.argv[2], "utf8"), JSON.parse(fs.readFileSync(process.argv[3], "utf8")), process.argv[4]];
const pem = fs.readFileSync(pemPath, "utf8");
const der = Buffer.from(pem.replace(/-----[^-]+-----|\s/g, ""), "base64");
let fails = 0; const expect = (ok, what) => { console.log((ok ? "PASS " : "FAIL ") + what); if (!ok) fails++; };
const posts = [];
const view = { commissioned: false, url: "", deviceId: "", tokenSet: false, cert: { kind: "none" }, status: "Not commissioned" };
const fetchImpl = (url, opts = {}) => {
  if (url.includes("?commission") && opts.method === "POST") {
    posts.push({ headers: opts.headers, body: JSON.parse(opts.body) });
    return Promise.resolve({ ok: true, json: () => Promise.resolve({ commissioned: true, url: "https://pharos.site.local", deviceId: "aurix-lobby-01",
      tokenSet: true, cert: { kind: "pem", subject: "pharos.site.local", fingerprint: "AA:BB", notAfter: Date.now() + 1e9 }, status: "Connecting" }) });
  }
  if (url.includes("?commission")) return Promise.resolve({ ok: true, json: () => Promise.resolve(view) });
  return Promise.resolve({ ok: true, json: () => Promise.resolve(metrics) });
};
const dom = new JSDOM(html, { runScripts: "dangerously", pretendToBeVisual: true,
  beforeParse(w) { w.fetch = fetchImpl; w.TextDecoder = TextDecoder; w.confirm = () => true; } });
const errors = []; dom.virtualConsole.on("jsdomError", e => errors.push(String(e)));
const w = dom.window, d = w.document, wait = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  await wait(300);
  expect(d.getElementById("connStatus").textContent.includes("not connected"), "panel shows not-connected state");
  d.getElementById("cUrl").value = "https://pharos.site.local";
  d.getElementById("cId").value = "aurix-lobby-01";
  d.getElementById("cTok").value = "secret-token-123";
  const file = new w.File([der], "pharos.cer", { type: "application/x-x509-ca-cert" });
  const input = d.getElementById("cFile");
  Object.defineProperty(input, "files", { value: [file] });
  input.dispatchEvent(new w.Event("change"));
  await wait(200);
  d.getElementById("connForm").dispatchEvent(new w.Event("submit", { cancelable: true }));
  await wait(300);
  const p = posts[0] || { headers: {}, body: {} };
  expect(posts.length === 1, "one save request sent");
  expect(p.headers["X-AURIX-Request"] === "1", "save carries the anti-forgery header");
  const sent = (p.body.cert || "").replace(/-----[^-]+-----|\s/g, "");
  expect((p.body.cert || "").startsWith("-----BEGIN CERTIFICATE-----") && Buffer.from(sent, "base64").equals(der), "binary DER file sent as the same certificate in PEM");
  expect(p.body.token === "secret-token-123" && p.body.deviceId === "aurix-lobby-01", "token and device ID sent");
  expect(d.getElementById("cTok").value === "" && d.getElementById("cTok").placeholder.includes("Leave blank"), "token field cleared after save, never shown again");
  expect(!d.getElementById("certBox").hidden && d.getElementById("certBox").textContent.includes("pharos.site.local"), "certificate details shown for checking");
  expect(errors.length === 0, "no JS errors" + (errors.length ? ": " + errors[0] : ""));
  console.log(fails ? fails + " failure(s)" : "all connection-panel checks passed");
  process.exit(fails ? 1 : 0);
})();
