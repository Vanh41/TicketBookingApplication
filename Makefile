CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
CPPFLAGS += -Iinclude
BIN_DIR ?= bin
PYTHON ?= python3

BREW_PREFIX := $(if $(wildcard /opt/homebrew/include/json-c/json.h),/opt/homebrew)
JSON_CFLAGS ?= $(shell pkg-config --cflags json-c 2>/dev/null || $(if $(BREW_PREFIX),echo -I$(BREW_PREFIX)/include))
JSON_LIBS ?= $(shell pkg-config --libs json-c 2>/dev/null || echo $(if $(BREW_PREFIX),-L$(BREW_PREFIX)/lib) -ljson-c)
CRYPTO_CFLAGS ?= $(shell pkg-config --cflags libcrypto 2>/dev/null || $(if $(BREW_PREFIX),echo -I$(BREW_PREFIX)/opt/openssl@3/include))
CRYPTO_LIBS ?= $(shell pkg-config --libs libcrypto 2>/dev/null || echo $(if $(BREW_PREFIX),-L$(BREW_PREFIX)/opt/openssl@3/lib) -lcrypto)

NETWORK = src/common/network.c src/common/ticket_protocol.c
SERVER = src/server/main.c src/server/request_handler.c \
         src/server/ticket_db.c src/server/ticket_auth.c \
         src/server/ticket_catalog.c src/server/ticket_booking.c src/common/utils.c $(NETWORK)
CLIENT = src/client/main.c src/client/ticket_cli.c $(NETWORK)
HEADERS = $(wildcard include/*.h)

.PHONY: all server client clean run_server run_client test
all: server client
server: $(BIN_DIR)/server
client: $(BIN_DIR)/client

$(BIN_DIR):
	mkdir -p "$@"

$(BIN_DIR)/server: $(SERVER) $(HEADERS) src/server/ticket_server.h | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(JSON_CFLAGS) $(CRYPTO_CFLAGS) -pthread -o "$@" $(SERVER) $(JSON_LIBS) $(CRYPTO_LIBS) -lsqlite3

$(BIN_DIR)/client: $(CLIENT) $(HEADERS) | $(BIN_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(JSON_CFLAGS) -o "$@" $(CLIENT) $(JSON_LIBS)

test: all
	TICKET_TEST_BIN="$(abspath $(BIN_DIR))" $(PYTHON) tests/ticket_booking_test.py

run_server: server
	"$(abspath $(BIN_DIR))/server"

run_client: client
	"$(abspath $(BIN_DIR))/client" 127.0.0.1 3636

clean:
	rm -rf "$(BIN_DIR)"
