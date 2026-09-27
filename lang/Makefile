# The pico compiler and virtual machine, built for this PC.
#
#   make                build build-host/picoc and build-host/pico
#   make test           compile and run every tests/*.pico, check the output
#   make asan           the same under AddressSanitizer, with no pooling
#   make bench          timings for the benchmark programs
#
# The device build is the ESP-IDF component next door (lang/CMakeLists.txt).

CC     ?= cc
OPT    ?= -O3
CFLAGS ?= $(OPT) -g -std=gnu2x -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare
LDLIBS := -lm -pthread

SRC := builtins.c compile.c driver.c image.c lex.c port_host.c quicken.c runtime.c vm.c
HDR := pico.h lex.h port.h driver.h
B   := build-host
A   := build-asan
OBJ := $(SRC:%.c=$(B)/%.o)
AOBJ := $(SRC:%.c=$(A)/%.o)
ASAN := -fsanitize=address,undefined -fno-omit-frame-pointer -DAL_NO_POOL

.PHONY: all test asan bench clean
all: $(B)/pico $(B)/picoc

$(B) $(A):
	@mkdir -p $@

$(B)/%.o: %.c $(HDR) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/pico: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(B)/picoc: $(B)/pico
	@cp -f $(B)/pico $(B)/picoc

$(A)/%.o: %.c $(HDR) | $(A)
	$(CC) $(CFLAGS) $(ASAN) -c -o $@ $<

$(A)/pico: $(AOBJ)
	$(CC) $(CFLAGS) $(ASAN) -o $@ $(AOBJ) $(LDLIBS)

$(A)/picoc: $(A)/pico
	@cp -f $(A)/pico $(A)/picoc

test: all
	@sh tests/run.sh $(B)

# LeakSanitizer is off: a program that stops on a runtime error does not
# unwind on purpose, and the suite checks for leaks with PICO_LEAKCHECK anyway.
asan: $(A)/pico $(A)/picoc
	@ASAN_OPTIONS=detect_leaks=0 sh tests/run.sh $(A)

bench: all
	@sh tests/bench.sh $(B)

clean:
	rm -rf $(B) $(A)
