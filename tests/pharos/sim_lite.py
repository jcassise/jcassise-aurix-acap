"""Minimal AURIX-Pharos stand-in for CI (step 1: /hello + status). Not the Pharos aurix-sim.

Every request body is validated against contract/schemas; a violation fails the test run.
Controlled in-process from the scenario runner (see run_scenarios.py)."""
import datetime, hashlib, json, os, ssl, threading, time, base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from jsonschema import Draft202012Validator, FormatChecker
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

HERE = os.path.dirname(os.path.abspath(__file__))
SCHEMAS = os.path.join(HERE, "..", "..", "contract", "schemas")


def schema(name):
    return Draft202012Validator(json.load(open(os.path.join(SCHEMAS, name + ".schema.json"))),
                                format_checker=FormatChecker())


def make_cert(dirpath, cn="pharos-sim"):
    key = ec.generate_private_key(ec.SECP256R1())
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)]))
            .issuer_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(days=1))
            .not_valid_after(now + datetime.timedelta(days=30)).sign(key, hashes.SHA256()))
    cp, kp = os.path.join(dirpath, cn + ".crt"), os.path.join(dirpath, cn + ".key")
    open(cp, "wb").write(cert.public_bytes(serialization.Encoding.PEM))
    open(kp, "wb").write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                           serialization.NoEncryption()))
    der = cert.public_bytes(serialization.Encoding.DER)
    spki = cert.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    return {"crt": cp, "key": kp, "pem": cert.public_bytes(serialization.Encoding.PEM).decode(),
            "pubkey_pem": cert.public_key().public_bytes(serialization.Encoding.PEM,
                                                         serialization.PublicFormat.SubjectPublicKeyInfo).decode(),
            "fingerprint": hashlib.sha256(der).hexdigest(),
            "spki_pin": "sha256//" + base64.b64encode(hashlib.sha256(spki).digest()).decode()}


def make_jpeg(seed, size=160):
    """A small, unique JPEG (content differs by seed) for sync tests."""
    import io, random
    from PIL import Image
    rnd = random.Random(seed)
    img = Image.new("RGB", (size, size), (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)))
    for _ in range(40):
        x, y = rnd.randrange(size), rnd.randrange(size)
        img.paste((rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)), (x, y, min(size, x + 12), min(size, y + 12)))
    b = io.BytesIO()
    img.save(b, "JPEG", quality=85)
    return b.getvalue()


class Sim:
    def __init__(self, workdir, device_id="aurix-test-01", token="tok-123", tls_min=None, tls_max=None):
        self.tls_min, self.tls_max = tls_min, tls_max
        self.device_id, self.token = device_id, token
        self.cert = make_cert(workdir)
        self.v = {n: schema(n) for n in ["hello-request", "hello-response", "status-request", "status-response",
                                         "people-page", "deletions-response", "sync-state", "policies-response",
                                         "event", "event-ack"]}
        self.events = {}            # eventId -> latest stored body
        self.event_puts = []        # (eventId, revision, applied)
        self.images = {}            # (eventId, kind) -> bytes
        self.image_early = 0        # images that arrived before their event (404 returned)
        self.offline = False        # refuse event/image uploads with 503
        self.people = {}            # personId -> record (without photo bytes)
        self.photos = {}            # (personId, photoId) -> bytes
        self.revision = 0
        self.policies, self.policies_revision = [], 0
        self.deletions = []         # (personId, revision)
        self.snapshots = {}         # id -> (snapshotRevision, [records])
        self.downloads = {}         # (personId, photoId) -> count
        self.inline_deletions = False
        self.sync_fault = None      # {"page": n, "status": 500} | {"page": n, "status": 409, "once": True} | {"truncate": True}
        self.photo_fault = {}       # (personId, photoId) -> 404 | "badbytes"
        self.page_size = 500
        self.lock = threading.Lock()
        self.log = []                 # (path, status, request json)
        self.violations = []
        self.config, self.config_rev = {}, 1
        self.commands = []            # re-sent every reply until a result is seen
        self.results_seen = {}
        self.fault = None             # dict: {"route": "status"|"hello", "status": 503, "retry_after": 2, "body": "...", "ctype": "..."}
        self.revoked = False
        self.status_interval = 500
        self.clock_skew_ms = 0

    def add_person(self, pid, name, watchlist="no_concern", photos=1, seed=None, policy_ids=None):
        with self.lock:
            self.revision += 1
            ph = []
            for k in range(photos):
                data = make_jpeg((seed if seed is not None else hash(pid)) * 10 + k)
                phid = f"ph-{pid}-{k}"
                self.photos[(pid, phid)] = data
                ph.append({"photoId": phid, "sha256": hashlib.sha256(data).hexdigest(), "kind": "face",
                           "updatedAt": int(time.time() * 1000)})
            self.people[pid] = {"personId": pid, "revision": self.revision, "externalRef": None, "displayName": name,
                                "firstName": None, "lastName": None, "personType": "employee", "watchlist": watchlist,
                                "policyIds": policy_ids or [], "validFrom": None, "validUntil": None, "photos": ph}

    def replace_photo(self, pid, k=0, seed=999):
        with self.lock:
            self.revision += 1
            data = make_jpeg(seed)
            rec = self.people[pid]
            phid = rec["photos"][k]["photoId"]
            self.photos[(pid, phid)] = data
            rec["photos"][k]["sha256"] = hashlib.sha256(data).hexdigest()
            rec["revision"] = self.revision

    def delete_person(self, pid):
        with self.lock:
            self.revision += 1
            self.people.pop(pid, None)
            self.deletions.append((pid, self.revision))

    def set_policies(self, policies):
        with self.lock:
            self.policies, self.policies_revision = policies, self.policies_revision + 1

    def set_config(self, cfg):
        with self.lock:
            self.config, self.config_rev = cfg, self.config_rev + 1

    def statuses(self):
        with self.lock:
            return [j for (p, s, j) in self.log if p.endswith("/status") and j]

    def start(self):
        sim = self

        class H(BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def reply(self, code, obj=None, raw=None, ctype="application/json", headers=None):
                body = raw if raw is not None else (json.dumps(obj).encode() if obj is not None else b"")
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(body)))
                for k, v in (headers or {}).items():
                    self.send_header(k, v)
                self.end_headers()
                self.wfile.write(body)

            def err(self, code, ecode, msg, headers=None):
                self.reply(code, {"error": {"code": ecode, "message": msg}}, headers=headers)

            def auth_ok(self):
                if sim.revoked:
                    self.err(410, "device_revoked", "revoked"); return False
                if self.headers.get("Authorization", "") != f"Bearer {sim.token}" or self.headers.get("X-AURIX-Device") != sim.device_id:
                    self.err(401, "unauthorized", "bad token"); return False
                return True

            def check(self, name, obj):
                for e in sim.v[name].iter_errors(obj):
                    sim.violations.append(f"sim {name}: {e.message}")

            def do_GET(self):
                from urllib.parse import urlparse, parse_qs
                u = urlparse(self.path)
                q = {k: v[0] for k, v in parse_qs(u.query).items()}
                with sim.lock:
                    sim.log.append((u.path, None, q))
                if not self.auth_ok():
                    return
                base = f"/aurix/v1/devices/{sim.device_id}/sync/"
                if u.path == base + "state":
                    r = {"revision": sim.revision, "policiesRevision": sim.policies_revision}
                    self.check("sync-state", r); return self.reply(200, r)
                if u.path == base + "policies":
                    r = {"revision": sim.policies_revision, "policies": sim.policies}
                    self.check("policies-response", r); return self.reply(200, r)
                if u.path == base + "deletions":
                    since = int(q.get("since", 0))
                    r = {"snapshotRevision": sim.revision,
                         "deleted": [{"personId": p, "revision": rv} for p, rv in sim.deletions if rv > since]}
                    self.check("deletions-response", r); return self.reply(200, r)
                if u.path == base + "people":
                    return self.people_page(q)
                if u.path.startswith("/aurix/v1/people/"):
                    parts = u.path.split("/")
                    if len(parts) == 7 and parts[5] == "photos":
                        key = (parts[4], parts[6])
                        with sim.lock:
                            sim.downloads[key] = sim.downloads.get(key, 0) + 1
                            data = sim.photos.get(key)
                            fault = sim.photo_fault.get(key)
                        if fault == 404 or data is None:
                            return self.err(404, "not_found", "photo")
                        if fault == "badbytes":
                            data = data[:-10] + b"0123456789"
                        return self.reply(200, raw=data, ctype="image/jpeg")
                self.err(404, "not_found", u.path)

            def people_page(self, q):
                import base64 as b64m, uuid
                limit = min(int(q.get("limit", 500)), sim.page_size)
                with sim.lock:
                    if "cursor" in q:
                        c = json.loads(b64m.urlsafe_b64decode(q["cursor"]))
                        snap = sim.snapshots.get(c["snap"])
                        if snap is None:
                            return self.err(409, "snapshot_expired", "expired")
                        sid, off, page_no, since = c["snap"], c["off"], c["page"], c["since"]
                    else:
                        since = int(q["since"]) if "since" in q else None
                        recs = sorted((r for r in sim.people.values() if since is None or r["revision"] > since),
                                      key=lambda r: r["personId"])
                        sid = uuid.uuid4().hex
                        sim.snapshots[sid] = (sim.revision, json.loads(json.dumps(recs)))
                        snap, off, page_no = sim.snapshots[sid], 0, 1
                    f = sim.sync_fault
                    if f and f.get("page") == page_no:
                        if f.get("once"):
                            sim.sync_fault = None
                        if f["status"] == 409:
                            sim.snapshots.pop(sid, None)
                            return self.err(409, "snapshot_expired", "expired")
                        return self.err(f["status"], "unavailable", "fault")
                    rev, recs = snap
                    chunk = recs[off:off + limit]
                    more = off + limit < len(recs)
                    nxt = b64m.urlsafe_b64encode(json.dumps({"snap": sid, "off": off + limit, "page": page_no + 1,
                                                             "since": since}).encode()).decode() if more else None
                    total = len(recs) + (1 if f and f.get("truncate") else 0)
                    page = {"snapshotRevision": rev, "people": chunk, "nextCursor": nxt, "total": total}
                    if sim.inline_deletions and since is not None and page_no == 1:
                        page["deleted"] = [{"personId": p, "revision": rv} for p, rv in sim.deletions if rv > since]
                self.check("people-page", {k: v for k, v in page.items() if k != "deleted"})
                self.reply(200, page)

            def do_PUT(self):
                n = int(self.headers.get("Content-Length", 0))
                raw = self.rfile.read(n)
                path = self.path
                with sim.lock:
                    sim.log.append((path, None, None))
                if not self.auth_ok():
                    return
                if sim.offline:
                    return self.err(503, "unavailable", "offline", headers={"Retry-After": "1"})
                parts = path.split("/")
                if len(parts) == 5 and parts[3] == "events":
                    eid = parts[4]
                    try:
                        body = json.loads(raw)
                    except Exception:
                        return self.err(400, "bad_request", "json")
                    errs = list(sim.v["event"].iter_errors(body))
                    for e in errs:
                        sim.violations.append(f"event: {e.message}")
                    if errs or body.get("eventId") != eid:
                        return self.err(400, "bad_request", "schema")
                    with sim.lock:
                        cur = sim.events.get(eid)
                        applied = cur is None or body["revision"] > cur["revision"]
                        if applied:
                            sim.events[eid] = body
                        stored = sim.events[eid]["revision"]
                        sim.event_puts.append((eid, body["revision"], applied))
                    ack = {"eventId": eid, "applied": applied, "storedRevision": stored}
                    self.check("event-ack", ack)
                    return self.reply(200, ack)
                if len(parts) == 7 and parts[3] == "events" and parts[5] == "images" and parts[6] in ("face", "scene"):
                    eid, kind = parts[4], parts[6]
                    with sim.lock:
                        known = eid in sim.events
                        if not known:
                            sim.image_early += 1
                    if not known:
                        return self.err(404, "event_unknown", "event first")
                    if len(raw) > 2 * 1024 * 1024:
                        return self.err(413, "image_too_large", "2 MiB")
                    if raw[:2] != b"\xff\xd8":
                        sim.violations.append(f"image {kind} is not a JPEG")
                    with sim.lock:
                        sim.images[(eid, kind)] = raw
                    self.send_response(204); self.send_header("Content-Length", "0"); self.end_headers()
                    return
                self.err(404, "not_found", path)

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                raw = self.rfile.read(n)
                try:
                    req = json.loads(raw)
                except Exception:
                    req = None
                path = self.path
                route = "hello" if path == "/aurix/v1/hello" else \
                        "status" if path == f"/aurix/v1/devices/{sim.device_id}/status" else None
                with sim.lock:
                    sim.log.append((path, None, req))
                if route is None:
                    return self.err(404, "not_found", path)
                schema_name = route + "-request" if route == "hello" else "status-request"
                for e in sim.v[schema_name].iter_errors(req):
                    sim.violations.append(f"{route}: {e.message}")
                if sim.revoked:
                    return self.err(410, "device_revoked", "revoked")
                auth = self.headers.get("Authorization", "")
                if auth != f"Bearer {sim.token}" or self.headers.get("X-AURIX-Device") != sim.device_id:
                    return self.err(401, "unauthorized", "bad token")
                f = sim.fault
                if f and f.get("route") == route:
                    if "body" in f:
                        return self.reply(f.get("status", 200), raw=f["body"].encode(), ctype=f.get("ctype", "text/html"))
                    hdr = {"Retry-After": str(f["retry_after"])} if "retry_after" in f else None
                    return self.err(f["status"], "busy", "fault", headers=hdr)
                if route == "hello":
                    resp = {"accepted": True, "protocol": 1, "serverTime": int(time.time() * 1000) + sim.clock_skew_ms,
                            "statusIntervalMs": sim.status_interval,
                            "endpoints": {"base": f"https://127.0.0.1:{sim.port}/aurix/v1"}}
                    return self.reply(200, resp)
                with sim.lock:
                    for r in (req or {}).get("commandResults", []):
                        sim.results_seen[r["commandId"]] = r
                    sim.commands = [c for c in sim.commands if c["commandId"] not in sim.results_seen]
                    resp = {"configRevision": sim.config_rev, "config": sim.config, "commands": list(sim.commands),
                            "nextStatusInMs": sim.status_interval, "serverTime": int(time.time() * 1000) + sim.clock_skew_ms}
                for e in sim.v["status-response"].iter_errors(resp):
                    sim.violations.append(f"sim status-response: {e.message}")
                self.reply(200, resp)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self.cert["crt"], self.cert["key"])
        if self.tls_min: ctx.minimum_version = self.tls_min
        if self.tls_max: ctx.maximum_version = self.tls_max
        self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = f"https://127.0.0.1:{self.port}"
        return self

    def stop(self):
        self.httpd.shutdown()
