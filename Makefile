CFLAGS=		-O1 -g -Wall

CLEANFILES=	cocofs

all: cocofs

cocofs: cocofs.o
	$(CC) -o cocofs cocofs.o

clean:
	-rm -f $(CLEANFILES) *.o *.core
