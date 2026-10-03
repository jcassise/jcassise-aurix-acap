"""Validate the OpenAPI file, export standalone JSON Schemas, check example payloads.

Usage: python tools/build_and_check.py      (from the contract root)
Exit 0 only if the spec is valid, every valid/ example passes, every invalid/ example fails.
"""
import json, sys, copy, pathlib, yaml
from openapi_spec_validator import validate
from jsonschema import Draft202012Validator, FormatChecker

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = yaml.safe_load((ROOT / "aurix-pharos-v1.yaml").read_text())
validate(spec)
print("OpenAPI 3.1: valid")

all_schemas = spec["components"]["schemas"]
EXPORT = {  # file name -> component schema
    "hello-request": "HelloRequest", "hello-response": "HelloResponse",
    "status-request": "StatusRequest", "status-response": "StatusResponse",
    "config": "Config", "command": "Command", "sync-state": "SyncState",
    "policies-response": "PoliciesResponse", "policy": "Policy",
    "people-page": "PeoplePage", "person": "Person",
    "deletions-response": "DeletionsResponse", "event": "Event",
    "event-ack": "EventAck", "error": "Error",
}

def rewrite(node):
    if isinstance(node, dict):
        out = {}
        for k, v in node.items():
            if k == "$ref" and v.startswith("#/components/schemas/"):
                out[k] = "#/$defs/" + v.rsplit("/", 1)[1]
            elif k.startswith("x-") or k == "contentMediaType":
                out[k] = v
            else:
                out[k] = rewrite(v)
        return out
    if isinstance(node, list):
        return [rewrite(x) for x in node]
    return node

defs = rewrite(copy.deepcopy(all_schemas))
validators = {}
for fname, comp in EXPORT.items():
    doc = {
        "$schema": "https://json-schema.org/draft/2020-12/schema",
        "$id": f"https://pharos/aurix/v1/schemas/{fname}.schema.json",
        "title": comp,
        "$ref": f"#/$defs/{comp}",
        "$defs": defs,
    }
    Draft202012Validator.check_schema(doc)
    (ROOT / "schemas" / f"{fname}.schema.json").write_text(json.dumps(doc, indent=2) + "\n")
    validators[fname] = Draft202012Validator(doc, format_checker=FormatChecker())
print(f"Exported {len(EXPORT)} JSON Schemas")

failures = 0
for kind in ("valid", "invalid"):
    for f in sorted((ROOT / "examples" / kind).glob("*.json")):
        schema_name = f.name.split("__")[0]
        errs = list(validators[schema_name].iter_errors(json.loads(f.read_text())))
        ok = (not errs) if kind == "valid" else bool(errs)
        failures += not ok
        note = "" if kind == "valid" or not errs else f"  (rejected: {errs[0].message[:70]})"
        print(f"{'PASS' if ok else 'FAIL'}  {kind}/{f.name}{note}")
        if not ok and errs:
            for e in errs[:3]: print("      ", list(e.path), e.message[:120])
sys.exit(1 if failures else 0)
