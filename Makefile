# Copyright (c) 2026 Praveen Vaddadi <thynktank@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

CC = gcc
CFLAGS = -O3 -pthread -Wall -I.

# The example binaries (placed in examples/ folder)
BINARIES = examples/chat_server \
           examples/ping_client \
           examples/firehose \
           examples/telemetry_sink

all: $(BINARIES)

# --- Example Applications ---

examples/chat_server: examples/chat_server.c vibe.h
	$(CC) $(CFLAGS) examples/chat_server.c -o examples/chat_server

examples/ping_client: examples/ping_client.c vibe.h
	$(CC) $(CFLAGS) examples/ping_client.c -o examples/ping_client

examples/firehose: examples/firehose.c vibe.h
	$(CC) $(CFLAGS) examples/firehose.c -o examples/firehose

examples/telemetry_sink: examples/telemetry_sink.c vibe.h
	$(CC) $(CFLAGS) examples/telemetry_sink.c -o examples/telemetry_sink

clean:
	rm -f $(BINARIES)
