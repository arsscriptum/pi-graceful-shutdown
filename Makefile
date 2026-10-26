CC      = cc
CFLAGS  = -ansi -pedantic -Wall -Wextra -O2 \
          -D_POSIX_C_SOURCE=200112L -D_DEFAULT_SOURCE
LDFLAGS =

PREFIX     = /usr/local
SBINDIR    = $(PREFIX)/sbin
CONFDIR    = /etc/iboot-agent
UNITDIR    = /etc/systemd/system

SRCDIR     = src
TESTDIR    = test

AGENT      = iboot-agent
TEST       = dxp_decode_test

SRCS       = $(SRCDIR)/main.c \
             $(SRCDIR)/config.c \
             $(SRCDIR)/dxp.c \
             $(SRCDIR)/halt.c \
             $(SRCDIR)/log.c \
             $(SRCDIR)/session.c \
             $(SRCDIR)/signal_source.c

OBJS       = $(SRCS:.c=.o)

TEST_SRCS  = $(TESTDIR)/dxp_decode_test.c $(SRCDIR)/dxp.c
TEST_OBJS  = $(TEST_SRCS:.c=.o)

.PHONY: all clean test install

all: $(AGENT) $(TEST)

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(TESTDIR)/%.o: $(TESTDIR)/%.c
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

$(AGENT): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(TEST): $(TEST_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJS)

test: $(TEST)
	./$(TEST)

clean:
	rm -f $(AGENT) $(TEST) $(SRCDIR)/*.o $(TESTDIR)/*.o

install: $(AGENT)
	install -d $(DESTDIR)$(SBINDIR)
	install -m 0755 $(AGENT) $(DESTDIR)$(SBINDIR)/$(AGENT)
	install -d $(DESTDIR)$(CONFDIR)
	test -f $(DESTDIR)$(CONFDIR)/agent.conf || \
		install -m 0600 agent.conf.example $(DESTDIR)$(CONFDIR)/agent.conf
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 iboot-agent.service $(DESTDIR)$(UNITDIR)/iboot-agent.service
