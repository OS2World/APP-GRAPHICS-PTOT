REM Commandfile for compiling PTOT.EXE
REM Needs ICC.EXE (IBM C-Compiler)
REM Tested with VAC++ V3.0
REM
REM This file done by Wolfram M. Koerner
REM koerner@usa.net or koerner@bigfoot.com
REM Wuerzburg, Germany 21.10.1997

icc /c inflate.c
icc /c crc32.c
icc /c tiff.c
icc /c tempfile.c
icc /c zchunks.c
icc /c ppm.c

icc /D_PNG2PPM_ ptot.c zchunks.obj tempfile.obj tiff.obj crc32.obj inflate.obj ppm.obj

del *.obj

copy ptot.exe png2ppm.exe
