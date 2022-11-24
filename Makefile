CC = gcc
CFLAGS = -Wall
INSTALL = `which install`

chwspi: chwspi.c
	$(CC) $(CFLAGS) chwspi.c -o chwspi

install: chwspi
	$(INSTALL) ./chwspi /usr/local/bin/chwspi

clean:
	rm -f chwspi
