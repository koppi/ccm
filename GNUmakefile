CC ?= gcc

PREFIX ?= /usr

CFLAGS = -Wall -Wextra -Werror -pthread -O3
LDFLAGS = -lncursesw
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
