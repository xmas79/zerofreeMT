CC=gcc

all: zerofree.c
	$(CC) $(CFLAGS) -o zerofree zerofree.c -lext2fs -lpthread

clean:
	/bin/rm -f *.o *~ zerofree
