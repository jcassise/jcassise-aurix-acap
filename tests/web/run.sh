#!/bin/sh
# Dashboard end-to-end: real metrics + real FastCGI endpoint (cgi-fcgi client), then the page in jsdom.
#   apt: libfcgi-dev libfcgi-bin libjansson-dev    npm: jsdom
set -e
cd "$(dirname "$0")"
SRC=../../app/src
HTML=../../app/web/dashboard.html
cc -std=gnu11 -Wall -Wextra -I$SRC webhost.c $SRC/metrics.c $SRC/sysinfo.c $SRC/capacity.c $SRC/web.c $SRC/commission.c $SRC/pharos_http.c $SRC/event_queue.c $SRC/settings.c $SRC/pharos_config.c \
   $(pkg-config --cflags --libs jansson fcgi openssl libcurl) -lpthread -o webhost
SOCK=$(mktemp -u /tmp/aurix-web-XXXX.sock)
FCGI_SOCK="$SOCK" ./webhost "$HTML" & PID=$!
trap 'kill $PID 2>/dev/null; rm -f "$SOCK" data.http page.http metrics.json webhost; rm -rf /tmp/aurix-webhost-commission' EXIT
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.3; done
MODE=$(stat -c %a "$SOCK")
[ "$MODE" = "777" ] || { echo "FAIL socket mode $MODE: the camera's web server (another user) could not connect -> 503"; exit 1; }
echo "PASS socket is connectable by the camera's web server (mode $MODE)"
REQUEST_METHOD=GET REQUEST_URI='/local/aurix/aurix.cgi?data' cgi-fcgi -bind -connect "$SOCK" > data.http
REQUEST_METHOD=GET REQUEST_URI='/local/aurix/aurix.cgi' cgi-fcgi -bind -connect "$SOCK" > page.http
python3 check_http.py data.http page.http metrics.json
# commissioning over real FastCGI: refused without the header, accepted with it, token never echoed
rm -rf /tmp/aurix-webhost-commission
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -keyout /tmp/aurix-tk.pem -out /tmp/aurix-tc.pem -days 30 -subj "/CN=pharos.site.local" 2>/dev/null
BODY=$(python3 -c "import json;print(json.dumps({'url':'https://pharos.site.local','deviceId':'aurix-lobby-01','token':'secret-token-123','cert':open('/tmp/aurix-tc.pem').read()}))")
LEN=$(printf '%s' "$BODY" | wc -c)
printf '%s' "$BODY" | REQUEST_METHOD=POST REQUEST_URI='/local/aurix/aurix.cgi?commission' CONTENT_LENGTH=$LEN CONTENT_TYPE=application/json \
  cgi-fcgi -bind -connect "$SOCK" > post_nocsrf.http
printf '%s' "$BODY" | REQUEST_METHOD=POST REQUEST_URI='/local/aurix/aurix.cgi?commission' CONTENT_LENGTH=$LEN CONTENT_TYPE=application/json \
  HTTP_X_AURIX_REQUEST=1 cgi-fcgi -bind -connect "$SOCK" > post_ok.http
python3 - <<'PY'
import json
a=open('post_nocsrf.http','rb').read(); b=open('post_ok.http','rb').read()
assert b'Status: 403' in a, a[:80]
hdr, body = b.split(b'\r\n\r\n', 1)
assert b'Status: 200' in hdr and b'secret-token-123' not in b, hdr
v = json.loads(body); assert v['commissioned'] and v['tokenSet'] and v['cert']['subject'] == 'pharos.site.local', v
print("PASS commissioning over FastCGI (403 without header, 200 with it, token never echoed)")
PY
rm -f post_nocsrf.http post_ok.http
# settings over real FastCGI: read, refuse a Pharos-managed key, save a local one
REQUEST_METHOD=GET REQUEST_URI='/local/aurix/aurix.cgi?settings' cgi-fcgi -bind -connect "$SOCK" > set_get.http
B1='{"recognition.matchThreshold":0.6}'; B2='{"tracking.sameFace":0.42}'
printf '%s' "$B1" | REQUEST_METHOD=POST REQUEST_URI='/local/aurix/aurix.cgi?settings' CONTENT_LENGTH=${#B1} HTTP_X_AURIX_REQUEST=1 cgi-fcgi -bind -connect "$SOCK" > set_managed.http
printf '%s' "$B2" | REQUEST_METHOD=POST REQUEST_URI='/local/aurix/aurix.cgi?settings' CONTENT_LENGTH=${#B2} cgi-fcgi -bind -connect "$SOCK" > set_nocsrf.http
printf '%s' "$B2" | REQUEST_METHOD=POST REQUEST_URI='/local/aurix/aurix.cgi?settings' CONTENT_LENGTH=${#B2} HTTP_X_AURIX_REQUEST=1 cgi-fcgi -bind -connect "$SOCK" > set_ok.http
python3 - <<'PY'
import json
def body(f):
    h, b = open(f, 'rb').read().split(b'\r\n\r\n', 1); return h, json.loads(b)
h, d = body('set_get.http'); assert b'Status: 200' in h and len(d['groups']) == 4, h
h, d = body('set_managed.http'); assert b'Status: 422' in h and 'Pharos' in d['error']['message'], d
h, d = body('set_nocsrf.http'); assert b'Status: 403' in h, h
h, d = body('set_ok.http'); assert b'Status: 200' in h
v = [s for g in d['groups'] for s in g['settings'] if s['key'] == 'tracking.sameFace'][0]
assert v['value'] == 0.42 and v['source'] == 'local', v
print("PASS settings over FastCGI (read, Pharos key refused, no header refused, local change saved)")
PY
rm -f set_get.http set_managed.http set_nocsrf.http set_ok.http
node dashboard.test.js "$HTML" metrics.json
node commission.test.js "$HTML" metrics.json /tmp/aurix-tc.pem
node settings.test.js "$HTML" metrics.json
cc -std=gnu11 -Wall -Wextra -I$SRC test_settings.c $SRC/settings.c $SRC/pharos_config.c $(pkg-config --cflags --libs jansson) -o test_settings && ./test_settings | tail -1 && rm -f test_settings
cc -std=gnu11 -Wall -Wextra -I$SRC test_commission.c $SRC/web.c $SRC/commission.c $SRC/metrics.c $SRC/sysinfo.c $SRC/capacity.c $SRC/pharos_http.c $SRC/event_queue.c \
   $(pkg-config --cflags --libs jansson fcgi openssl libcurl) -lpthread -o test_commission && ./test_commission /tmp/aurix-tc.pem && rm -f test_commission
