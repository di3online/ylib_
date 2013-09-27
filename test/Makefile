CC:=gcc
CFLAGS:=-Wall -Wextra -Wno-unused-parameter -O3 -g

.PHONY: all test clean

all: test

yuri: ../yuri.c ../yuri.h yuri.c
	$(CC) $(CFLAGS) -I.. ../yuri.c yuri.c -o yuri

ecbuf: ../ecbuf.h ecbuf.c
	$(CC) $(CFLAGS) -I.. ecbuf.c -o ecbuf

evtp: ../evtp.c ../evtp.h evtp.c
	$(CC) $(CFLAGS) -I.. ../evtp.c evtp.c -lpthread -lev -o evtp

sqlasync: ../sqlasync.c ../sqlasync.h sqlasync.c
	$(CC) $(CFLAGS) -I.. ../sqlasync.c sqlasync.c -lrt -lpthread -lsqlite3 -o sqlasync

ylog: ../ylog.c ../ylog.h ylog.c
	$(CC) $(CFLAGS) -I.. ylog.c -o ylog

test: yuri ecbuf evtp sqlasync ylog
	./yuri
	./ecbuf
	./evtp
	./sqlasync
	./ylog
	@echo All tests passed.

ecbuf-bench: ../ecbuf.h ecbuf-bench.c
	$(CC) $(CFLAGS) -DNDEBUG -I.. ecbuf-bench.c -o ecbuf-bench

evtp-bench-plain: ../evtp.c ../evtp.h evtp.c
	$(CC) $(CFLAGS) -DBENCH -I.. ../evtp.c evtp.c -lpthread -lm -lev -o evtp-bench-plain

evtp-bench-work: ../evtp.c ../evtp.h evtp.c
	$(CC) $(CFLAGS) -DBENCH -DWORK -I.. ../evtp.c evtp.c -lpthread -lm -lev -o evtp-bench-work

bench: ecbuf-bench evtp-bench-plain evtp-bench-work
	@#./ecbuf-bench
	sh -c 'time ./evtp-bench-plain'
	sh -c 'time ./evtp-bench-work'

clean:
	rm -f yuri ecbuf evtp sqlasync ecbuf-bench evtp-benchp-plain evtp-bench-work
