Copyright (c) 2018 Bluespec, Inc. All Rights Reserved

This standalone C program takes two command-line arguments, and ELF
filename and a Mem Hex filename.  It reads the ELF file and writes out
the Mem Hex file.

The makefile takes two variables:

   - MEMSIZE: 16/32/64/128/256/512/1024 KB, 16/256 MB 
   - BASE_ADDR: 32-bit start of memory

The memory is organized in blocks of 32-bit words.
