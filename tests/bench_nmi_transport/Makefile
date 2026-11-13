# Linux-only standalone experiment; modern libbpf (the daemon's bundled
# libbpf on EL8 also works). No bpftool/skeleton or daemon objects required.
CC ?= cc
CLANG ?= clang
LIBBPF_INC ?=
LIBBPF_LIB ?= -lbpf
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror
BPF_CFLAGS ?= -O2 -g -target bpf -D__BPF__ -Wall -Wextra -Werror
MULTIARCH ?= $(shell uname -m)-linux-gnu

.PHONY: all clean test-reader
ifeq ($(shell uname -s),Linux)
all: nmi_transport transport.bpf.o

transport.bpf.o: transport.bpf.c transport.h
	$(CLANG) $(BPF_CFLAGS) $(LIBBPF_INC) -I/usr/include/$(MULTIARCH) -c $< -o $@

nmi_transport: nmi_transport.c transport.h reader.h cadence.h cpu_accounting.h
	$(CC) $(CFLAGS) $(LIBBPF_INC) -o $@ nmi_transport.c $(LIBBPF_LIB) -lelf -lz -pthread
else
all:
	@echo 'NMI transport experiment requires Linux' >&2; exit 1
endif

test_reader: test_reader.c reader.h transport.h cadence.h cpu_accounting.h
	$(CC) $(CFLAGS) -o $@ test_reader.c

test-reader: test_reader
	./test_reader

clean:
	rm -f nmi_transport transport.bpf.o test_reader
	rm -rf test_reader.dSYM
