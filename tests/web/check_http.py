import json, sys
for path in sys.argv[1:3]:
    raw = open(path, "rb").read()
    hdr, body = raw.split(b"\r\n\r\n", 1)
    h = dict(l.split(": ", 1) for l in hdr.decode().split("\r\n") if ": " in l)
    assert h["Status"].startswith("200"), (path, h)
    assert int(h["Content-Length"]) == len(body), (path, "content-length mismatch")
    assert h["Cache-Control"] == "no-store"
m = json.loads(open(sys.argv[1], "rb").read().split(b"\r\n\r\n", 1)[1])
st = {s["name"]: s for s in m["pipeline"]["stages"]}
assert abs(m["pipeline"]["fps"] - 10.0) < 0.01, m["pipeline"]["fps"]
assert abs(st["capture"]["perFrameMs"] - 45) < 0.1 and abs(st["embed"]["perFrameMs"] - 5.0) < 0.1
assert len(m["history"]) == 4 and m["recentMatches"]
c = m["capacity"]
assert c["nsPerEntry"] == 25.5 and c["byMatching"] == int(5e6 / 25.5) and c["galleryRamBytes"] == 591, c
assert c["estimate"] == min(x for x in (c["byMemory"], c["byStorage"], c["byMatching"]) if x), c
json.dump(m, open(sys.argv[3], "w"))
print("PASS FastCGI endpoints: headers, Content-Length, metrics values")
