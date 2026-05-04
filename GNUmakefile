CC ?= gcc

PREFIX ?= /usr

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
	install $(TARGET) $(DESTDIR)$(PREFIX)/bin/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

run: $(TARGET)
	./$(TARGET)
