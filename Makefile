CFLAGS=-O -D_FILE_OFFSET_BITS=64 -DUSE_FTELLO

BIN=torrentcheck
SRC=torrentcheck.c sha1.c

$(BIN) : $(SRC)
	gcc $(CFLAGS) $^ -o $@

.PHONY: clean

clean:
	rm -f $(BIN)
