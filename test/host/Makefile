# Host (PC) unit tests for the protocol code. Run: make -C test/host
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined

all: test_proto test_apple
	./test_proto
	./test_apple

test_proto: test_proto.c ../../main/yamaha_proto.c ../../main/yamaha_proto.h
	$(CC) $(CFLAGS) -o $@ test_proto.c ../../main/yamaha_proto.c

test_apple: test_apple.c ../../main/apple_proto.c ../../main/apple_proto.h
	$(CC) $(CFLAGS) -o $@ test_apple.c ../../main/apple_proto.c

clean:
	rm -f test_proto test_apple

.PHONY: all clean
