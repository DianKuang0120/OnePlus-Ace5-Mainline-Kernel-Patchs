CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: fpctl

fpctl: fpctl.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f fpctl

.PHONY: all clean
