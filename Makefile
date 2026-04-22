# Makefile â libhive build system
#
# Targets:
#   make / make dev   - debug build with ASan/UBSan (Linux only)
#   make release      - optimised hardened build
#   make test         - build and run unit tests (ASan/UBSan)
#   make test-tsan    - TSan build, Linux only
#   make valgrind     - run tests under Valgrind, Linux only
#   make h2spec-server - build conformance test server
#   make h2spec       - run h2spec conformance suite
#   make tools        - build developer tools (frame decoder)
#   make lint         - clang-tidy + cppcheck
#   make format       - clang-format -i on all sources
#   make clean        - remove build artifacts
#   make install      - install libhive.a and include/hive.h
#
# Compatible with GNU make (Linux) and BSD make (OpenBSD).
# Uses != for shell assignment - supported by GNU make >= 3.82 and BSD make.
# Explicit per-file compile rules - no pattern rules (BSD make portable).
# See TECH_STACK.md Â§5 for full build system documentation.
# --- Platform detection -----------------------------------------------------
OS != uname -s
# --- Compiler ---------------------------------------------------------------
CC = clang
AR = ar
# --- Platform-specific flags ------------------------------------------------
CFLAGS_OS != if [ "$(OS)" = "Linux" ]; then echo "-DLINUX"; \
             elif [ "$(OS)" = "OpenBSD" ]; then echo "-DOPENBSD"; \
             else echo ""; fi
# strlcpy/strlcat compat â Linux only; OpenBSD libc provides them.
COMPAT_SRC != if [ "$(OS)" = "Linux" ]; then echo "src/compat_str.c"; \
              else echo ""; fi
# ASan/UBSan â Linux only; OpenBSD clang does not support -fsanitize=address.
SANITIZERS != if [ "$(OS)" = "Linux" ]; then \
                  echo "-fsanitize=address,undefined"; \
              else echo ""; fi
# --- Compiler flags ---------------------------------------------------------
CSTD = -std=c11
CWARN = -Wall -Wextra -Wpedantic \
        -Wshadow \
        -Wformat=2 \
        -Wconversion \
        -Wnull-dereference \
        -Wstrict-prototypes \
        -Wmissing-prototypes \
        -Wold-style-definition
# Feature test macros â defined here, not in source files.
# See TECH_STACK.md Â§1.
CFLAGS_FT = -D_POSIX_C_SOURCE=200809L \
            -D_XOPEN_SOURCE=700
CFLAGS_DEV  = $(CSTD) $(CWARN) $(CFLAGS_FT) $(CFLAGS_OS) \
              -g -O0 \
              $(SANITIZERS) \
              -fno-omit-frame-pointer \
              -DDEBUG \
              -DHIVE_DEBUG=1
CFLAGS_REL  = $(CSTD) $(CWARN) $(CFLAGS_FT) $(CFLAGS_OS) \
              -O2 \
              -DNDEBUG \
              -fstack-protector-strong \
              -fPIC \
              -D_FORTIFY_SOURCE=2
CFLAGS_VG   = $(CSTD) $(CWARN) $(CFLAGS_FT) $(CFLAGS_OS) \
              -g -O0 \
              -DDEBUG \
              -DHIVE_DEBUG=1
CFLAGS_TSAN = $(CSTD) $(CWARN) $(CFLAGS_FT) $(CFLAGS_OS) \
              -g -O0 \
              -fsanitize=thread \
              -fno-omit-frame-pointer \
              -DDEBUG \
              -DHIVE_DEBUG=1
# Extra flags hook: make dev EXTRA_CFLAGS=-DHIVE_TEST_CLOCK=1
EXTRA_CFLAGS ?=
INCLUDES = -I include/
# --- Build paths ------------------------------------------------------------
BUILD_DIR       = build
BUILD_REL_DIR   = $(BUILD_DIR)/rel
BUILD_VG_DIR    = $(BUILD_DIR)/vg
BUILD_TSAN_DIR  = $(BUILD_DIR)/tsan
BUILD_TEST_DIR  = $(BUILD_DIR)/tests
BUILD_TOOLS_DIR = $(BUILD_DIR)/tools
LIB_DEV  = $(BUILD_DIR)/libhive.a
LIB_REL  = $(BUILD_REL_DIR)/libhive.a
LIB_VG   = $(BUILD_VG_DIR)/libhive.a
LIB_TSAN = $(BUILD_TSAN_DIR)/libhive.a
TEST_BIN    = $(BUILD_TEST_DIR)/run_tests
TEST_BIN_VG = $(BUILD_TEST_DIR)/run_tests_vg
# Top-level copy â embedders and TECH_STACK.md reference libhive.a here.
LIBHIVE_A = libhive.a
# --- Test source ------------------------------------------------------------
TEST_SRC = tests/run_tests.c \
           tests/test_compat.c
# --- Install paths ----------------------------------------------------------
PREFIX     ?= /usr/local
LIBDIR     ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
# --- Phony targets ----------------------------------------------------------
.PHONY: all dev release test test-asan test-tsan valgrind \
        h2spec-server h2spec tools lint format clean install
all: dev
# --- Development build ------------------------------------------------------
dev: $(LIB_DEV)
	cp $(LIB_DEV) $(LIBHIVE_A)
# --- Release build ----------------------------------------------------------
release: $(LIB_REL)
	cp $(LIB_REL) $(LIBHIVE_A)
# --- Test (ASan/UBSan) ------------------------------------------------------
test: $(TEST_BIN)
	$(TEST_BIN)
test-asan: test
# --- TSan (Linux only) ------------------------------------------------------
test-tsan: $(LIB_TSAN)
	@mkdir -p $(BUILD_TEST_DIR)
	$(CC) $(CFLAGS_TSAN) $(EXTRA_CFLAGS) $(INCLUDES) \
	    $(TEST_SRC) $(LIB_TSAN) -o $(BUILD_TEST_DIR)/run_tests_tsan
	$(BUILD_TEST_DIR)/run_tests_tsan
# --- Valgrind (Linux only) --------------------------------------------------
valgrind: $(TEST_BIN_VG)
	valgrind --leak-check=full \
	         --show-leak-kinds=all \
	         --track-origins=yes \
	         --error-exitcode=1 \
	         $(TEST_BIN_VG)
# --- Conformance test server ------------------------------------------------
h2spec-server: $(LIB_REL)
	$(CC) $(CFLAGS_REL) $(EXTRA_CFLAGS) $(INCLUDES) \
	    tests/h2spec_server.c $(LIB_REL) -ltls \
	    -o tests/h2spec_server
h2spec:
	h2spec -h 127.0.0.1 -p 8443 --tls --insecure
# --- Developer tools --------------------------------------------------------
tools: $(LIB_DEV)
	@mkdir -p $(BUILD_TOOLS_DIR)
	$(CC) $(CFLAGS_REL) $(EXTRA_CFLAGS) $(INCLUDES) \
	    tools/hive_decode.c src/hive_frame_bare.c \
	    -o $(BUILD_TOOLS_DIR)/hive_decode
# --- Lint -------------------------------------------------------------------
lint:
	clang-tidy src/*.c -- $(CFLAGS_DEV) $(EXTRA_CFLAGS) $(INCLUDES)
	cppcheck --enable=all --error-exitcode=1 \
	         --suppress=missingIncludeSystem \
	         src/
# --- Format -----------------------------------------------------------------
format:
	clang-format -i src/*.c src/*.h include/hive.h
# --- Install ----------------------------------------------------------------
install: $(LIB_REL)
	install -d $(LIBDIR) $(INCLUDEDIR)
	install -m 644 $(LIB_REL)     $(LIBDIR)/libhive.a
	install -m 644 include/hive.h $(INCLUDEDIR)/hive.h
# --- Clean ------------------------------------------------------------------
clean:
	rm -rf $(BUILD_DIR)
	rm -f $(LIBHIVE_A) tests/h2spec_server
# ---------------------------------------------------------------------------
# Library build rules â explicit per-file compile, no pattern rules.
# BSD make does not support GNU-style pattern rules portably.
# ---------------------------------------------------------------------------
# --- Development library ----------------------------------------------------
$(LIB_DEV): src/hive.c $(COMPAT_SRC)
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS_DEV) $(EXTRA_CFLAGS) $(INCLUDES) \
	    -c src/hive.c -o $(BUILD_DIR)/hive.o
	test -z "$(COMPAT_SRC)" || \
	    $(CC) $(CFLAGS_DEV) $(EXTRA_CFLAGS) $(INCLUDES) \
	    -c src/compat_str.c -o $(BUILD_DIR)/compat_str.o
	ar rcs $(LIB_DEV) $(BUILD_DIR)/hive.o
	test -z "$(COMPAT_SRC)" || ar qs $(LIB_DEV) $(BUILD_DIR)/compat_str.o
# --- Release library --------------------------------------------------------
$(LIB_REL): src/hive.c $(COMPAT_SRC)
	@mkdir -p $(BUILD_REL_DIR)
	$(CC) $(CFLAGS_REL) $(EXTRA_CFLAGS) $(INCLUDES) \
	    -c src/hive.c -o $(BUILD_REL_DIR)/hive.o
	test -z "$(COMPAT_SRC)" || \
	    $(CC) $(CFLAGS_REL) $(EXTRA_CFLAGS) $(INCLUDES) \
	    -c src/compat_str.c -o $(BUILD_REL_DIR)/compat_str.o
	ar rcs $(LIB_REL) $(BUILD_REL_DIR)/hive.o
	test -z "$(COMPAT_SRC)" || ar qs $(LIB_REL) $(BUILD_REL_DIR)/compat_str.o
# --- Valgrind library (no sanitizers) ---------------------------------------
$(LIB_VG): src/hive.c $(COMPAT_SRC)
	@mkdir -p $(BUILD_VG_DIR)
	$(CC) $(CFLAGS_VG) $(EXTRA_CFLAGS) $(INCLUDES) \
	    -c src/hive.c -o $(BUILD_VG_DIR)/hive.o
	test -z "$(COMPAT_SRC)" || \
	    $(CC) $(CFLAGS_VG) $(EXTRA_CFLAGS) $(INCLUDES) \
	    -c src/compat_str.c -o $(BUILD_VG_DIR)/compat_str.o
	ar rcs $(LIB_VG) $(BUILD_VG_DIR)/hive.o
	test -z "$(COMPAT_SRC)" || ar qs $(LIB_VG) $(BUILD_VG_DIR)/compat_str.o
# --- TSan library -----------------------------------------------------------
$(LIB_TSAN): src/hive.c $(COMPAT_SRC)
	@mkdir -p $(BUILD_TSAN_DIR)
	$(CC) $(CFLAGS_TSAN) $(EXTRA_CFLAGS) $(INCLUDES) \
	    -c src/hive.c -o $(BUILD_TSAN_DIR)/hive.o
	test -z "$(COMPAT_SRC)" || \
	    $(CC) $(CFLAGS_TSAN) $(EXTRA_CFLAGS) $(INCLUDES) \
	    -c src/compat_str.c -o $(BUILD_TSAN_DIR)/compat_str.o
	ar rcs $(LIB_TSAN) $(BUILD_TSAN_DIR)/hive.o
	test -z "$(COMPAT_SRC)" || ar qs $(LIB_TSAN) $(BUILD_TSAN_DIR)/compat_str.o
# --- Test binary (ASan/UBSan) -----------------------------------------------
$(TEST_BIN): $(LIB_DEV) $(TEST_SRC)
	@mkdir -p $(BUILD_TEST_DIR)
	$(CC) $(CFLAGS_DEV) $(EXTRA_CFLAGS) $(INCLUDES) \
	    $(TEST_SRC) $(LIB_DEV) -o $(TEST_BIN)
# --- Valgrind test binary (no sanitizers) -----------------------------------
$(TEST_BIN_VG): $(LIB_VG) $(TEST_SRC)
	@mkdir -p $(BUILD_TEST_DIR)
	$(CC) $(CFLAGS_VG) $(EXTRA_CFLAGS) $(INCLUDES) \
	    $(TEST_SRC) $(LIB_VG) -o $(TEST_BIN_VG)
