const { JSDOM } = require("jsdom");
const fs = require("fs");
const html = fs.readFileSync(process.argv[2], "utf8");
const metrics = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
function load(fetchImpl) {
  const errors = [];
  const dom = new JSDOM(html, { runScripts: "dangerously", pretendToBeVisual: true,
    beforeParse(w) { w.fetch = fetchImpl; w.addEventListener("error", e => errors.push(e.message)); } });
  dom.virtualConsole.on("jsdomError", e => errors.push(String(e)));
  return { dom, errors };
}
const wait = ms => new Promise(r => setTimeout(r, ms));
let failures = 0;
const expect = (ok, what) => { if (!ok) { failures++; console.log("FAIL", what); } else console.log("PASS", what); };
(async () => {
  // 1) live data from the C endpoint
  const live = load(() => Promise.resolve({ ok: true, json: () => Promise.resolve(metrics) }));
  await wait(300);
  const d = live.dom.window.document, t = id => d.getElementById(id).textContent;
  console.log("fps:", t("fps"), "|", t("fpsTarget"));
  console.log("budget:", t("budgetNote"));
  console.log("ident:", t("ident"));
  console.log("people:", t("people"), t("peopleSplit"), "| pharos:", t("pharosText"), "| demo banner hidden:", d.getElementById("demo").hidden);
  const segs = [...d.querySelectorAll(".seg")].map(s => s.style.flexBasis);
  console.log("segments:", segs.join(" "));
  const rows = d.querySelectorAll("#recent tr");
  console.log("recent rows:", rows.length, "| first name cell:", JSON.stringify(rows[0].children[1].textContent),
              "| raw HTML injected?", d.querySelector("#recent b") !== null);
  console.log("chart polylines:", d.querySelectorAll("#chartFps polyline").length, d.querySelectorAll("#chartSys polyline").length);
  console.log("JS errors:", live.errors.length ? live.errors : "none");
  expect(t("fps") === "10.0", "fps rendered from endpoint");
  expect(t("ident").includes("P3267"), "device identity rendered");
  expect(d.getElementById("demo").hidden, "no demo banner with live data");
  expect(rows.length > 0 && d.querySelector("#recent b") === null, "recent matches rendered as text (no HTML injection)");
  expect(d.querySelectorAll("#chartFps polyline").length === 2, "frame-rate chart drawn");
  expect(live.errors.length === 0, "no JS errors with live data");
  console.log("capacity:", t("capHead"), "|", t("capSub").slice(0, 90));
  expect(/^About [\d,]+ people, limited by (memory|app storage|matching speed)\.$/.test(t("capHead")), "capacity headline rendered");
  expect(d.querySelectorAll(".limit.binding").length === 1, "exactly one limit marked as binding");
  expect(t("galMem") === "0.6 KB", "gallery memory shown");
  // 2) camera unreachable -> simulated preview after two failures
  const off = load(() => Promise.reject(new Error("offline")));
  await wait(2600);
  const d2 = off.dom.window.document;
  console.log("offline -> demo banner shown:", !d2.getElementById("demo").hidden, "| fps:", d2.getElementById("fps").textContent,
              "| errors:", off.errors.length ? off.errors : "none");
  expect(!d2.getElementById("demo").hidden && off.errors.length === 0, "offline falls back to labelled simulated preview");
  // 3) live then lost -> stale banner, no switch to demo
  let n = 0;
  const flaky = load(() => (n++ === 0) ? Promise.resolve({ ok: true, json: () => Promise.resolve(metrics) }) : Promise.reject(new Error("lost")));
  await wait(2600);
  const d3 = flaky.dom.window.document;
  console.log("lost after live -> stale shown:", !d3.getElementById("stale").hidden, "| demo shown:", !d3.getElementById("demo").hidden);
  expect(!d3.getElementById("stale").hidden && d3.getElementById("demo").hidden, "lost contact shows stale banner, never fake data");
  console.log(failures ? failures + " failure(s)" : "all dashboard checks passed");
  process.exit(failures ? 1 : 0);
})();
