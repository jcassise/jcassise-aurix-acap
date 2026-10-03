// Settings section in jsdom: built from the camera's list, Pharos keys locked, saves only changes with the header.
const { JSDOM } = require("jsdom");
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8"), metrics = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
let fails = 0; const expect = (ok, what) => { console.log((ok ? "PASS " : "FAIL ") + what); if (!ok) fails++; };
const desc = { pharosManaged: true, groups: [
  { name: "Recognition", settings: [
    { key: "recognition.matchThreshold", label: "Match threshold", help: "h", type: "number", min: 0.3, max: 0.8, step: 0.01, value: 0.5, default: 0.45, source: "pharos" },
    { key: "recognition.maxYawDeg", label: "Maximum head turn", help: "h", type: "integer", min: 15, max: 90, step: 5, unit: "°", value: 50, default: 50, source: "default" } ] },
  { name: "Live view", settings: [
    { key: "overlay.showScores", label: "Show match scores", help: "h", type: "boolean", value: false, default: false, source: "default" },
    { key: "overlay.offsetY", label: "Vertical position correction", help: "h", type: "number", min: -0.2, max: 0.2, step: 0.005, value: -0.03, default: 0, source: "local" } ] } ] };
const posts = [];
const fetchImpl = (url, o = {}) => {
  if (url.includes("?settings") && o.method === "POST") { posts.push({ headers: o.headers, body: JSON.parse(o.body) });
    return Promise.resolve({ ok: true, json: () => Promise.resolve(desc) }); }
  if (url.includes("?settings")) return Promise.resolve({ ok: true, json: () => Promise.resolve(desc) });
  if (url.includes("?commission")) return Promise.resolve({ ok: true, json: () => Promise.resolve({ commissioned: true, cert: { kind: "none" } }) });
  return Promise.resolve({ ok: true, json: () => Promise.resolve(metrics) });
};
const dom = new JSDOM(html, { runScripts: "dangerously", pretendToBeVisual: true, beforeParse(w) { w.fetch = fetchImpl; } });
const errors = []; dom.virtualConsole.on("jsdomError", e => errors.push(String(e)));
const w = dom.window, d = w.document, wait = ms => new Promise(r => setTimeout(r, ms));
(async () => {
  await wait(300);
  const thr = d.getElementById("set_recognition_matchThreshold"), yaw = d.getElementById("set_recognition_maxYawDeg");
  expect(d.querySelectorAll(".sgroup").length === 2, "settings grouped as the camera describes them");
  expect(thr && thr.disabled && thr.parentNode.textContent.includes("Set by Pharos"), "Pharos-managed setting is locked and labelled");
  expect(yaw && !yaw.disabled && yaw.parentNode.textContent.includes("°"), "local setting editable, with its unit");
  expect(d.getElementById("setSave").disabled, "nothing to save until something changes");
  yaw.value = "200"; yaw.dispatchEvent(new w.Event("change"));
  expect(d.getElementById("setResult").textContent.includes("between 15 and 90") && d.getElementById("setSave").disabled, "out-of-range value caught before sending");
  yaw.value = "35"; yaw.dispatchEvent(new w.Event("change"));
  const sc = d.getElementById("set_overlay_showScores"); sc.checked = true; sc.dispatchEvent(new w.Event("change"));
  d.querySelector(".srow .reset").click();
  expect(!d.getElementById("setSave").disabled, "save enabled after a change");
  d.getElementById("setSave").click();
  await wait(200);
  const p = posts[0] || { headers: {}, body: {} };
  expect(posts.length === 1 && p.headers["X-AURIX-Request"] === "1", "one save, with the anti-forgery header");
  expect(p.body["recognition.maxYawDeg"] === 35 && p.body["overlay.showScores"] === true && p.body["overlay.offsetY"] === null &&
         !("recognition.matchThreshold" in p.body), "only the changes are sent (reset as null, Pharos key untouched)");
  expect(errors.length === 0, "no JS errors" + (errors.length ? ": " + errors[0] : ""));
  console.log(fails ? fails + " failure(s)" : "all settings-panel checks passed");
  process.exit(fails ? 1 : 0);
})();
