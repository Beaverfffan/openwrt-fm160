# SPDX-License-Identifier: GPL-2.0-or-later
#
# Plain Makefile used by the OpenWrt package (see ../Makefile, Build/Compile).
# Kept deliberately dependency-free: no autotools, no configure step.

CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall
LDFLAGS ?=

BIN      = fm160d
OBJS     = atq.o cmds.o diag.o dialer.o main.o modesw.o net.o pdu.o sched.o \
           sms.o state.o ubus_methods.o usbmode.o
LIBS     = -lubus -lubox -lblobmsg_json

all: $(BIN)

# Link order matters: objects must precede $(LDFLAGS)/$(LIBS) so that
# -Wl,--as-needed (used by the OpenWrt toolchain) does not drop the libraries
# before it has seen the references to them.
$(BIN): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS) $(LIBS)

%.o: %.c fm160d.h
	$(CC) $(CFLAGS) -c -o $@ $<

# Three sources deliberately do not include fm160d.h: they are pure
# transformation with no modem, no ubus and no global state, which is what lets
# the host-side tests compile them on their own.
#   pdu.c     23-sms-pdu-test.sh
#   usbmode.c / net.c   25-usbmode-dial-test.sh
#
# diag.c does include it, for struct fm160_state and struct fm160_log_rec, and
# is still driven by a host-side test (27-diag-export-test.sh) - it is pure in
# the way that matters (no I/O, no globals, the clock is an argument).  What it
# needs from fm160d.h is only the two struct definitions; the libubox headers
# that come with them are already staged for tools/cccheck, so the test compiles
# against the real struct rather than a copy of it.  A renamed field breaks the
# test loudly instead of leaving it asserting against a stale duplicate.
pdu.o: pdu.c pdu.h
usbmode.o: usbmode.c usbmode.h
net.o: net.c net.h usbmode.h
diag.o: diag.c diag.h fm160d.h

clean:
	rm -f $(OBJS) $(BIN)

.PHONY: all clean
