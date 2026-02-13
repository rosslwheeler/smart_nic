BUILD_DIR ?= build
BUILD_TYPE ?= Debug
TRACY_TOOL_BUILD_TYPE ?= Release
TRACY_PROFILER_OPTS ?= -DLEGACY=ON
TRACY_TOOL_PARALLEL ?= $(shell nproc)
TRACY_CPM_CACHE ?= $(HOME)/.cache/cpm
CMAKE_GENERATOR ?=
CMAKE ?= cmake
NIC_ENABLE_TRACY ?= ON

# Prefer the gcov that matches the configured compiler, then fall back.
CXX ?= g++
GCOV_TOOL := $(shell $(CXX) -print-prog-name=gcov 2>/dev/null)
ifeq ($(strip $(GCOV_TOOL)),)
GCOV_TOOL := $(shell which gcov-14 2>/dev/null || which gcov 2>/dev/null || echo gcov)
endif

.PHONY: all build test test-trace clean clean-tracy tracy-profiler tracy-capture coverage asan

all: build

# Generate build tree and cache once; reused by build/test targets.
$(BUILD_DIR)/CMakeCache.txt:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DNIC_ENABLE_TRACY=$(NIC_ENABLE_TRACY) $(CMAKE_GENERATOR) $(CMAKE_ARGS)

configure: $(BUILD_DIR)/CMakeCache.txt

build: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --build $(BUILD_DIR) --config $(BUILD_TYPE)

test: test-trace

test-notrace: build
	$(CMAKE) --build $(BUILD_DIR) --config $(BUILD_TYPE)
	ctest --test-dir $(BUILD_DIR) --output-on-failure

# Run tests with Tracy capture - starts capture in background, runs tests, waits for sync
TRACY_TRACE_FILE ?= $(BUILD_DIR)/ctests.tracy

test-trace: build tracy-capture
	@echo "Starting Tracy capture in background..."
	@$(TRACY_CAPTURE_BUILD_DIR)/tracy-capture -f -o $(TRACY_TRACE_FILE) & \
		CAPTURE_PID=$$!; \
		sleep 2; \
		echo "Running tests..."; \
		ctest --test-dir $(BUILD_DIR) --output-on-failure; \
		TEST_EXIT=$$?; \
		echo "Waiting for Tracy capture to sync..."; \
		sleep 2; \
		kill -INT $$CAPTURE_PID 2>/dev/null || true; \
		wait $$CAPTURE_PID 2>/dev/null || true; \
		echo "Trace saved to $(TRACY_TRACE_FILE)"; \
		exit $$TEST_EXIT

clean:
	$(RM) -r $(BUILD_DIR) $(TRACY_PROFILER_BUILD_DIR) $(TRACY_CAPTURE_BUILD_DIR) CMakeFiles CMakeCache.txt cmake_install.cmake Testing CTestTestfile.cmake compile_commands.json

clean-tracy:
	$(RM) -r $(TRACY_PROFILER_BUILD_DIR) $(TRACY_CAPTURE_BUILD_DIR)

# Tracy utilities (GUI profiler and capture tool)
TRACY_PROFILER_BUILD_DIR ?= $(BUILD_DIR)/tracy_profiler
TRACY_CAPTURE_BUILD_DIR ?= $(BUILD_DIR)/tracy_capture

tracy-profiler:
	$(CMAKE) -E make_directory $(TRACY_CPM_CACHE)
	CPM_SOURCE_CACHE=$(TRACY_CPM_CACHE) $(CMAKE) -S third-party/tracy/profiler -B $(TRACY_PROFILER_BUILD_DIR) -DCMAKE_BUILD_TYPE=$(TRACY_TOOL_BUILD_TYPE) $(TRACY_PROFILER_OPTS) $(CMAKE_GENERATOR) $(CMAKE_ARGS)
	CPM_SOURCE_CACHE=$(TRACY_CPM_CACHE) $(CMAKE) --build $(TRACY_PROFILER_BUILD_DIR) --config $(TRACY_TOOL_BUILD_TYPE) --parallel $(TRACY_TOOL_PARALLEL)

tracy-capture:
	$(CMAKE) -E make_directory $(TRACY_CPM_CACHE)
	CPM_SOURCE_CACHE=$(TRACY_CPM_CACHE) $(CMAKE) -S third-party/tracy/capture -B $(TRACY_CAPTURE_BUILD_DIR) -DCMAKE_BUILD_TYPE=$(TRACY_TOOL_BUILD_TYPE) $(CMAKE_GENERATOR) $(CMAKE_ARGS)
	CPM_SOURCE_CACHE=$(TRACY_CPM_CACHE) $(CMAKE) --build $(TRACY_CAPTURE_BUILD_DIR) --config $(TRACY_TOOL_BUILD_TYPE) --parallel $(TRACY_TOOL_PARALLEL)

coverage:
	$(CMAKE) -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=$(CXX) -DNIC_ENABLE_COVERAGE=ON -DNIC_ENABLE_TRACY=ON $(CMAKE_GENERATOR) $(CMAKE_ARGS)
	$(CMAKE) --build build-coverage --config Debug
	ctest --test-dir build-coverage --output-on-failure
	$(CMAKE) -E make_directory build-coverage/coverage
	lcov --gcov-tool $(GCOV_TOOL) --capture --directory build-coverage --output-file build-coverage/coverage/coverage.info
	lcov --gcov-tool $(GCOV_TOOL) --remove build-coverage/coverage/coverage.info "*/tests/*" "*/third-party/*" "*/libs/bit_fields/*" "*/usr/include/*" --output-file build-coverage/coverage/coverage.info
	genhtml build-coverage/coverage/coverage.info --output-directory build-coverage/coverage/html

asan:
	$(CMAKE) -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DNIC_ENABLE_ASAN=ON -DNIC_ENABLE_TRACY=ON $(CMAKE_GENERATOR) $(CMAKE_ARGS)
	$(CMAKE) --build build-asan --config Debug
	ctest --test-dir build-asan --output-on-failure
