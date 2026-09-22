# The a compiler and virtual machine, built for this PC.
#
#   make                build build-host/ac and build-host/a
#   make test           compile and run every tests/*.al, check the output
#   make asan           the same under AddressSanitizer, with no pooling
#   make bench          timings for the benchmark programs
#
# The device build is the ESP-IDF component next door (lang/CMakeLists.txt).

CC     ?= cc
OPT    ?= -O3
CFLAGS ?= $(OPT) -g -std=gnu2x -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare
LDLIBS := -lm -pthread

SRC := builtins.c compile.c driver.c image.c lex.c port_host.c quicken.c runtime.c vm.c
HDR := al.h lex.h port.h driver.h
B   := build-host
A   := build-asan
OBJ := $(SRC:%.c=$(B)/%.o)
AOBJ := $(SRC:%.c=$(A)/%.o)
ASAN := -fsanitize=address,undefined -fno-omit-frame-pointer -DAL_NO_POOL

.PHONY: all test asan bench clean
all: $(B)/a $(B)/ac

$(B) $(A):
	@mkdir -p $@

$(B)/%.o: %.c $(HDR) | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/a: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(B)/ac: $(B)/a
	@cp -f $(B)/a $(B)/ac

$(A)/%.o: %.c $(HDR) | $(A)
	$(CC) $(CFLAGS) $(ASAN) -c -o $@ $<

$(A)/a: $(AOBJ)
	$(CC) $(CFLAGS) $(ASAN) -o $@ $(AOBJ) $(LDLIBS)

$(A)/ac: $(A)/a
	@cp -f $(A)/a $(A)/ac

test: all
	@sh tests/run.sh $(B)

# LeakSanitizer is off: a program that stops on a runtime error does not
# unwind on purpose, and the suite checks for leaks with AL_LEAKCHECK anyway.
asan: $(A)/a $(A)/ac
	@ASAN_OPTIONS=detect_leaks=0 sh tests/run.sh $(A)

bench: all
	@sh tests/bench.sh $(B)

clean:
	rm -rf $(B) $(A)
