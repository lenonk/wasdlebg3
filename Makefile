CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
BUILD   := build

# The extender lives in its own repo and is vendored here as a submodule. The
# mod ships as one file with the host statically inside it, so users install a
# single .so and set one LD_PRELOAD.
SE      := vendor/bg3lese
SE_LIB  := $(SE)/build/libbg3lese.a
INC     := -I$(SE)/include -I$(SE)/src $(SDL_CFLAGS)

# --whole-archive is mandatory, not a tuning flag: this plugin never references
# anything in the core, so ordinary archive semantics would drop the extender's
# main.o and with it the constructor and the SDL_PollEvent interposer. The
# result would link and load cleanly and do nothing at all.
SE_LINK := -Wl,--whole-archive $(SE_LIB) -Wl,--no-whole-archive

.PHONY: all test clean se
all: $(BUILD)/bg3le.so $(BUILD)/bg3le-check

$(BUILD):
	@mkdir -p $@

# Build the vendored extender. Auto-inits the submodule so a plain `git clone`
# followed by `make` works.
#
# FORCE, because a rule with no prerequisites is satisfied by the file merely
# existing: bumping the submodule would leave a stale libbg3lese.a in place and
# silently link plugins against the wrong ABI. Recursing every time is cheap and
# the inner make no-ops when it is already current.
$(SE_LIB): FORCE
	@test -f $(SE)/Makefile || git submodule update --init --recursive
	@$(MAKE) -s -C $(SE) build/libbg3lese.a

.PHONY: FORCE
FORCE:

se: $(SE_LIB)

PLUGINS := src/wasd.c src/camlook.c

$(BUILD)/bg3le.so: $(PLUGINS) src/movesig.c src/movesig.h $(SE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared $(INC) -o $@ $(PLUGINS) src/movesig.c \
	  $(SE_LINK) -ldl

$(BUILD)/bg3le-check: src/check.c src/movesig.c $(SE)/src/scan.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_movesig: src/test_movesig.c src/movesig.c $(SE)/src/scan.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/sdl_harness: test/sdl_harness.c | $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $< $(SDL_LIBS)

test: $(BUILD)/test_movesig $(BUILD)/bg3le.so $(BUILD)/sdl_harness
	@./$(BUILD)/test_movesig
	@echo
	@./test/run_shim_test.sh $(BUILD)

clean:
	rm -rf $(BUILD)
	@test -f $(SE)/Makefile && $(MAKE) -C $(SE) clean || true
