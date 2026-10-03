#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <unistd.h>
#include "metrics.h"
#include "web.h"
#include "commission.h"
int main(int c, char **v) {
    (void)c;
    metrics_identity id = { "0.5.0", "ARTPEC-8", "DLPU", "dlpu", "mobilefacenet-128-int8-dlpu", "AXIS P3267-LV",
                            "B8A44FD5707E", "12.11.118", 10.0, 1920, 1080 };
    metrics_init(&id);
    metrics_recognition(0.45, 40, "watchlist");
    metrics_gallery(3, 2, 1, "Settings", 3 * 197);
    metrics_match_cost(25.5, 4096);
    metrics_sync(1890, 0, 3, 3, 0, 0);
    metrics_overlay(2.5, 4.0);
    metrics_pharos("Connected", "", 42);
    metrics_sample(5, "/tmp");
    for (int w = 0; w < 3; w++) {                 /* three 5-s windows of synthetic activity */
        for (int f = 0; f < 50; f++) {
            metrics_frame();
            metrics_stage_time(ST_CAPTURE, 45.0); metrics_stage_time(ST_DETECT, 55.0);
            if (f % 2 == 0) {
                metrics_stage_time(ST_ALIGN, 2.5); metrics_stage_time(ST_EMBED, 10.0); metrics_stage_time(ST_MATCH, 0.02);
                metrics_faces(1, 1, 1);
                metrics_match(f % 10 == 0 ? "Watch <b>Test</b>" : "Dana Ruiz", f % 10 == 0, 0.6f, 1);
            } else metrics_faces(1, 0, 0);
        }
        metrics_sample(5, "/tmp");
    }
    setenv("FCGI_SOCKET_NAME", getenv("FCGI_SOCK") ? getenv("FCGI_SOCK") : "/tmp/aurix-web.sock", 1);
    static web_commissioning wc = { "/tmp/aurix-webhost-commission", NULL, NULL, NULL };
    if (web_start(v[1], &wc)) return 1;
    sleep(30);
    return 0;
}
