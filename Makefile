CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -fvisibility=hidden
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
BUILD   := build

.PHONY: all test clean
all: $(BUILD)/bg3le.so

$(BUILD):
	@mkdir -p $@

# The shim itself. Exported visibility on SDL_PollEvent only — everything else
# stays hidden so we cannot accidentally interpose a symbol the game relies on.
$(BUILD)/bg3le.so: src/bg3le.c src/symres.c src/symres.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ src/bg3le.c src/symres.c $(SDL_CFLAGS) -ldl

$(BUILD)/test_symres: src/test_symres.c src/symres.c src/symres.h | $(BUILD)
	$(CC) $(CFLAGS) -fPIE -pie -o $@ src/test_symres.c src/symres.c

$(BUILD)/sdl_harness: test/sdl_harness.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(SDL_CFLAGS) $(SDL_LIBS)

test: $(BUILD)/test_symres $(BUILD)/bg3le.so $(BUILD)/sdl_harness
	@./$(BUILD)/test_symres
	@echo
	@./test/run_shim_test.sh $(BUILD)

clean:
	rm -rf $(BUILD)
