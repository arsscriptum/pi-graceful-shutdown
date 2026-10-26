CC      = cc
CFLAGS  = -ansi -pedantic -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200112L
LDFLAGS =

PREFIX     = /usr/local
SBINDIR    = $(PREFIX)/sbin
CONFDIR    = /etc/iboot-agent
UNITDIR    = /etc/systemd/system

AGENT      = iboot-agent
TEST       = test_dxp

.PHONY: all clean test install

all: $(AGENT) $(TEST)

$(AGENT): iboot-agent.c dxp.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ iboot-agent.c

$(TEST): test_dxp.c dxp.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ test_dxp.c

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(AGENT) $(TEST)

install: $(AGENT)
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 $(AGENT) $(DESTDIR)$(SBINDIR)/$(AGENT)
	install -d $(DESTDIR)$(CONFDIR)
	test -f $(DESTDIR)$(CONFDIR)/agent.conf || \
		install -m 0600 agent.conf.example $(DESTDIR)$(CONFDIR)/agent.conf
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 iboot-agent.service $(DESTDIR)$(UNITDIR)/iboot-agent.service
