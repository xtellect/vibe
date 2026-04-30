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

// chat_server.c
#define VIBE_IMPLEMENTATION
#include "vibe.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile bool running = true;
void sig_handler(int s) { (void)s; running = false; }

#define MAX_USERS 1024

int main(int argc, char** argv) {
    if (argc < 2) return printf("Usage: %s <port>\n", argv[0]);

    signal(SIGINT, sig_handler);

    // 1. Start the engine
    vibe_hub_t* hub = vibe_hub_start();

    // 2. Listen for users
    char addr[64]; snprintf(addr, 64, "0.0.0.0:%s", argv[1]);
    if (!vibe_listen(hub, addr)) return -1;
    printf("[Chat] Room open on %s\n", addr);

    vibe_conn_t* users[MAX_USERS];
    int user_count = 0;

    while (running) {
        // 3. Poll the inbox (Non-blocking)
        vibe_msg_t* batch = vibe_poll(hub);

        if (batch) {
            vibe_for_each(msg, batch) {
                switch (msg->type) {
                    case VIBE_EVENT_CONNECTED:
                        if (user_count < MAX_USERS) {
                            users[user_count++] = msg->conn;
                            printf("[Chat] User joined (%d total)\n", user_count);
                        }
                        break;

                    case VIBE_EVENT_DATA:
                        // 4. Single-Copy Broadcast to all users
                        // The engine handles the reference counting!
                        vibe_multicast(users, user_count, msg->data, msg->len);
                        break;

                    case VIBE_EVENT_DISCONNECTED:
                        // Real app would remove user from array here (O(n))
                        printf("[Chat] User left\n");
                        break;
                }
            }
            // 5. Hygiene
            vibe_free(batch);
        } else {
            // No events? Sleep briefly to save CPU
            usleep(500);
        }
    }

    vibe_hub_stop(hub);
    return 0;
}
