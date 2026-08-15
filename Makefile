# Distributed Shared Memory (DSM) System
# Build configuration

CC       := gcc
CFLAGS   := -O3 -march=native -mtune=native -flto -fomit-frame-pointer
CFLAGS   += -Wall -Wextra -std=gnu11
CFLAGS   += -Iinclude
# -Wl,-z,now: Eager binding eliminates lazy symbol resolution overhead (~25% client CPU)
LDFLAGS  := -pthread -flto -Wl,-z,now

# Source files
SRCDIR   := src
INCDIR   := include
BUILDDIR := build

# Common sources (linked into both binaries)
COMMON_SRC := $(SRCDIR)/dsm_common.c $(SRCDIR)/dsm_config.c $(SRCDIR)/dsm_paging.c
COMMON_OBJ := $(BUILDDIR)/dsm_common.o $(BUILDDIR)/dsm_config.o $(BUILDDIR)/dsm_paging.o

# High-level API sources (region + array)
API_SRC := $(SRCDIR)/dsm_region.c $(SRCDIR)/dsm_array.c
API_OBJ := $(BUILDDIR)/dsm_region.o $(BUILDDIR)/dsm_array.o

# Cluster sources (server only)
CLUSTER_SRC := $(SRCDIR)/dsm_cluster.c $(SRCDIR)/dsm_gossip.c
CLUSTER_OBJ := $(BUILDDIR)/dsm_cluster.o $(BUILDDIR)/dsm_gossip.o

# io_uring sources
URING_SRC := $(SRCDIR)/dsm_uring.c
URING_OBJ := $(BUILDDIR)/dsm_uring.o
URING_LDFLAGS := -luring

# Unit test sources
TEST_SRC := tests/unit_paging.c
TEST_BIN := $(BUILDDIR)/unit_paging

# Targets
SERVER   := dsm-server
SERVER_URING := dsm-server-uring
CLIENT   := dsm-client
LIBRARY  := libdsm.so

# Docker-friendly flags (no -march=native for portability)
DOCKER_CFLAGS := -O3 -flto -fomit-frame-pointer -Wall -Wextra -std=gnu11 -Iinclude

.PHONY: all clean perf debug docker-build docker-run test unit cluster-test e2e-exhaustive install help server client uring lib python

all: $(SERVER) $(CLIENT)

uring: $(SERVER_URING) $(CLIENT)

lib: $(LIBRARY)

python: $(LIBRARY)
	@echo "Python bindings ready. Usage:"
	@echo "  export LD_LIBRARY_PATH=\$$(pwd):\$$LD_LIBRARY_PATH"
	@echo "  python3 -c 'import python.dsm as dsm; print(dsm.__doc__)'"

# Create build directory
$(BUILDDIR):
	@mkdir -p $(BUILDDIR)

# Compile common objects
$(BUILDDIR)/dsm_common.o: $(SRCDIR)/dsm_common.c $(INCDIR)/dsm_protocol.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/dsm_config.o: $(SRCDIR)/dsm_config.c $(INCDIR)/dsm_config.h $(INCDIR)/dsm_protocol.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/dsm_paging.o: $(SRCDIR)/dsm_paging.c $(INCDIR)/dsm_paging.h $(INCDIR)/dsm_protocol.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# Compile cluster objects
$(BUILDDIR)/dsm_cluster.o: $(SRCDIR)/dsm_cluster.c $(INCDIR)/dsm_cluster.h $(INCDIR)/dsm_gossip.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/dsm_gossip.o: $(SRCDIR)/dsm_gossip.c $(INCDIR)/dsm_gossip.h $(INCDIR)/dsm_cluster.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/dsm_server.o: $(SRCDIR)/dsm_server.c $(INCDIR)/dsm_protocol.h $(INCDIR)/dsm_paging.h $(INCDIR)/dsm_config.h $(INCDIR)/dsm_cluster.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/dsm_server_uring.o: $(SRCDIR)/dsm_server_uring.c $(INCDIR)/dsm_protocol.h $(INCDIR)/dsm_config.h $(INCDIR)/dsm_uring.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILDDIR)/dsm_client.o: $(SRCDIR)/dsm_client.c $(INCDIR)/dsm_protocol.h $(INCDIR)/dsm_paging.h $(INCDIR)/dsm_config.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# io_uring object
$(BUILDDIR)/dsm_uring.o: $(SRCDIR)/dsm_uring.c $(INCDIR)/dsm_uring.h $(INCDIR)/dsm_protocol.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c -o $@ $<

# High-level API objects (for shared library)
$(BUILDDIR)/dsm_region.o: $(SRCDIR)/dsm_region.c $(INCDIR)/dsm_region.h $(INCDIR)/dsm_paging.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(BUILDDIR)/dsm_array.o: $(SRCDIR)/dsm_array.c $(INCDIR)/dsm_array.h $(INCDIR)/dsm_region.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

# PIC versions of common objects for shared library
$(BUILDDIR)/dsm_common_pic.o: $(SRCDIR)/dsm_common.c $(INCDIR)/dsm_protocol.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(BUILDDIR)/dsm_paging_pic.o: $(SRCDIR)/dsm_paging.c $(INCDIR)/dsm_paging.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

$(BUILDDIR)/dsm_config_pic.o: $(SRCDIR)/dsm_config.c $(INCDIR)/dsm_config.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

# Shared library for Python bindings
$(LIBRARY): $(API_OBJ) $(BUILDDIR)/dsm_common_pic.o $(BUILDDIR)/dsm_paging_pic.o $(BUILDDIR)/dsm_config_pic.o
	$(CC) -shared $(LDFLAGS) -o $@ $^
	@echo "Built $(LIBRARY) for Python bindings"

# Link server (includes cluster support)
$(SERVER): $(BUILDDIR)/dsm_server.o $(COMMON_OBJ) $(CLUSTER_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Built $(SERVER)"

# Link io_uring server
$(SERVER_URING): $(BUILDDIR)/dsm_server_uring.o $(COMMON_OBJ) $(CLUSTER_OBJ) $(URING_OBJ)
	$(CC) $(LDFLAGS) $(URING_LDFLAGS) -o $@ $^
	@echo "Built $(SERVER_URING) (io_uring enabled)"

# Link client
$(CLIENT): $(BUILDDIR)/dsm_client.o $(COMMON_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "Built $(CLIENT)"

# Convenience targets
server: $(SERVER)
client: $(CLIENT)

# Example programs
examples: $(LIBRARY)
	@mkdir -p examples
	$(CC) $(CFLAGS) -o examples/matrix_demo examples/matrix_demo.c -L. -Wl,-rpath,'$$ORIGIN/..' $(LIBRARY)
	@echo "Built examples/matrix_demo"

# Docker build (portable flags)
docker: CFLAGS := $(DOCKER_CFLAGS)
docker: clean all
	@echo "Built for Docker with portable flags"

# Debug build
debug: CFLAGS := -O0 -g -Wall -Wextra -std=gnu11 -Iinclude -fsanitize=address,undefined
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
	rm -rf $(BUILDDIR) $(SERVER) $(SERVER_URING) $(CLIENT) $(LIBRARY)
	rm -f examples/matmul_bench examples/cluster_bench examples/matrix_demo

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
$(TEST_BIN): $(TEST_SRC) $(COMMON_OBJ) $(INCDIR)/dsm_paging.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC) $(COMMON_OBJ) $(LDFLAGS)

unit: $(TEST_BIN)
	@./$(TEST_BIN)

# Cluster test: 2-node cluster (30s timeout per client)
cluster-test: $(SERVER) $(CLIENT)
	@echo "=== Starting 2-node DSM cluster test ==="
	@pkill -9 dsm-server 2>/dev/null || true; sleep 1
	@./$(SERVER) -p 9001 -c 10001 -r 0:1024 -n 1024 & PID1=$$!; \
	sleep 1; \
	./$(SERVER) -p 9002 -c 10002 -r 1024:2048 -n 1024 -s localhost -S 10001 & PID2=$$!; \
	sleep 3; \
	echo "=== Running benchmark on node 1 ==="; \
	timeout 30 ./$(CLIENT) -H localhost -p 9001 -N 1024 -i 5000 -b || true; \
	echo "=== Running benchmark on node 2 ==="; \
	timeout 30 ./$(CLIENT) -H localhost -p 9002 -N 1024 -i 5000 -b || true; \
	kill $$PID1 $$PID2 2>/dev/null || true; wait $$PID1 $$PID2 2>/dev/null || true
	@echo "=== Cluster test complete ==="

# Docker cluster test
cluster-docker: docker-build
	docker-compose -f docker-compose.yaml up --scale client=0 -d
	@sleep 3
	docker-compose run --rm client -H node1 -v -i 10000
	docker-compose down

# Exhaustive E2E fuzz test with perf stats
e2e-exhaustive: $(SERVER) $(CLIENT)
	@chmod +x tests/e2e_fuzz_test.sh
	@tests/e2e_fuzz_test.sh all

e2e-quick: $(SERVER) $(CLIENT)
	@chmod +x tests/e2e_fuzz_test.sh
	@tests/e2e_fuzz_test.sh quick

e2e-chaos: $(SERVER) $(CLIENT)
	@chmod +x tests/e2e_fuzz_test.sh
	@FUZZ_DURATION=60 tests/e2e_fuzz_test.sh chaos

e2e-perf: $(SERVER) $(CLIENT)
	@chmod +x tests/e2e_fuzz_test.sh
	@tests/e2e_fuzz_test.sh perf

# Docker targets
docker-build:
	docker build -t dsm:latest .

docker-run:
	docker-compose up

# Install both binaries
install: $(SERVER) $(CLIENT)
	install -m 755 $(SERVER) /usr/local/bin/
	install -m 755 $(CLIENT) /usr/local/bin/

help:
	@echo "DSM (Distributed Shared Memory) Build System"
	@echo ""
	@echo "Build Targets:"
	@echo "  all            - Build server and client (default)"
	@echo "  lib            - Build shared library (libdsm.so)"
	@echo "  uring          - Build io_uring server variant"
	@echo "  debug          - Build with debug symbols and sanitizers"
	@echo "  clean          - Remove built files"
	@echo ""
	@echo "Test Targets:"
	@echo "  test           - Single server/client test (30s timeout)"
	@echo "  cluster-test   - 2-node cluster test (30s timeout)"
	@echo "  e2e-quick      - Quick E2E fuzz test (~60s)"
	@echo "  e2e-exhaustive - Full E2E fuzz test suite"
	@echo ""
	@echo "Docker Targets:"
	@echo "  docker-build   - Build Docker image"
	@echo "  docker-run     - Run with docker-compose"
