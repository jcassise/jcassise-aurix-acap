#!/usr/bin/env python3
"""Identity-sync scenarios (protocol v1 §5): real AURIX sync code (harness) vs sim_lite. Exit 0 = all pass."""
import json, os, re, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sim_lite import Sim

HARNESS = sys.argv[1] if len(sys.argv) > 1 else "./pharos_harness"
ENV = dict(os.environ, AURIX_SYNC_JITTER_MS="0")
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if detail and not ok else ""))


def run(sim, seconds, state_dir, during=None):
    p = subprocess.Popen([HARNESS, sim.url, sim.device_id, sim.token, sim.cert["spki_pin"], state_dir, str(seconds)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=ENV)
    if during:
        during(p)
    out, _ = p.communicate(timeout=seconds + 30)
    return out


def last_people(out):
    lines = [l for l in out.splitlines() if l.startswith("PEOPLE ")]
    if not lines:
        return {}
    head, _, names = lines[-1][7:].partition(" names=")
    m = dict(kv.split("=", 1) for kv in head.split(" "))
    m["names"] = [n for n in names.split(",") if n]
    return m


def full_syncs(sim):
    return sum(1 for (p, s_, q) in sim.log if p.endswith("/sync/people") and q is not None
               and "since" not in q and "cursor" not in q)


def last_status(sim):
    st = sim.statuses()
    return st[-1] if st else {}


sim = Sim(tempfile.mkdtemp()).start()
sd = tempfile.mkdtemp()

# S4 / P1 initial full sync: 3 people, one Threat with 2 photos
sim.add_person("p-1", "Dana Ruiz")
sim.add_person("p-2", "Marco Ilic")
sim.add_person("p-3", "Watchlist Test", watchlist="threat", photos=2)
out = run(sim, 3, sd)
pp = last_people(out)
check("P1 full sync loads everyone", pp.get("n") == "3" and pp.get("ready") == "4", str(pp))
check("P1 each photo downloaded once", sorted(sim.downloads.values()) == [1, 1, 1, 1], str(sim.downloads))
st = last_status(sim)
check("P1 status reports the sync", st.get("sync", {}).get("revision") == sim.revision and
      st["sync"].get("templatesReady") == 4 and st["sync"].get("people") == 3, str(st.get("sync")))
hello = [j for (p, s_, j) in sim.log if p.endswith("/hello") and j]
check("P1 hello reports capacity", hello and hello[-1].get("limits", {}).get("maxPeople") == 20000, str(hello[-1:]))
check("P1 people stored on the camera", os.path.exists(os.path.join(sd, "people.json")))
check("P1 contract respected both ways", not sim.violations, "; ".join(sim.violations[:3]))

# S4 / P2 delta: add a Threat later -> only its photo downloaded
sim.add_person("p-4", "Second Threat", watchlist="threat")
out = run(sim, 2.5, sd)
pp = last_people(out)
check("P2 delta adds the new person", pp.get("n") == "4" and "Second Threat" in pp.get("names", []), str(pp))
check("P2 only the new photo downloaded", sim.downloads.get(("p-4", "ph-p-4-0")) == 1 and
      all(v == 1 for v in sim.downloads.values()), str(sim.downloads))

# P3 photo replaced -> re-downloaded once, template rebuilt
sim.replace_photo("p-1", 0, seed=4242)
out = run(sim, 2.5, sd)
check("P3 changed photo re-downloaded once", sim.downloads.get(("p-1", "ph-p-1-0")) == 2 and
      sim.downloads.get(("p-2", "ph-p-2-0")) == 1, str(sim.downloads))

# S7 / P4 deletion via /sync/deletions, then via deleted[] inside the page
sim.delete_person("p-2")
out = run(sim, 2.5, sd)
pp = last_people(out)
check("P4 deleted person removed (deletions endpoint)", pp.get("n") == "3" and "Marco Ilic" not in pp.get("names", []), str(pp))
sim.inline_deletions = True
sim.delete_person("p-4")
out = run(sim, 2.5, sd)
pp = last_people(out)
check("P4 deleted person removed (inline deleted[])", pp.get("n") == "2" and "Second Threat" not in pp.get("names", []), str(pp))
sim.inline_deletions = False

# P9 restart: persisted list used, nothing downloaded again
before = dict(sim.downloads)
out = run(sim, 2, sd)
pp = last_people(out)
check("P9 restart keeps people and templates, no re-download", pp.get("n") == "2" and pp.get("ready") == "3"
      and sim.downloads == before, f"{pp} {sim.downloads}")

# S6 / P5 full sync fails on page 3 of 5 -> previous list unchanged, retried later
for i in range(5, 13):
    sim.add_person(f"p-{i}", f"Person {i}")
sim.page_size = 2                      # 10 people -> 5 pages
pre = run(sim, 3, sd)                  # delta picks the new ones up first
n_before = last_people(pre).get("n")
sim.sync_fault = {"page": 3, "status": 500}
sim.commands = [{"commandId": "rs-1", "type": "resync", "args": {}}]
out = run(sim, 3, sd)
pp = last_people(out)
st = last_status(sim)
check("P5 failed full sync keeps the previous list", pp.get("n") == n_before == "10", f"{n_before} -> {pp.get('n')}")
check("P5 failure reported in status", "keeping the current list" in (st.get("sync", {}).get("lastError") or ""),
      str(st.get("sync", {}).get("lastError")))
sim.sync_fault = None
sim.commands = []
out = run(sim, 13, sd)
check("P5 full sync retried and applied later", last_people(out).get("n") == "10" and
      not (last_status(sim).get("sync", {}).get("lastError")), str(last_status(sim).get("sync")))

# P6 snapshot expires mid full sync (409 on page 2, once) -> restarts from page 1 and succeeds
sim.sync_fault = {"page": 2, "status": 409, "once": True}
fs0 = full_syncs(sim)
sim.commands = [{"commandId": "rs-2", "type": "resync", "args": {}}]
out = run(sim, 3, sd)
check("P10 resync command executed and reported done", sim.results_seen.get("rs-2", {}).get("status") == "done",
      str(sim.results_seen.get("rs-2")))
check("P6 409 restarts the sync and it completes", full_syncs(sim) - fs0 >= 2 and last_people(out).get("n") == "10"
      and not last_status(sim).get("sync", {}).get("lastError"), f"{full_syncs(sim) - fs0} {last_status(sim).get('sync')}")

# P7 short count (total claims one more) -> not applied
sim.add_person("p-99", "Never Applied")      # would show up if the truncated full sync were applied...
sim.sync_fault = {"truncate": True}
sim.commands = [{"commandId": "rs-3", "type": "resync", "args": {}}]
out = run(sim, 3, sd)
check("P7 count mismatch: full sync not applied", "received" in (last_status(sim).get("sync", {}).get("lastError") or ""),
      str(last_status(sim).get("sync")))
sim.sync_fault = None
sim.commands = []
sim.delete_person("p-99")
run(sim, 13, sd)

# P8 photo 404 and wrong bytes -> counted as failed, person kept
sim.add_person("p-50", "Missing Photo")
sim.add_person("p-51", "Bad Bytes")
sim.photo_fault[("p-50", "ph-p-50-0")] = 404
sim.photo_fault[("p-51", "ph-p-51-0")] = "badbytes"
out = run(sim, 3, sd)
pp = last_people(out)
st = last_status(sim)
check("P8 failed photos counted, people kept", "Missing Photo" in pp.get("names", []) and "Bad Bytes" in pp.get("names", [])
      and st.get("sync", {}).get("templatesFailed") == 2, f"{pp} {st.get('sync')}")

# P10 resync command reported done; P11 reenroll rebuilds one person's templates
sim.photo_fault = {}
sim.results_seen.clear()
sim.commands = [{"commandId": "re-1", "type": "reenroll", "args": {"personIds": ["p-3"]}}]
d0 = sim.downloads.get(("p-3", "ph-p-3-0"), 0)
out = run(sim, 3, sd)
check("P11 reenroll re-downloads that person's photos once", sim.downloads.get(("p-3", "ph-p-3-0"), 0) == d0 + 1 and
      sim.results_seen.get("re-1", {}).get("status") == "done", f"{d0} -> {sim.downloads.get(('p-3', 'ph-p-3-0'))}")

# P12 zones change -> full sync
sim.commands = []
fs0 = full_syncs(sim)
sim.set_config({"site.zones": ["Lobby"]})
run(sim, 2, sd)          # first config seen after restart is the baseline
fs1 = full_syncs(sim)
sim.set_config({"site.zones": ["Lobby", "East Entry"]})          # changed while the camera is off
out = run(sim, 3, sd)
check("P12 zone change triggers a full sync (even across a restart)", full_syncs(sim) > fs1, f"{fs1} -> {full_syncs(sim)}")
fs2 = full_syncs(sim)
def change_live(p):
    time.sleep(1.5)
    sim.set_config({"site.zones": ["Lobby"]})                       # changed while running
out = run(sim, 4, sd, during=change_live)
check("P12 zone change while running triggers a full sync", full_syncs(sim) > fs2, f"{fs2} -> {full_syncs(sim)}")

# P13 clock correction: Pharos 60 s ahead -> status time follows Pharos
sim.clock_skew_ms = 60000
run(sim, 2.5, sd)
st = last_status(sim)
skew = st.get("time", 0) - int(time.time() * 1000)
check("P13 status time corrected to Pharos time", 55000 < skew < 65000 and abs(st.get("health", {}).get("clockDriftMs", 0) + 60000) < 5000,
      f"time skew {skew} ms, drift {st.get('health', {}).get('clockDriftMs')}")
check("P* contract respected throughout", not sim.violations, "; ".join(sim.violations[:3]))
sim.stop()

failed = [n for n, ok in results if not ok]
print(f"\n{len(results) - len(failed)}/{len(results)} passed")
sys.exit(1 if failed else 0)
