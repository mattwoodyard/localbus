


CFLAGS=-Wall -g -O0
LDFLAGS=-levent


localbus: localbus.c
	$(CC) -o localbus localbus.c $(CFLAGS) $(LDFLAGS)

