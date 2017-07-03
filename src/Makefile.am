### Makefile.am
include $(top_builddir)/version.mk

AM_CFLAGS = $(EXTRA_CFLAGS)
AM_CPPFLAGS = -D_POSIX_C_SOURCE=201001L -D_XOPEN_SOURCE=700 -D_BSD_SOURCE
AM_CPPFLAGS += -D_HAVE_VERSION_H
AM_LDFLAGS = $(XCCLDFLAGS)

bin_PROGRAMS =
noinst_PROGRAMS =
noinst_LIBRARIES =
noinst_HEADERS =
BUILT_SOURCES =
EXTRA_DIST = $(BUILT_SOURCES)
CLEANFILES = 
SUFFIXES =

CLEANFILES += version.c
EXTRA_DIST += version.c.in

noinst_LIBRARIES += libdfp.a
libdfp_a_SOURCES =
libdfp_a_SOURCES += dfp754_d32.c dfp754_d32.h
libdfp_a_SOURCES += dfp754_d64.c dfp754_d64.h
libdfp_a_CPPFLAGS = $(AM_CPPFLAGS)
libdfp_a_CPPFLAGS += $(dfp754_CFLAGS)

noinst_LIBRARIES += liblol.a
liblol_a_SOURCES =
liblol_a_SOURCES += lol.c lol.h
liblol_a_SOURCES += bot.c bot.h
liblol_a_SOURCES += sock.c sock.h
liblol_a_CPPFLAGS = $(AM_CPPFLAGS)
liblol_a_CPPFLAGS += $(ev_CFLAGS)
liblol_a_CPPFLAGS += $(dfp754_CFLAGS)
liblol_a_CPPFLAGS += $(clob_CFLAGS)
liblol_a_LIBADD = libdfp.a
lol_CFLAGS = $(clob_CFLAGS) $(dfp754_CFLAGS)
lol_LIBS = $(dfp754_LIBS) $(ev_LIBS) libdfp.a


noinst_PROGRAMS += lobot
lobot_SOURCES = lobot.c lobot.yuck
lobot_SOURCES += sock.c sock.h
lobot_SOURCES += hash.c hash.h
lobot_CPPFLAGS = $(AM_CPPFLAGS)
lobot_CPPFLAGS += $(ev_CFLAGS)
lobot_CPPFLAGS += $(clob_CFLAGS)
lobot_CPPFLAGS += $(dfp754_CFLAGS)
lobot_LDFLAGS = $(AM_LDFLAGS)
lobot_LDFLAGS += $(ev_LIBS)
lobot_LDFLAGS += $(clob_LIBS)
lobot_LDFLAGS += $(dfp754_LIBS)
lobot_LDADD = libdfp.a liblol.a
BUILT_SOURCES += lobot.yucc

noinst_PROGRAMS += monkey
monkey_SOURCES = monkey.c monkey.yuck
monkey_SOURCES += pcg_basic.c pcg_basic.h
monkey_CPPFLAGS = $(AM_CPPFLAGS)
monkey_CPPFLAGS += $(lol_CFLAGS)
monkey_LDFLAGS = $(AM_LDFLAGS)
monkey_LDFLAGS += $(lol_LIBS)
monkey_LDADD = libdfp.a liblol.a
BUILT_SOURCES += monkey.yucc

noinst_PROGRAMS += egobot
egobot_SOURCES = egobot.c egobot.yuck
egobot_CPPFLAGS = $(AM_CPPFLAGS)
egobot_CPPFLAGS += $(lol_CFLAGS)
egobot_CPPFLAGS += -mkl
egobot_LDFLAGS = $(AM_LDFLAGS)
egobot_LDFLAGS += $(lol_LIBS)
egobot_LDFLAGS += -mkl
egobot_LDADD = libdfp.a liblol.a
BUILT_SOURCES += egobot.yucc

noinst_PROGRAMS += clampbot
clampbot_SOURCES = clampbot.c clampbot.yuck
clampbot_CPPFLAGS = $(AM_CPPFLAGS)
clampbot_CPPFLAGS += $(lol_CFLAGS)
clampbot_CPPFLAGS += $(clob_CFLAGS)
clampbot_LDFLAGS = $(AM_LDFLAGS)
clampbot_LDFLAGS += $(lol_LIBS)
clampbot_LDADD = libdfp.a liblol.a
BUILT_SOURCES += clampbot.yucc


## version rules
version.c: version.c.in $(top_builddir)/.version
	$(AM_V_GEN) PATH="$(top_builddir)/build-aux:$${PATH}" \
		yuck$(EXEEXT) scmver --ignore-noscm --force -o $@ \
			--use-reference --reference $(top_builddir)/.version $<

## yuck rule
SUFFIXES += .yuck
SUFFIXES += .yucc
.yuck.yucc:
	$(AM_V_GEN) PATH=$(top_builddir)/build-aux:"$${PATH}" \
		yuck$(EXEEXT) gen -o $@ $<

## Create preprocessor output (debugging purposes only)
.c.i:
	$(COMPILE) -E -o $@ $<

## Create assembler output (debugging purposes only)
.c.s:
	$(COMPILE) -S -c $(AM_CFLAGS) $<
