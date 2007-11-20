include version.mk

CC = gcc
CFLAGS = -O0 -g -DPOISON -DDEBUG_POOL_REF -DSPLICE
LDFLAGS =

MACHINE := $(shell uname -m)
ifeq ($(MACHINE),x86_64)
ARCH_CFLAGS = -march=athlon64
else ifeq ($(MACHINE),i686)
ARCH_CFLAGS = -march=pentium4
else
ARCH_CFLAGS =
endif

ifeq ($(O),1)
CFLAGS = -O3 -g -DNDEBUG -DSPLICE
endif

ifeq ($(PROFILE),1)
CFLAGS = -O3 -g -DNDEBUG -DSPLICE -DPROFILE -pg
LDFLAGS = -lc_p -pg
endif

WARNING_CFLAGS = -Wall -W -pedantic -Werror -pedantic-errors -std=gnu99 -Wmissing-prototypes -Wwrite-strings -Wcast-qual -Wfloat-equal -Wshadow -Wpointer-arith -Wbad-function-cast -Wsign-compare -Waggregate-return -Wmissing-declarations -Wmissing-noreturn -Wmissing-format-attribute -Wredundant-decls -Wnested-externs -Winline -Wdisabled-optimization -Wno-long-long -Wstrict-prototypes -Wundef

ifeq ($(ICC),1)
CC = icc
ARCH_CFLAGS = -march=pentium4
WARNING_CFLAGS = -std=gnu99 -x c -Wall -Werror -wd981
endif

MORE_CFLAGS = -DVERSION=\"$(VERSION)\" -Iinclude

ALL_CFLAGS = $(CFLAGS) $(ARCH_CFLAGS) $(MORE_CFLAGS) $(WARNING_CFLAGS) 

LIBDAEMON_CFLAGS := $(shell pkg-config --cflags libcm4all-daemon)
LIBDAEMON_LIBS := $(shell pkg-config --libs libcm4all-daemon)

LIBEVENT_CFLAGS =
LIBEVENT_LIBS = -L/usr/local/lib -levent

LIBATTR_CFLAGS =
LIBATTR_LIBS = -lattr

SOURCES = src/main.c \
	src/cmdline.c \
	src/child.c \
	src/session.c \
	src/cookie.c \
	src/connection.c \
	src/translate.c \
	src/request.c \
	src/response.c \
	src/handler.c \
	src/file-handler.c \
	src/proxy-handler.c \
	src/replace.c \
	src/widget.c \
	src/widget-class.c \
	src/widget-ref.c \
	src/widget-session.c \
	src/widget-uri.c \
	src/proxy-widget.c \
	src/html-escape.c \
	src/js-filter.c \
	src/processor.c \
	src/penv.c \
	src/parser.c \
	src/embed.c \
	src/wembed.c \
	src/frame.c \
	src/socket-util.c \
	src/address.c \
	src/listener.c \
	src/client-socket.c \
	src/buffered-io.c \
	src/header-parser.c \
	src/header-writer.c \
	src/http.c \
	src/http-body.c \
	src/http-server.c \
	src/http-client.c \
	src/http-util.c \
	src/url-stream.c \
	src/filter.c \
	src/fifo-buffer.c \
	src/growing-buffer.c \
	src/duplex.c \
	src/istream-memory.c \
	src/istream-null.c \
	src/istream-string.c \
	src/istream-file.c \
	src/istream-chunked.c \
	src/istream-dechunk.c \
	src/istream-cat.c \
	src/istream-pipe.c \
	src/istream-delayed.c \
	src/istream-hold.c \
	src/istream-deflate.c \
	src/istream-subst.c \
	src/istream-byte.c \
	src/istream-fail.c \
	src/istream-head.c \
	src/uri.c \
	src/args.c \
	src/gmtime.c \
	src/date.c \
	src/strutil.c \
	src/format.c \
	src/hashmap.c \
	src/strmap.c \
	src/pstring.c \
	src/pool.c

HEADERS = $(wildcard src/*.h) $(wildcard include/beng-proxy/*.h)

OBJECTS = $(patsubst %.c,%.o,$(SOURCES))

DEBUG_ARGS = -vvvvvD

.PHONY: all clean

all: src/cm4all-beng-proxy

clean:
	rm -f src/cm4all-beng-proxy src/*.o doc/beng.{log,aux,ps,pdf,html} vgcore* core* gmon.out test/*.o test/benchmark-gmtime test/format-http-date test/request-translation test/js test/t-istream-chunked test/t-istream-dechunk test/t-html-unescape test/t-html-unescape test/t-http-server-mirror

src/cm4all-beng-proxy: $(OBJECTS)
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBEVENT_LIBS) $(LIBDAEMON_LIBS) $(LIBATTR_LIBS) -lz

$(OBJECTS): %.o: %.c $(HEADERS)
	$(CC) -c -o $@ $< $(ALL_CFLAGS) $(LIBEVENT_CFLAGS) $(LIBDAEMON_CFLAGS) $(LIBATTR_CFLAGS)

test/%.o: test/%.c $(HEADERS) $(wildcard test/*.h)
	$(CC) -c -o $@ $< $(ALL_CFLAGS) $(LIBEVENT_CFLAGS) $(LIBDAEMON_CFLAGS) $(LIBATTR_CFLAGS) -Isrc

test/benchmark-gmtime: test/benchmark-gmtime.o src/gmtime.o test/libcore-gmtime.o
	$(CC) -o $@ $^ $(LDFLAGS)

test/format-http-date: test/format-http-date.o src/gmtime.o src/date.o
	$(CC) -o $@ $^ $(LDFLAGS)

test/request-translation: test/request-translation.o src/translate.o src/pool.o src/growing-buffer.o src/socket-util.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBEVENT_LIBS) $(LIBDAEMON_LIBS)

test/js: test/js.o src/js-filter.o src/pool.o src/istream-file.o src/fifo-buffer.o src/buffered-io.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBDAEMON_LIBS)

test/t-parser-cdata: test/t-parser-cdata.o src/parser.o src/istream-file.o src/pool.o src/fifo-buffer.o src/buffered-io.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBDAEMON_LIBS)

test/t-html-unescape: test/t-html-unescape.o src/html-escape.o
	$(CC) -o $@ $^ $(LDFLAGS)

test/t-html-escape: test/t-html-escape.o src/html-escape.o
	$(CC) -o $@ $^ $(LDFLAGS)

test/t-http-server-mirror: test/t-http-server-mirror.o src/http-server.o src/fifo-buffer.o src/duplex.o src/pool.o src/pstring.o src/buffered-io.o src/strmap.o src/header-writer.o src/istream-dechunk.o src/istream-chunked.o src/istream-pipe.o src/istream-memory.o src/istream-cat.o src/http-body.o src/date.o src/socket-util.o src/growing-buffer.o src/http.o src/header-parser.o src/format.o src/strutil.o src/gmtime.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBEVENT_LIBS) $(LIBDAEMON_LIBS)

FILTER_TEST_CLASSES = cat chunked dechunk pipe hold delayed subst deflate
FILTER_TESTS = $(patsubst %,test/t-istream-%,$(FILTER_TEST_CLASSES))

$(FILTER_TESTS): test/t-istream-%: test/t-istream-%.o src/pool.o src/istream-memory.o src/istream-string.o src/istream-byte.o src/istream-fail.o src/istream-head.o src/istream-cat.o src/istream-%.o src/fifo-buffer.o src/format.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LIBDAEMON_LIBS) -lz

$(patsubst %,check-filter-%,$(FILTER_TEST_CLASSES)): check-filter-%: test/t-istream-%
	exec $<

check: $(patsubst %,check-filter-%,$(FILTER_TEST_CLASSES))

debug: src/cm4all-beng-proxy
	rm -f /tmp/cm4all-beng-proxy.gdb
	echo -en "handle SIGPIPE noprint nostop\nrun $(DEBUG_ARGS)\n" >/tmp/cm4all-beng-proxy.gdb
	LD_LIBRARY_PATH=/usr/lib/debug:$(LD_LIBRARY_PATH) gdb -x /tmp/cm4all-beng-proxy.gdb $<

profile: CFLAGS = -O3 -DNDEBUG -DSPLICE -DPROFILE -g -pg
profile: LDFLAGS = -lc_p -pg
profile: src/cm4all-beng-proxy
	./src/cm4all-beng-proxy -D -u max

# -DNO_DATE_HEADER -DNO_XATTR -DNO_LAST_MODIFIED_HEADER
benchmark: CFLAGS = -O3 -DNDEBUG -DALWAYS_INLINE
benchmark: src/cm4all-beng-proxy
	./src/cm4all-beng-proxy -D -u max -p 8080

valgrind: CFLAGS = -O0 -g -DPOISON -DVALGRIND
valgrind: src/cm4all-beng-proxy
	valgrind --show-reachable=yes --leak-check=yes ./src/cm4all-beng-proxy $(DEBUG_ARGS)

doc/beng.pdf: doc/beng.tex
	cd $(dir $<) && pdflatex $(notdir $<) && pdflatex $(notdir $<)

doc/beng.dvi: doc/beng.tex
	cd $(dir $<) && latex $(notdir $<)
