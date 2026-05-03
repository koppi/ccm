CC = gcc
CFLAGS = -Wall -Wextra -Werror -pthread -O2
LDFLAGS = -lncursesw
TARGET = ccm
SOURCES = ccm.c
PREFIX ?= /usr

.PHONY: all clean install run

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
