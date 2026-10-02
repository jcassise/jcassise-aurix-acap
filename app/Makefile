# Built inside the ACAP Native SDK container (see ../Dockerfile).
PROG       = aurix
PKGS       = glib-2.0 gio-2.0 vdostream liblarod
PKG_CONFIG ?= pkg-config

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)

CFLAGS += -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lm

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f $(PROG) $(OBJS) *.eap *LICENSE.txt package.conf* param.conf

.PHONY: all clean
