CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
PREFIX ?= $(HOME)/.local

all: serve

serve: serve.c manager.c serve.h manager.h
	$(CC) $(CFLAGS) serve.c manager.c -o $@

install-user: serve
	install -Dm755 serve "$(PREFIX)/bin/serve"
	install -Dm644 systemd/serve@.service "$(HOME)/.config/systemd/user/serve@.service"
	systemctl --user daemon-reload

clean:
	rm -f serve

.PHONY: all install-user clean
