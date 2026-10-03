#!/bin/sh
# Dashboard end-to-end: real metrics + real FastCGI endpoint (cgi-fcgi client), then the page in jsdom.
#   apt: libfcgi-dev libfcgi-bin libjansson-dev    npm: jsdom
set -e
cd "$(dirname "$0")"
SRC=../../app/src
HTML=../../app/web/dashboard.html
cc -std=gnu11 -Wall -Wextra -I$SRC webhost.c $SRC/metrics.c $SRC/sysinfo.c $SRC/capacity.c $SRC/web.c $SRC/commission.c $SRC/pharos_http.c \
   $(pkg-config --cflags --libs jansson fcgi openssl libcurl) -lpthread -o webhost
SOCK=$(mktemp -u /tmp/aurix-web-XXXX.sock)
FCGI_SOCK="$SOCK" ./webhost "$HTML" & PID=$!
trap 'kill $PID 2>/dev/null; rm -f "$SOCK" data.http page.http metrics.json webhost; rm -rf /tmp/aurix-webhost-commission' EXIT
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.3; done
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
node dashboard.test.js "$HTML" metrics.json
node commission.test.js "$HTML" metrics.json /tmp/aurix-tc.pem
cc -std=gnu11 -Wall -Wextra -I$SRC test_commission.c $SRC/web.c $SRC/commission.c $SRC/metrics.c $SRC/sysinfo.c $SRC/capacity.c $SRC/pharos_http.c \
   $(pkg-config --cflags --libs jansson fcgi openssl libcurl) -lpthread -o test_commission && ./test_commission /tmp/aurix-tc.pem && rm -f test_commission
