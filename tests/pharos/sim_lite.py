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


class Sim:
    def __init__(self, workdir, device_id="aurix-test-01", token="tok-123"):
        self.device_id, self.token = device_id, token
        self.cert = make_cert(workdir)
        self.v = {n: schema(n) for n in ["hello-request", "hello-response", "status-request", "status-response"]}
        self.lock = threading.Lock()
        self.log = []                 # (path, status, request json)
        self.violations = []
        self.config, self.config_rev = {}, 1
        self.commands = []            # re-sent every reply until a result is seen
        self.results_seen = {}
        self.fault = None             # dict: {"route": "status"|"hello", "status": 503, "retry_after": 2, "body": "...", "ctype": "..."}
        self.revoked = False
        self.status_interval = 500

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
                    resp = {"accepted": True, "protocol": 1, "serverTime": int(time.time() * 1000),
                            "statusIntervalMs": sim.status_interval,
                            "endpoints": {"base": f"https://127.0.0.1:{sim.port}/aurix/v1"}}
                    return self.reply(200, resp)
                with sim.lock:
                    for r in (req or {}).get("commandResults", []):
                        sim.results_seen[r["commandId"]] = r
                    sim.commands = [c for c in sim.commands if c["commandId"] not in sim.results_seen]
                    resp = {"configRevision": sim.config_rev, "config": sim.config, "commands": list(sim.commands),
                            "nextStatusInMs": sim.status_interval}
                for e in sim.v["status-response"].iter_errors(resp):
                    sim.violations.append(f"sim status-response: {e.message}")
                self.reply(200, resp)

        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.httpd.server_address[1]
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self.cert["crt"], self.cert["key"])
        self.httpd.socket = ctx.wrap_socket(self.httpd.socket, server_side=True)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        self.url = f"https://127.0.0.1:{self.port}"
        return self

    def stop(self):
        self.httpd.shutdown()
