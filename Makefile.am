AUTOMAKE_OPTIONS = subdir-objects
ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS} -I m4
AM_CFLAGS = -g -O3 -flto -Isrc

bin_PROGRAMS = \
bin/peek

PEEK_SOURCE_FILES = \
src/main.c \
src/peek.c \
src/rule.c \
src/data.c

PEEK_HEADER_FILES = \
src/peek.h \
src/rule.h \
src/data.h

bin_peek_SOURCES = $(PEEK_SOURCE_FILES) $(PEEK_HEADER_FILES)
bin_peek_LDFLAGS = -pthread
bin_peek_LDADD = -ldynamic -lreactor

MAINTAINERCLEANFILES = aclocal.m4 config.h.in configure Makefile.in
maintainer-clean-local:; rm -rf autotools m4

CLEANFILES = {.,src}/*.{gcno,gcda,gcov}

indent:
	clang-format -i $(PEEK_SOURCE_FILES)

.phony: indent
