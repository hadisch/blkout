# Compiler und Flags
CC      = gcc
CFLAGS  = -Wall -Wextra -I/usr/include -Iprotocols
LDFLAGS = -lwayland-client -lrt

# Zieldatei
TARGET  = blkout

# Quell- und Objektdateien
SRCS    = src/main.c \
          src/xdg-popup-stub.c \
          protocols/wlr-layer-shell-unstable-v1.c \
          protocols/ext-idle-notify-v1.c
OBJS    = $(SRCS:.c=.o)

# Generierte Protocol-Dateien
PROTO_HEADERS = \
    protocols/wlr-layer-shell-unstable-v1-client-protocol.h \
    protocols/ext-idle-notify-v1-client-protocol.h
PROTO_SRCS = \
    protocols/wlr-layer-shell-unstable-v1.c \
    protocols/ext-idle-notify-v1.c

.PHONY: all clean install

# Standardziel: erst Protocol-Dateien generieren, dann linken
all: $(PROTO_HEADERS) $(PROTO_SRCS) $(TARGET)

# Protocol-Header aus XML generieren
protocols/%-client-protocol.h: protocols/%.xml
	wayland-scanner client-header $< $@

# Protocol-C-Quellcode aus XML generieren
protocols/%.c: protocols/%.xml
	wayland-scanner private-code $< $@

# Hauptprogramm linken
$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

# Objektdateien compilieren; main.o hängt von den generierten Headern ab
src/main.o: src/main.c $(PROTO_HEADERS)
	$(CC) $(CFLAGS) -c -o $@ $<

protocols/%.o: protocols/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Aufräumen: generierte und compilierte Dateien entfernen
clean:
	rm -f $(TARGET) $(OBJS) $(PROTO_HEADERS) $(PROTO_SRCS)

# Installation
install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	install -dm755 /usr/local/share/$(TARGET)
