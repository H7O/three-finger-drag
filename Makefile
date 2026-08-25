# three-finger-drag — no dependencies beyond libc and the kernel uapi headers.

PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
UNITDIR    ?= /etc/systemd/system

CC         ?= cc
CFLAGS     ?= -O2 -g
CFLAGS     += -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
LDFLAGS    ?=

BIN         = three-finger-drag
SRC         = three-finger-drag.c
UNIT        = systemd/three-finger-drag.service

.PHONY: all clean install uninstall enable disable

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 $(UNIT) $(DESTDIR)$(UNITDIR)/three-finger-drag.service

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(UNITDIR)/three-finger-drag.service

enable:
	systemctl daemon-reload
	systemctl enable --now three-finger-drag.service

disable:
	systemctl disable --now three-finger-drag.service
