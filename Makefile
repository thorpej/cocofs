CFLAGS=	-O1 -g -Wall -Wextra -Wformat \
	-Wstrict-prototypes -Wmissing-prototypes \
	-Werror

CLEANFILES=	cocofs

all: cocofs

cocofs: cocofs.o
	$(CC) -o cocofs cocofs.o

clean:
	-rm -f $(CLEANFILES) *.o *.core
