#
# You can un-comment this to build for Windows using MinGW-w64.
# Adjust the path to your MinGW toolchain as needed.  The final
# executable will be named cocofs.exe.
#
# CC=/usr/pkg/cross/x86_64-w64-mingw32/bin/x86_64-w64-mingw32-gcc

CFLAGS=	-O1 -g -Wall -Wextra -Wformat \
	-Wstrict-prototypes -Wmissing-prototypes \
	-Werror

CLEANFILES=	cocofs cocofs.exe

all: cocofs

cocofs: cocofs.o
	$(CC) -o cocofs cocofs.o

clean:
	-rm -f $(CLEANFILES) *.o *.core
