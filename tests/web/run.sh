#!/bin/sh
# Dashboard end-to-end: real metrics + real FastCGI endpoint (cgi-fcgi client), then the page in jsdom.
#   apt: libfcgi-dev libfcgi-bin libjansson-dev    npm: jsdom
set -e
cd "$(dirname "$0")"
SRC=../../app/src
HTML=../../app/web/dashboard.html
cc -std=gnu11 -Wall -Wextra -I$SRC webhost.c $SRC/metrics.c $SRC/sysinfo.c $SRC/capacity.c $SRC/web.c \
   $(pkg-config --cflags --libs jansson fcgi) -lpthread -o webhost
SOCK=$(mktemp -u /tmp/aurix-web-XXXX.sock)
FCGI_SOCK="$SOCK" ./webhost "$HTML" & PID=$!
trap 'kill $PID 2>/dev/null; rm -f "$SOCK" data.http page.http metrics.json webhost' EXIT
for i in 1 2 3 4 5 6 7 8 9 10; do [ -S "$SOCK" ] && break; sleep 0.3; done
REQUEST_METHOD=GET REQUEST_URI='/local/aurix/aurix.cgi?data' cgi-fcgi -bind -connect "$SOCK" > data.http
REQUEST_METHOD=GET REQUEST_URI='/local/aurix/aurix.cgi' cgi-fcgi -bind -connect "$SOCK" > page.http
python3 check_http.py data.http page.http metrics.json
node dashboard.test.js "$HTML" metrics.json
