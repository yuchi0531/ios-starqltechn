# Host test for the starqltechn loader: runs dl_main() on x86 with the board
# addresses mmap'd, so its validation, copy and logging paths execute without an
# aarch64 cross compiler or a phone. The cross build is build.sh.
#
#   make -C shim/starqltechn            # host test
#   make -C shim/starqltechn clean
CC ?= gcc
CFLAGS ?= -O1 -g -Wall -Wextra -Werror -I..

all: out/starq_host_test
	./out/starq_host_test

out/starq_host_test: host_test.c loader.c ../fdt.c loader.h
	@mkdir -p out
	$(CC) $(CFLAGS) -o $@ host_test.c loader.c ../fdt.c

clean:
	rm -rf out

.PHONY: all clean
