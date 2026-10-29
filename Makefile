CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -g -D_GNU_SOURCE
LDLIBS = -lX11

SOURCES = main.c gui.c shell.c parser.c builtins.c hotkeys.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean
all: terminator

terminator: $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

%.o: %.c terminator.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJECTS) terminator
