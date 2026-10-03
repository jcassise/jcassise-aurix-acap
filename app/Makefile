# Built inside the ACAP Native SDK container (see ../Dockerfile).
PROG       = aurix
PKGS       = glib-2.0 gio-2.0 vdostream liblarod cairo axparameter libcurl jansson openssl fcgi
PKG_CONFIG ?= pkg-config

SRCS = $(wildcard src/*.c)
OBJS = $(SRCS:.c=.o)

# Set by the Dockerfile for the SDK 1.15 (AXIS OS 11.x) build
ifeq ($(AURIX_LEGACY_SDK),1)
CFLAGS += -DAURIX_LEGACY_SDK
endif

CFLAGS += -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter
CFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS)) -laxoverlay -lm -lpthread

all: $(PROG)

$(PROG): $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f $(PROG) $(OBJS) *.eap *LICENSE.txt package.conf* param.conf manifest.json

.PHONY: all clean
