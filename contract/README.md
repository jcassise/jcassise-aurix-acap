# AURIX–Pharos contract, v1 (draft 1)

The machine-readable source of truth named in §10 of
`aurix-pharos-integration.md`. **Owner: Pharos.** Either side proposes changes;
Pharos accepts them and bumps `info.version`.

```
aurix-pharos-v1.yaml        OpenAPI 3.1 — every endpoint, header, status code and schema
schemas/*.schema.json       standalone JSON Schema 2020-12, generated from the YAML (do not edit)
examples/valid/*.json       payloads that MUST validate     (file name: <schema>__<case>.json)
examples/invalid/*.json     payloads that MUST be rejected
tools/build_and_check.py    validates the YAML, regenerates schemas/, checks every example
```

```
pip install openapi-spec-validator jsonschema pyyaml
python tools/build_and_check.py        # exit 0 = contract consistent
```

## How each side uses it

- **AURIX:** validate every request it builds against `schemas/`. Feed
  `examples/valid` into its parsers as fixtures. Check that it never builds
  anything resembling `examples/invalid`.
- **Pharos:** the `/aurix/v1` routes validate requests and responses against
  the same files. `aurix-sim` is generated from or tested against the YAML.
- **Adding a case:** drop a JSON file into `examples/valid` or
  `examples/invalid` and re-run the tool. Any bug found at a boundary between
  the two sides should end up as an example here.

## Proposed in draft 1, not yet in the integration document

These are marked `x-status: proposed` in the YAML. Pharos accepts or rejects
each one before 1.0.0.

| Item | Why |
|---|---|
| `Person.validFrom` / `validUntil` are epoch ms, not ISO 8601 | Every other time in the protocol is epoch ms. One format means one parser and no time-zone ambiguity |
| `site.timeZone` (IANA) config key | Schedules and `excludedDates` are camera-local. Without this, a camera with the wrong time zone grants access at the wrong hours, and nothing reports it |
| Schedule windows are same-day; a window crossing midnight is sent as two entries | Removes the ambiguity of 22:00–06:00 |
| `HelloRequest.platform.ip` | Pharos joins the device to the camera row by address |
| `status.sync.inProgress`, `status.queue.dropped` | Lets Pharos show "full sync running" and "queue overflowed, events lost" |
| `Event.unrecognizable`, `Event.relay`, `Event.device`, `Event.commandId` | The document defines `relay`, `device` and `snapshot` events but not their fields |
| `PUT /events/{id}` returns `EventAck { applied, storedRevision }` | Makes a stale PUT visibly harmless, so the device doesn't retry it |
| Image PUT before its event → `404 event_unknown`; images over 2 MiB → `413` | The document didn't say what happens in these cases |
| `limit` on `sync/people`: 1–1000, default 500 | |
| `X-AURIX-Device` must equal the path `deviceId`, otherwise 401 | |

## Conventions enforced by the schemas

- `watchlist` is exactly `no_concern` · `concern` · `threat`.
- `eventId` is a UUID string, used raw in the path (no base64).
- Scores and thresholds are 0–1, never percentages.
- An `access` event must carry a decision. A `relay` or `device` event must
  carry its block. An event that has a `person` cannot also be `stranger: true`.
- Unknown JSON properties are allowed everywhere, and unknown config keys and
  command types are valid. Forward compatibility depends on it: report them,
  don't reject them.
