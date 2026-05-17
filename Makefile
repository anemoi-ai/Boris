# =============================================================================
# Boris - Example Agent from Crafting Agents
# =============================================================================
#
# To build boris, simply type:
#     make
#
# To run boris:
#     ./boris
#
# To run the tests:
#     make test
#
# To clean up and start fresh:
#     make clean
#
# =============================================================================

CC = gcc

ifdef RELEASE
  OPT_FLAGS = -O3 -DNDEBUG -march=native
else
  OPT_FLAGS = -g -O0
endif

CFLAGS = -std=c11 -D_DEFAULT_SOURCE -Wall -Wextra -Werror $(OPT_FLAGS) -MMD -MP

CURL_CFLAGS = $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null)

ALL_CFLAGS = $(CFLAGS) $(CURL_CFLAGS)

BUILD_DIR = build
OBJ_DIR = $(BUILD_DIR)/obj

SOURCE_FILES = $(wildcard src/*.c)
OBJECT_FILES = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCE_FILES))

TOOL_FILES = $(wildcard src/tools/*.c)
TOOL_OBJECTS = $(patsubst src/tools/%.c,$(OBJ_DIR)/tools_%.o,$(TOOL_FILES))

VENDOR_FILES = vendor/cJSON.c
VENDOR_OBJECTS = $(patsubst vendor/%.c,$(OBJ_DIR)/vendor_%.o,$(VENDOR_FILES))

ALL_OBJECT_FILES = $(OBJECT_FILES) $(TOOL_OBJECTS) $(VENDOR_OBJECTS)

LIB_OBJECT_FILES = $(filter-out $(OBJ_DIR)/main.o,$(ALL_OBJECT_FILES))

# test_e2e links tests/mock_http.c instead of the real http_client.o
LIB_OBJECT_FILES_NO_HTTP = $(filter-out $(OBJ_DIR)/http_client.o,$(LIB_OBJECT_FILES))

TARGET = boris

TEST_SOURCE_FILES = $(wildcard tests/test_*.c)
TEST_TARGETS = $(patsubst tests/%.c,$(BUILD_DIR)/%,$(TEST_SOURCE_FILES))

DEP_FILES = $(ALL_OBJECT_FILES:.o=.d)
-include $(DEP_FILES)

all: $(TARGET)
	@echo ""
	@echo "Run ./boris to begin."

$(TARGET): $(ALL_OBJECT_FILES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(CURL_LIBS)

$(OBJ_DIR)/%.o: src/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(ALL_CFLAGS) -I include -I vendor -c $< -o $@

$(OBJ_DIR)/tools_%.o: src/tools/%.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(ALL_CFLAGS) -I include -I vendor -c $< -o $@

$(OBJ_DIR)/vendor_cJSON.o: vendor/cJSON.c
	@mkdir -p $(OBJ_DIR)
	$(CC) $(ALL_CFLAGS) -I include -I vendor -c $< -o $@

# =============================================================================
# Running the tests
# =============================================================================

test: $(TEST_TARGETS)
	@echo ""
	@echo "Running tests..."
	@passed=0; failed=0; \
	for test in $(TEST_TARGETS); do \
		echo ""; \
		echo "--- Running $$test ---"; \
		if $$test; then \
			passed=$$((passed + 1)); \
		else \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "================================"; \
	echo "Results: $$passed passed, $$failed failed"; \
	echo "================================"

# test_e2e links mock_http.c instead of the real http_client.o
$(BUILD_DIR)/test_e2e: tests/test_e2e.c $(LIB_OBJECT_FILES_NO_HTTP) tests/mock_http.c
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -I include -I vendor -I tests -o $@ $^ $(CURL_LIBS)

# Compile each test file (linked with library objects, not main.c)
$(BUILD_DIR)/test_%: tests/test_%.c $(LIB_OBJECT_FILES)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(ALL_CFLAGS) -I include -I vendor -I tests -o $@ $^ $(CURL_LIBS)

# =============================================================================
# Cleaning up
# =============================================================================

clean:
	rm -rf $(BUILD_DIR) boris
	@echo "Build directory cleaned."

# =============================================================================
# Valgrind memory checking
# =============================================================================

memcheck: $(TEST_TARGETS)
	@echo ""
	@echo "Running tests under Valgrind..."
	@for test in $(TEST_TARGETS); do \
		echo ""; \
		echo "--- Valgrind: $$test ---"; \
		valgrind --leak-check=full --error-exitcode=1 $$test || exit 1; \
	done
	@echo ""
	@echo "All tests pass under Valgrind."
