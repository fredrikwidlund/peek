AUTOMAKE_OPTIONS	= subdir-objects
ACLOCAL_AMFLAGS 	= ${ACLOCAL_FLAGS} -I m4
AM_CFLAGS	 	= -g -O3 -std=gnu2x

POKE_SOURCE_FILES 	= \
src/main.c \
src/poke.c

POKE_HEADER_FILES 	= \
src/poke.h \
src/value.h

bin_PROGRAMS 		= poke
poke_SOURCES 		= $(POKE_SOURCE_FILES) $(POKE_HEADER_FILES)
poke_LDADD 		= -ljansson

MAINTAINERCLEANFILES 	= aclocal.m4 config.h.in configure Makefile.in

maintainer-clean-local:; rm -rf autotools m4

CLEANFILES = {.,src}/*.{gcno,gcda,gcov}

indent:
	clang-format -i src/*.c

.phony: indent
