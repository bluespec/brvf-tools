# Makefile to create an elf_to_hex executable.
# The executable creates mem-hex files containing 32-Byte words

CC = gcc

# This macro is used inside elf-to-hex to figure out the mem's max addr
MEMSIZE ?= 16M
BASE_ADDR ?= 0x80000000lu
MSZ_MACRO = MEM_SIZE_$(MEMSIZE)


.PHONY: elf_to_hex
elf_to_hex: elf_to_hex.c
	gcc -D$(MSZ_MACRO) -DBASE_ADDR_B=$(BASE_ADDR) -g  -o elf_to_hex  elf_to_hex.c  -lelf

# ================================================================

.PHONY: clean
clean:
	rm  -f  *~

.PHONY: full_clean
full_clean:
	rm  -f  *~  elf_to_hex
