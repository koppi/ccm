CC ?= gcc

PREFIX ?= /usr
MANDIR ?= $(PREFIX)/share/man

CFLAGS   = -Wall -Wextra -Werror -pthread -O3
CFLAGS  += $(shell pkg-config --cflags ncursesw)
LDFLAGS += $(shell pkg-config --libs ncursesw)

TARGET = ccm

SOURCES = ccm.c

.PHONY: all clean install uninstall run

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(MANDIR)/man1
	install $(TARGET) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 ccm.1 $(DESTDIR)$(MANDIR)/man1/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(MANDIR)/man1/$(TARGET).1

run: $(TARGET)
	./$(TARGET)
