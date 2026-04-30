/*
 * Copyright (c) 2026 Praveen Vaddadi <thynktank@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// telemetry_sink.c
#define VIBE_IMPLEMENTATION
#include "vibe.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) return printf("Usage: %s <port>\n", argv[0]);

    vibe_hub_t* hub = vibe_hub_start();

    // Listen on port
    char addr[64]; snprintf(addr, 64, "0.0.0.0:%s", argv[1]);
    vibe_listen(hub, addr);

    printf("[Sink] Collecting metrics on %s...\n", addr);

    uint64_t msgs = 0;
    uint64_t bytes = 0;
    time_t last_log = time(NULL);

    while (1) {
        vibe_msg_t* batch = vibe_poll(hub);

        if (batch) {
            // Iterate entire batch
            vibe_for_each(msg, batch) {
                if (msg->type == VIBE_EVENT_DATA) {
                    msgs++;
                    bytes += msg->len;
                }
            }
            vibe_free(batch);
        } else {
            // Idle strategy: yield to OS
            usleep(10);
        }

        // 1-second interval logging
        time_t now = time(NULL);
        if (now > last_log) {
            double mb = (double)bytes / (1024*1024);
            printf("[Sink] %lu msgs/sec | %.2f MB/sec\n", msgs, mb);

            msgs = 0;
            bytes = 0;
            last_log = now;
        }
    }

    vibe_hub_stop(hub);
    return 0;
}