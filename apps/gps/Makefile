CC ?= cc
CFLAGS ?= -O2 -Wall -fPIC
QMI_CFLAGS := $(shell pkg-config --cflags qmi-glib)
QMI_LIBS := $(shell pkg-config --libs qmi-glib)

all: locmaster.so

locmaster.so: locmaster.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(QMI_CFLAGS) $(QMI_LIBS) -ldl

clean:
	rm -f locmaster.so
