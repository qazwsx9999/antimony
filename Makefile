# Makefile for building the Antimony compiler (sbc) on Linux / POSIX x64.
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra
TARGET  := sbc
SRCS    := src/util.c src/lexer.c src/parser.c src/check.c src/x64.c \
           src/codegen.c src/pe.c src/llvm_c.c src/fmt.c src/main.c

.PHONY: all clean check-fmt

all: $(TARGET)

$(TARGET): $(SRCS) src/sb.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

check-fmt: $(TARGET)
	./$(TARGET) fmt --check tests/stdlib_test.sb

clean:
	rm -f $(TARGET)
