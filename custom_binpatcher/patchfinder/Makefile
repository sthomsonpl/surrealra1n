CC ?= cc
CFLAGS ?= -O2

SOURCE := custombin_patchfinder.c
OUTPUT := ../../bin/custombin_patchfinder

.PHONY: all clean

all: $(OUTPUT)

$(OUTPUT): $(SOURCE)
	mkdir -p ../../bin
	$(CC) -std=c11 $(CFLAGS) -Wall -Wextra $(SOURCE) -o $(OUTPUT)

clean:
	$(RM) $(OUTPUT)
