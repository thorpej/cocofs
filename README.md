# cocofs - A tool for interacting with TRS-80 Color Computer disk images

cocofs is a simple tool for interacting with TRS-80 Color Computer disk images that are
formatted with the file system supported by Disk Extended Color Basic.  This file system
is very simple and closely resembles the FAT file system used in MS-DOS.  Information
about the file system format can be found [here](http://dragon32.info/info/tandydsk.html),
but here are the basics:

- single-sided (1 track-per-cylinder), double-density
- 35 cylinders (numbered 0 - 34)
- 18 sectors-per-track (numbered 1 - 18)
- 256 bytes-per-sector

Each track is broken up into 2 "granules" of 9 sectors.  The granule is the file system's
allocation unit.  With 35 tracks, there are a total of 70 granules.  The directory is
stored in track 17, leaving 68 granules available for file data.

cocofs currently supports simple linear disk images that are 161280 bytes in size (35 * 18 * 256).
These files often have the *.DSK* file name extension.

cocofs can perform the following operations:

- format -- create a new disk image
- ls *[file1 [file2 [...]]]* -- list the directory or specific files
- copyin *file1 [file2 [...]]* -- copy files into the disk image
- copyout *file1 [file2 [...]]* -- copy files out of the disk image
- rm *file1 [file2 [...]]* -- remove files from the disk image
- dump -- dump information about the disk image, file allocation, etc.

This tool isn't super advanced, and could still be improved, but it is useful in its current
form.  Some ideas for future enhancements:

- fsck functionality -- the ability to check and repair CoCo file systems
- more advanced command parsing for copyin and copyout.
- rename functionality -- the ability to rename files in the file system
- support for more disk image formats

cocofs should be extremely portable to any Unix-like system with a C99 compiler, and can also be
built for Windows using MinGW-w64 (see [Makefile](Makefile) for details).  Please let me know if
you have issues building it on your system.

If you're intrested in contributing, please do!  In any case, I hope you find this tool useful!
