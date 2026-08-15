# Distributed Shared Memory (DSM) System
# Build configuration

CC       := gcc
CFLAGS   := -O3 -march=native -mtune=native -flto -fomit-frame-pointer
CFLAGS   += -Wall -Wextra -std=gnu11
CFLAGS   += -Iinclude
CFLAGS   += -MMD -MP
# -Wl,-z,now: Eager binding eliminates lazy symbol resolution overhead (~25% client CPU)
LDFLAGS  := -pthread -flto -Wl,-z,now

# Source files
SRCDIR   := src
BUILDDIR := build

# Common objects (linked into both binaries)
COMMON_OBJ := $(BUILDDIR)/dsm_common.o $(BUILDDIR)/dsm_config.o $(BUILDDIR)/dsm_paging.o

# Cluster objects (server only)
CLUSTER_OBJ := $(BUILDDIR)/dsm_cluster.o $(BUILDDIR)/dsm_gossip.o

# Unit test sources
TEST_SRC := tests/unit_paging.c
TEST_BIN := $(BUILDDIR)/unit_paging
GOSSIP_TEST_SRC := tests/unit_gossip.c
GOSSIP_TEST_BIN := $(BUILDDIR)/unit_gossip

# Targets
SERVER   := dsm-server
CLIENT   := dsm-client
LIBRARY  := libdsm.so

# Install prefix
PREFIX  ?= /usr/local

# Docker-friendly flags (no -march=native for portability)
DOCKER_CFLAGS := -O3 -flto -fomit-frame-pointer -Wall -Wextra -std=gnu11 -Iinclude -MMD -MP

.PHONY: all server client lib python debug perf test unit python-test \
        cluster-test docker docker-build docker-run cluster-docker \
        clean install help

all: $(SERVER) $(CLIENT)

lib: $(LIBRARY)

python: $(LIBRARY)
	@echo "Python bindings ready. Usage:"
	@echo "  export LD_LIBRARY_PATH=\$$(pwd):\$$LD_LIBRARY_PATH"
	@echo "  python3 -c 'import python.dsm as dsm; print(dsm.__doc__)'"

# Create build directory
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

# PIC objects for the shared library
$(BUILDDIR)/%_pic.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

# Regular objects; header deps come from -MMD -MP .d files
$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Shared library for Python bindings
$(LIBRARY): $(BUILDDIR)/dsm_common_pic.o $(BUILDDIR)/dsm_paging_pic.o $(BUILDDIR)/dsm_config_pic.o
	$(CC) -shared $(LDFLAGS) -o $@ $^
	@echo "Built $(LIBRARY) for Python bindings"

# Link server (includes cluster support)
$(SERVER): $(BUILDDIR)/dsm_server.o $(COMMON_OBJ) $(CLUSTER_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Built $(SERVER)"

# Link client
$(CLIENT): $(BUILDDIR)/dsm_client.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Built $(CLIENT)"

# Convenience targets
server: $(SERVER)
client: $(CLIENT)

# Docker build (portable flags)
docker: CFLAGS := $(DOCKER_CFLAGS)
docker: clean all
	@echo "Built for Docker with portable flags"

# Debug build
debug: CFLAGS := -O0 -g -Wall -Wextra -std=gnu11 -Iinclude -MMD -MP -fsanitize=address,undefined
debug: LDFLAGS := -pthread -fsanitize=address,undefined
debug: clean all
	@echo "Built with debug symbols and sanitizers"

# Performance analysis
perf: $(CLIENT) $(SERVER)
	@echo "Running performance analysis..."
	@./$(SERVER) & SERVER_PID=$$!; \
	sleep 1; \
	perf stat -d -d -d ./$(CLIENT) -H localhost -v; \
	kill $$SERVER_PID 2>/dev/null || true

clean:
	rm -rf $(BUILDDIR) $(SERVER) $(CLIENT) $(LIBRARY)

# Test: end-to-end verify (write, evict, writeback, refetch), then unit tests
test: $(SERVER) $(CLIENT)
	@echo "Starting verify test..."
	@./$(SERVER) -n 1024 & SERVER_PID=$$!; \
	sleep 1; \
	timeout 60 ./$(CLIENT) -H localhost --verify; RC=$$?; \
	kill $$SERVER_PID 2>/dev/null || true; wait $$SERVER_PID 2>/dev/null || true; \
	exit $$RC
	@$(MAKE) unit
	@echo "Test complete"

# Unit tests: pager tests (in-memory mode, no server needed)
$(TEST_BIN): $(TEST_SRC) $(COMMON_OBJ) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(COMMON_OBJ) $(LDFLAGS)

# Unit tests: gossip message encode/parse
$(GOSSIP_TEST_BIN): $(GOSSIP_TEST_SRC) $(BUILDDIR)/dsm_gossip.o | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(GOSSIP_TEST_SRC) $(BUILDDIR)/dsm_gossip.o $(LDFLAGS)

unit: $(TEST_BIN) $(GOSSIP_TEST_BIN)
	@./$(TEST_BIN)
	@./$(GOSSIP_TEST_BIN)

# Python binding end-to-end test (needs python3 and a free port 9999)
python-test: $(SERVER) $(LIBRARY)
	@echo "Starting Python e2e test..."
	@./$(SERVER) -n 4096 & SERVER_PID=$$!; \
	sleep 1; \
	python3 tests/python_e2e.py; RC=$$?; \
	kill $$SERVER_PID 2>/dev/null || true; wait $$SERVER_PID 2>/dev/null || true; \
	exit $$RC

# Cluster test: 2-node cluster (30s timeout per client)
cluster-test: $(SERVER) $(CLIENT)
	@echo "=== Starting 2-node DSM cluster test ==="
	@./$(SERVER) -p 9001 -c 10001 -r 0:1024 -n 1024 & PID1=$$!; \
	sleep 1; \
	./$(SERVER) -p 9002 -c 10002 -r 1024:2048 -n 1024 -s localhost -S 10001 & PID2=$$!; \
	sleep 3; \
	echo "=== Running benchmark on node 1 ==="; \
	timeout 30 ./$(CLIENT) -H localhost -p 9001 -N 1024 -i 5000 || true; \
	echo "=== Running benchmark on node 2 ==="; \
	timeout 30 ./$(CLIENT) -H localhost -p 9002 -N 1024 -i 5000 || true; \
	kill $$PID1 $$PID2 2>/dev/null || true; wait $$PID1 $$PID2 2>/dev/null || true
	@echo "=== Cluster test complete ==="

# Docker cluster test
cluster-docker: docker-build
	docker compose -f docker-compose.yaml up -d
	@sleep 3
	docker compose run --rm client -H node1 -v -i 10000
	docker compose down

# Docker targets
docker-build:
	docker build -t dsm:latest .

docker-run:
	docker compose up

# Install both binaries
install: $(SERVER) $(CLIENT)
	install -d $(PREFIX)/bin
	install -m 755 $(SERVER) $(PREFIX)/bin/
	install -m 755 $(CLIENT) $(PREFIX)/bin/

help:
	@echo "DSM (Distributed Shared Memory) Build System"
	@echo ""
	@echo "Build Targets:"
	@echo "  all            - Build server and client (default)"
	@echo "  server         - Build dsm-server"
	@echo "  client         - Build dsm-client"
	@echo "  lib            - Build shared library (libdsm.so)"
	@echo "  python         - Build libdsm.so and print Python usage"
	@echo "  debug          - Rebuild with debug symbols and sanitizers"
	@echo "  docker         - Rebuild with portable flags (no -march=native)"
	@echo "  clean          - Remove built files"
	@echo ""
	@echo "Test Targets:"
	@echo "  test           - End-to-end verify test, then unit tests"
	@echo "  unit           - Unit tests (paging, gossip)"
	@echo "  python-test    - Python binding end-to-end test"
	@echo "  cluster-test   - 2-node cluster test"
	@echo "  perf           - Run client under perf stat against a local server"
	@echo ""
	@echo "Docker Targets:"
	@echo "  docker-build   - Build Docker image (dsm:latest)"
	@echo "  docker-run     - Start the cluster with docker compose"
	@echo "  cluster-docker - Run cluster benchmark via docker compose"
	@echo ""
	@echo "Other Targets:"
	@echo "  install        - Install binaries to PREFIX/bin (default /usr/local)"
	@echo "  help           - Show this message"

# Auto-generated header dependencies
-include $(wildcard $(BUILDDIR)/*.d)
