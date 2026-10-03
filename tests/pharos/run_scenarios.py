#!/usr/bin/env python3
"""Step-1 protocol scenarios: real AURIX client (harness) against sim_lite. Exit 0 = all pass."""
import os, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sim_lite import Sim

HARNESS = sys.argv[1] if len(sys.argv) > 1 else "./pharos_harness"
results = []


def run(sim, seconds, trust=None, token=None, state_dir=None, device=None, during=None):
    state_dir = state_dir or tempfile.mkdtemp()
    p = subprocess.Popen([HARNESS, sim.url, device or sim.device_id, token or sim.token,
                          sim.cert["spki_pin"] if trust is None else trust, state_dir, str(seconds)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if during:
        during(p)
    out, _ = p.communicate(timeout=seconds + 30)
    return out, state_dir


def check(name, ok, detail=""):
    results.append((name, ok))
    print(("PASS " if ok else "FAIL ") + name + (f"  [{detail}]" if detail and not ok else ""))


def fresh():
    return Sim(tempfile.mkdtemp()).start()


# S1 commission and connect (SPKI pin), status every interval
sim = fresh()
out, _ = run(sim, 3.2)
st = sim.statuses()
check("S1 connected", "STATE Connected" in out, out[-300:])
check("S1 status every ~0.5 s", len(st) >= 4, f"{len(st)} statuses")
check("S1 requests match contract", not sim.violations, "; ".join(sim.violations[:3]))
sim.stop()

# S2a wrong token: 401, no status calls
sim = fresh()
out, _ = run(sim, 2, token="wrong")
check("S2a credentials rejected", "STATE Credentials rejected" in out, out[-300:])
check("S2a no status calls after 401", not sim.statuses())
sim.stop()

# S2b wrong fingerprint: refused before any HTTP request (token never sent)
sim = fresh()
out, _ = run(sim, 2, trust="ab" * 32)
check("S2b pin mismatch shown", "TLS pin mismatch" in out, out[-300:])
check("S2b no HTTP request reached the server", not sim.log, f"{len(sim.log)} requests")
sim.stop()

# S2c wrong SPKI pin: refused, no HTTP
sim = fresh()
out, _ = run(sim, 2, trust="sha256//" + "A" * 43 + "=")
check("S2c wrong SPKI pin refused", "TLS pin mismatch" in out and not sim.log, out[-200:])
sim.stop()

# S2d correct certificate fingerprint and PEM both connect
for label, trust in (("fingerprint", None), ("PEM", None)):
    sim = fresh()
    t = sim.cert["fingerprint"] if label == "fingerprint" else sim.cert["pem"]
    if label == "fingerprint":
        t = ":".join(t[i:i + 2].upper() for i in range(0, 64, 2))   # colon/upper form as browsers show
    out, _ = run(sim, 1.5, trust=t)
    check(f"S2d connects with {label}", "STATE Connected" in out, out[-300:])
    sim.stop()

# S2g fingerprint pasted exactly as openssl prints it ("sha256 Fingerprint=AB:CD:...")
sim = fresh()
fp = sim.cert["fingerprint"].upper()
out, _ = run(sim, 1.5, trust="sha256 Fingerprint=" + ":".join(fp[i:i + 2] for i in range(0, 64, 2)))
check("S2g openssl-style fingerprint connects", "STATE Connected" in out, out[-300:])
sim.stop()

# S2h bare SPKI pin and '+' mangled to spaces both connect
sim = fresh()
out, _ = run(sim, 1.5, trust=sim.cert["spki_pin"][8:])
check("S2h bare SPKI pin connects", "STATE Connected" in out, out[-300:])
sim.stop()
sim = fresh()
out, _ = run(sim, 1.5, trust=sim.cert["spki_pin"].replace("+", " "))
check("S2h pin with '+' turned into spaces connects", "STATE Connected" in out or "+" not in sim.cert["spki_pin"], out[-300:])
sim.stop()

# S2i commissioned with the server's PUBLIC KEY (what Pharos hands out), multi-line and one-line
for label, t in (("multi-line", None), ("one-line", None)):
    sim = fresh()
    key = sim.cert["pubkey_pem"] if label == "multi-line" else " ".join(sim.cert["pubkey_pem"].split())
    out, _ = run(sim, 1.5, trust=key)
    check(f"S2i public key ({label}) connects", "STATE Connected" in out, out[-300:])
    sim.stop()
# ...and a different server key is refused before any request
sim = fresh(); other = Sim(tempfile.mkdtemp())
out, _ = run(sim, 1.5, trust=other.cert["pubkey_pem"])
check("S2i wrong public key refused, no HTTP sent", "TLS pin mismatch" in out and not sim.log, out[-200:])
sim.stop()

# T1 TLS 1.3-only server (as the Pharos dev unit is configured) and T2 TLS 1.2-only server
import ssl as _ssl
sim = Sim(tempfile.mkdtemp(), tls_min=_ssl.TLSVersion.TLSv1_3).start()
out, _ = run(sim, 1.5)
check("T1 TLS 1.3-only server connects", "STATE Connected" in out, out[-300:])
sim.stop()
sim = Sim(tempfile.mkdtemp(), tls_max=_ssl.TLSVersion.TLSv1_2).start()
out, _ = run(sim, 1.5)
check("T2 TLS 1.2-only server connects", "STATE Connected" in out, out[-300:])
sim.stop()
# T3 the failure detail reaches the status text (plain HTTP on the port -> SSL error with reason)
import socket, threading as _th
srv = socket.socket(); srv.bind(("127.0.0.1", 0)); srv.listen(5); port = srv.getsockname()[1]
def _plain():
    while True:
        try:
            c, _a = srv.accept(); c.recv(1024); c.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n"); c.close()
        except OSError:
            return
_th.Thread(target=_plain, daemon=True).start()
class _P: pass
fake = _P(); fake.url = f"https://127.0.0.1:{port}"; fake.device_id = "aurix-test-01"; fake.token = "t"
fake.cert = {"spki_pin": "sha256//" + "A" * 43 + "="}
out, _ = run(fake, 1.5)
srv.close()
detail = [l for l in out.splitlines() if l.startswith("STATE Pharos unreachable")]
check("T3 TLS failure reason is reported in detail", any(":" in l.split("|", 1)[1] and len(l.split("|", 1)[1]) > len("SSL connect error") for l in detail), str(detail[:1]))

# S2f PEM pasted into a one-line field (newlines lost) still works
sim = fresh()
out, _ = run(sim, 1.5, trust=" ".join(sim.cert["pem"].split()))
check("S2f one-line PEM connects", "STATE Connected" in out, out[-300:])
sim.stop()

# S2e no pin + self-signed: refused as untrusted (never 'accept any')
sim = fresh()
out, _ = run(sim, 1.5, trust="")
check("S2e self-signed without pin refused", "certificate not trusted" in out and not sim.log, out[-200:])
sim.stop()

# S3 config round-trip with rejected and unsupported keys
sim = fresh()
sim.set_config({"site.zones": ["Lobby"], "site.timeZone": "America/Los_Angeles", "reporting.mode": "enrolled",
                "recognition.matchThreshold": 0.5, "recognition.minFaceSizePx": 10, "recognition.liveness": True,
                "relay.pulseMs": 3000, "future.knob": 7, "events.trackCloseSec": 5})
out, sd = run(sim, 2)
last = sim.statuses()[-1]
ac = last["appliedConfig"]
rej = {r["key"] for r in last.get("rejectedConfig", [])}
check("S3 applied revision read back", last["appliedConfigRevision"] == sim.config_rev, str(last["appliedConfigRevision"]))
check("S3 values applied", ac.get("recognition.matchThreshold") == 0.5 and ac.get("reporting.mode") == "enrolled"
      and ac.get("site.zones") == ["Lobby"] and ac.get("events.trackCloseSec") == 5, str(ac))
check("S3 out-of-range and liveness rejected", {"recognition.minFaceSizePx", "recognition.liveness"} <= rej, str(rej))
check("S3 unknown/unsupported keys listed", {"relay.pulseMs", "future.knob"} <= set(last.get("unsupportedConfig", [])))
check("S3 hook received config", "APPLY threshold=0.500" in out and "mode=enrolled" in out, out[-300:])
check("S3 requests match contract", not sim.violations, "; ".join(sim.violations[:3]))
sim.stop()
# S3b offline reboot keeps last config
sim2 = fresh(); sim2.revoked = False; sim2.stop()   # unreachable server
out, _ = run(sim2, 1, state_dir=sd)
check("S3b last config applied offline after restart", "APPLY threshold=0.500" in out, out[:300])

# S10 command executed exactly once; result reported; survives restart
sim = fresh()
sim.commands = [{"commandId": "c-78", "type": "resync", "args": {}},
                {"commandId": "c-79", "type": "teleport", "args": {}}]
out, sd = run(sim, 2.5)
check("S10 resync executed once", out.count("EXEC resync") == 1, out.count("EXEC resync"))
check("S10 results reported", sim.results_seen.get("c-78", {}).get("status") == "done"
      and sim.results_seen.get("c-79", {}).get("status") == "unsupported", str(sim.results_seen))
sim.results_seen.clear()
sim.commands = [{"commandId": "c-78", "type": "resync", "args": {}}]       # Pharos re-sends after restart
out, _ = run(sim, 1.5, state_dir=sd)
check("S10 not re-executed after restart", "EXEC resync" not in out and "c-78" in sim.results_seen, out[-200:])
sim.stop()

# S11 revocation: 410 -> stop all traffic (count requests while the client is still running)
sim = fresh()
mark = {}
def revoke_later(p):
    time.sleep(1.2); sim.revoked = True
    time.sleep(0.8); mark["after_410"] = len(sim.log)      # the 410'd request is already logged by now
    time.sleep(2.0); mark["end"] = len(sim.log)
out, _ = run(sim, 4.5, during=revoke_later)
check("S11 revoked shown", "STATE Revoked" in out, out[-300:])
check("S11 no requests in the 2 s after 410", mark["end"] == mark["after_410"], str(mark))
sim.stop()

# E1 503 with Retry-After keeps the session and honours the delay
sim = fresh()
def busy(p):
    time.sleep(1.0); sim.fault = {"route": "status", "status": 503, "retry_after": 2}
    time.sleep(1.0); n0 = len(sim.statuses()); time.sleep(1.0); busy.during = len(sim.statuses()) - n0
    sim.fault = None
out, _ = run(sim, 5, during=busy)
check("E1 503 Retry-After honoured (<=1 retry in 1 s)", busy.during <= 1, f"{busy.during} statuses in 1 s")
check("E1 recovers after busy", "STATE Connected" in out and len([l for l in out.splitlines() if l.startswith('STATE Connected')]) >= 1)
sim.stop()

# E2 200 with HTML body counts as failure
sim = fresh()
sim.fault = {"route": "hello", "status": 200, "body": "<html>proxy login</html>", "ctype": "text/html"}
out, _ = run(sim, 1.5)
check("E2 HTML 200 is not success", "STATE Connected" not in out and "without a JSON body" in out, out[-300:])
sim.stop()

# E3 426 protocol mismatch shown
sim = fresh()
sim.fault = {"route": "hello", "status": 426}
out, _ = run(sim, 1.5)
check("E3 protocol mismatch shown", "STATE Protocol mismatch" in out, out[-200:])
sim.stop()

failed = [n for n, ok in results if not ok]
print(f"\n{len(results) - len(failed)}/{len(results)} passed")
sys.exit(1 if failed else 0)
