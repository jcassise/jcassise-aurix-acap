#!/usr/bin/env python3
"""Event scenarios (protocol v1 §7): real tracker + event builder + offline queue + uploader vs sim_lite."""
import os, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sim_lite import Sim

HARNESS = sys.argv[1] if len(sys.argv) > 1 else "./pharos_harness"
results = []


def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if detail and not ok else ""))


def run(sim, seconds, n_events, during=None, extra_env=None):
    env = dict(os.environ, AURIX_SYNC_JITTER_MS="0", AURIX_TEST_EVENTS=str(n_events), **(extra_env or {}))
    p = subprocess.Popen([HARNESS, sim.url, sim.device_id, sim.token, sim.cert["spki_pin"], tempfile.mkdtemp(), str(seconds)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    if during:
        during(p)
    out, _ = p.communicate(timeout=seconds + 30)
    return out


# S5 / E1: four visits (two known Threats, two strangers) -> four events, each opened then closed
sim = Sim(tempfile.mkdtemp()).start()
out = run(sim, 4, 4)
ev = sim.events
check("E1 one event per visit", len(ev) == 4, f"{len(ev)} events")
check("E1 every event closed (endedAt) at revision 2", all(e["revision"] == 2 and e["endedAt"] for e in ev.values()),
      str([(e["revision"], e["endedAt"]) for e in ev.values()]))
known = [e for e in ev.values() if e["person"]]
strangers = [e for e in ev.values() if e["stranger"]]
check("E1 known people carry personId and watchlist", len(known) == 2 and all(
      e["person"]["watchlist"] == "threat" and e["person"]["personId"].startswith("p-") for e in known), str(known[:1]))
check("E1 strangers have no person and stranger=true", len(strangers) == 2 and all(e["person"] is None for e in strangers))
check("E1 face and scene images for every event", all((eid, k) in sim.images for eid in ev for k in ("face", "scene")),
      f"{len(sim.images)} images")
check("E1 no image sent before its event", sim.image_early == 0, str(sim.image_early))
order_ok = True
seen = {}
for eid, rev, applied in sim.event_puts:
    if rev <= seen.get(eid, 0):
        order_ok = False
    seen[eid] = rev
check("E1 revisions arrive in increasing order", order_ok, str(sim.event_puts))
boxes = [e["face"].get("box") for e in ev.values()]
check("E1 face.box locates the person in the scene picture", all(b and 0 <= b["x"] <= 1 and 0 <= b["y"] <= 1 and 0 < b["w"] <= 1
      and 0 < b["h"] <= 1 for b in boxes) and all(isinstance(e["face"].get("boxAt"), int) for e in ev.values()), str(boxes[:1]))
check("E1 events and acks match the contract", not sim.violations, "; ".join(sim.violations[:3]))
sim.stop()

# S9 / E2: Pharos unreachable for events at first -> queued, then all delivered in order, no duplicates
sim = Sim(tempfile.mkdtemp()).start()
sim.offline = True
pending_seen = []
def recover(p):
    time.sleep(2.0)
    st = sim.statuses()
    pending_seen.append(st[-1]["queue"]["eventsPending"] if st else -1)
    sim.offline = False
out = run(sim, 6, 6, during=recover)
check("E2 queue depth reported while offline", pending_seen and pending_seen[0] >= 6, str(pending_seen))
check("E2 every event delivered after reconnect", len(sim.events) == 6 and all(e["revision"] == 2 for e in sim.events.values()),
      f"{len(sim.events)} events")
dups = len(sim.event_puts) - len({(e, r) for e, r, a in sim.event_puts})
check("E2 no duplicate deliveries", dups == 0, f"{dups} duplicates")
st = sim.statuses()[-1]
check("E2 queue empty afterwards", st["queue"]["eventsPending"] == 0 and st["queue"]["imagesPending"] == 0, str(st["queue"]))
check("E2 contract respected", not sim.violations, "; ".join(sim.violations[:3]))
sim.stop()

# E3: scene images switched off -> event says scene:false and no scene upload
sim = Sim(tempfile.mkdtemp()).start()
out = run(sim, 3, 2, extra_env={"AURIX_TEST_NO_SCENE": "1"})
check("E3 no scene image when scene images are off", len(sim.events) == 2 and all(not e["images"]["scene"] for e in sim.events.values())
      and not any(k == "scene" for (_e, k) in sim.images), str(list(sim.images)))
sim.stop()

# E4: camera-sized pictures arrive whole and byte-for-byte identical; the camera log says what left it
import hashlib, io
from PIL import Image
sim = Sim(tempfile.mkdtemp()).start()
out = run(sim, 6, 3, extra_env={"AURIX_TEST_BIG_IMAGES": "1"})
sent = {}
for l in out.splitlines():
    if l.startswith("SENT "):
        _, kind, eid, n, sha = l.split()
        sent[(eid, kind)] = (int(n), sha)
same = all(k in sim.images and len(sim.images[k]) == n and hashlib.sha256(sim.images[k]).hexdigest() == sha
           for k, (n, sha) in sent.items())
sizes = sorted(n for n, _ in sent.values())
check("E4 camera-sized pictures stored byte-for-byte identical", len(sent) == 6 and same, f"{len(sent)} sent, sizes {sizes[:2]}..{sizes[-1:]}")
ends = all(raw[-2:] == b"\xff\xd9" for raw in sim.images.values())
def decodes(raw):
    try:
        Image.open(io.BytesIO(raw)).load(); return True
    except Exception:
        return False
check("E4 every stored picture ends FF D9 and decodes", ends and all(decodes(r) for r in sim.images.values()))
logl = [l for l in out.splitlines() if " image of event " in l and "declared" in l]
check("E4 camera log: declared = sent, whole JPEG, for each picture",
      len(logl) == 6 and all("(whole JPEG)" in l and l.split("declared ")[1].split(" bytes")[0] == l.split("sent ")[1].split(",")[0] for l in logl),
      (logl[:1] or ["no log lines"])[0])
sim.stop()

failed = [n for n, ok in results if not ok]
print(f"\n{len(results) - len(failed)}/{len(results)} passed")
sys.exit(1 if failed else 0)
