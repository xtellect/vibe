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

// ping_client.c
#define VIBE_IMPLEMENTATION
#include "vibe.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

uint64_t nanos() {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1e9 + ts.tv_nsec;
}

int main(int argc, char** argv) {
    if (argc < 2) return printf("Usage: %s <port>\n", argv[0]);

    vibe_hub_t* hub = vibe_hub_start();
    char addr[64]; snprintf(addr, 64, "127.0.0.1:%s", argv[1]);

    // 1. Dial (Async)
    printf("[Ping] Dialing %s...\n", addr);
    vibe_dial(hub, addr);

    vibe_conn_t* server = NULL;

    // 2. Handshake Loop
    while (!server) {
        vibe_msg_t* batch = vibe_poll(hub);
        if (batch) {
            vibe_for_each(msg, batch) {
                if (msg->type == VIBE_EVENT_CONNECTED) {
                    server = msg->conn;
                    printf("[Ping] Connection Established!\n");
                }
            }
            vibe_free(batch);
        }
    }

    // 3. Benchmark
    char payload[] = "PING";
    for (int i = 0; i < 5; i++) {
        uint64_t start = nanos();

        vibe_send_msg(server, payload, 4);

        // Spin-wait for strict latency measurement
        bool pong_recvd = false;
        while (!pong_recvd) {
            vibe_msg_t* batch = vibe_poll(hub);
            if (batch) {
                vibe_for_each(msg, batch) {
                    if (msg->type == VIBE_EVENT_DATA) pong_recvd = true;
                }
                vibe_free(batch);
            }
        }

        double latency_us = (nanos() - start) / 1000.0;
        printf("Seq %d: %.2f us\n", i, latency_us);
        usleep(100000); // Sleep 100ms
    }

    vibe_hub_stop(hub);
    return 0;
}