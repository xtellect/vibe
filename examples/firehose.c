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

// firehose.c
#define VIBE_IMPLEMENTATION
#include "vibe.h"
#include <stdio.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char** argv) {
    if (argc < 2) return printf("Usage: %s <port>\n", argv[0]);

    vibe_hub_t* hub = vibe_hub_start();
    char addr[64]; snprintf(addr, 64, "127.0.0.1:%s", argv[1]);

    vibe_dial(hub, addr);

    vibe_conn_t* conn = NULL;
    // Simple connection wait
    while(!conn) {
        vibe_msg_t* b = vibe_poll(hub);
        if(b) {
            vibe_for_each(msg, b) {
                if(msg->type == VIBE_EVENT_CONNECTED) conn = msg->conn;
            }
            vibe_free(b);
        }
        usleep(1000); // Sleep while waiting to connect
    }

    printf("[Firehose] Flooding %s (Throttled)...\n", addr);

    char junk[1024]; // 1KB Payload
    long total_sent = 0;

    while (1) {
    // The library now protects us.
    // If vibe_send_msg returns false, it means "Network is busy, try later".
    bool sent = vibe_send_msg(conn, junk, sizeof(junk));

    if (sent) {
        total_sent++;
    } else {
        // Backpressure active! Yield CPU to let the IO thread drain the queue.
        usleep(100);
    }

    // Always poll to process ACKs/Events
    vibe_msg_t* b = vibe_poll(hub);
    if (b) vibe_free(b);
}

    vibe_hub_stop(hub);
    return 0;
}