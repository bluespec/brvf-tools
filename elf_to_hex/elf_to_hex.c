// Copyright (c) 2013-2018 Bluespec, Inc. All Rights Reserved

// This program reads an ELF file and outputs a Verilog hex memory
// image file (suitable for reading using $readmemh).

// ================================================================
// Standard C includes

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <gelf.h>

// ================================================================
// Memory buffer into which we load the ELF file before
// writing it back out to the output file.

// 1 Gigabyte sized elf files
// #define MAX_MEM_SIZE (((uint64_t) 0x400) * ((uint64_t) 0x400) * ((uint64_t) 0x400))
//
#define MAX_MEM_SIZE ((uint64_t) 0xD0000000)

uint8_t mem_buf [MAX_MEM_SIZE];

// Features of the ELF binary
int       bitwidth;
uint64_t  min_addr;
uint64_t  max_addr;

uint64_t  pc_start;       // Addr of label  '_start'
uint64_t  pc_exit;        // Addr of label  'exit'
uint64_t  tohost_addr;    // Addr of label  'tohost'

// ================================================================
// Load an ELF file.

void c_mem_load_elf (char *elf_filename,
		     const char *start_symbol,
		     const char *exit_symbol,
		     const char *tohost_symbol)
{
    int fd;
    // int n_initialized = 0;
    Elf *e;

    // Default start, exit and tohost symbols
    if (start_symbol == NULL)
	start_symbol = "_start";
    if (exit_symbol == NULL)
	exit_symbol = "exit";
    if (tohost_symbol == NULL)
	tohost_symbol = "tohost";
    
    // Verify the elf library version
    if (elf_version (EV_CURRENT) == EV_NONE) {
        fprintf (stderr, "ERROR: c_mem_load_elf: Failed to initialize the libelfg library!\n");
	exit (1);
    }

    // Open the file for reading
    fd = open (elf_filename, O_RDONLY, 0);
    if (fd < 0) {
        fprintf (stderr, "ERROR: c_mem_load_elf: could not open elf input file: %s\n", elf_filename);
	exit (1);
    }

    // Initialize the Elf pointer with the open file
    e = elf_begin (fd, ELF_C_READ, NULL);
    if (e == NULL) {
        fprintf (stderr, "ERROR: c_mem_load_elf: elf_begin() initialization failed!\n");
	exit (1);
    }

    // Verify that the file is an ELF file
    if (elf_kind (e) != ELF_K_ELF) {
        elf_end (e);
        fprintf (stderr, "ERROR: c_mem_load_elf: specified file '%s' is not an ELF file!\n", elf_filename);
	exit (1);
    }

    // Get the ELF header
    GElf_Ehdr ehdr;
    if (gelf_getehdr (e, & ehdr) == NULL) {
        elf_end (e);
        fprintf (stderr, "ERROR: c_mem_load_elf: get_getehdr() failed: %s\n", elf_errmsg(-1));
	exit (1);
    }

    // Is this a 32b or 64 ELF?
    if (gelf_getclass (e) == ELFCLASS32) {
	fprintf (stdout, "c_mem_load_elf: %s is a 32-bit ELF file\n", elf_filename);
	bitwidth = 32;
    }
    else if (gelf_getclass (e) == ELFCLASS64) {
	fprintf (stdout, "c_mem_load_elf: %s is a 64-bit ELF file\n", elf_filename);
	bitwidth = 64;
    }
    else {
        fprintf (stderr, "ERROR: c_mem_load_elf: ELF file '%s' is not 32b or 64b\n", elf_filename);
	elf_end (e);
	exit (1);
    }

    // Verify we are dealing with a RISC-V ELF
    if (ehdr.e_machine != 243) { // EM_RISCV is not defined, but this returns 243 when used with a valid elf file.
        elf_end (e);
        fprintf (stderr, "ERROR: c_mem_load_elf: %s is not a RISC-V ELF file\n", elf_filename);
	exit (1);
    }

    // Verify we are dealing with a little endian ELF
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        elf_end (e);
        fprintf (stderr,
		 "ERROR: c_mem_load_elf: %s is a big-endian 64-bit RISC-V executable which is not supported\n",
		 elf_filename);
	exit (1);
    }

    // Grab the string section index
    size_t shstrndx;
    shstrndx = ehdr.e_shstrndx;

    // Iterate through each of the sections looking for code that should be loaded
    Elf_Scn  *scn   = 0;
    GElf_Shdr shdr;

    min_addr    = 0xFFFFFFFFFFFFFFFFllu;
    max_addr    = 0x0000000000000000llu;
    pc_start    = 0xFFFFFFFFFFFFFFFFllu;
    pc_exit     = 0xFFFFFFFFFFFFFFFFllu;
    tohost_addr = 0xFFFFFFFFFFFFFFFFllu;

    while ((scn = elf_nextscn (e,scn)) != NULL) {
        // get the header information for this section
        gelf_getshdr (scn, & shdr);

	char *sec_name = elf_strptr (e, shstrndx, shdr.sh_name);
	fprintf (stdout, "Section %-16s: ", sec_name);

	Elf_Data *data = 0;
	// If we find a code/data section, load it into the model
	if (   ((shdr.sh_type == SHT_PROGBITS)
		|| (shdr.sh_type == SHT_NOBITS)
		|| (shdr.sh_type == SHT_INIT_ARRAY)
		|| (shdr.sh_type == SHT_FINI_ARRAY))
	    && ((shdr.sh_flags & SHF_WRITE)
		|| (shdr.sh_flags & SHF_ALLOC)
		|| (shdr.sh_flags & SHF_EXECINSTR))) {
	    data = elf_getdata (scn, data);

	    // n_initialized += data->d_size;
	    if (shdr.sh_addr < min_addr)
		min_addr = shdr.sh_addr;
	    if (max_addr < (shdr.sh_addr + data->d_size - 1))   // shdr.sh_size + 4))
		max_addr = shdr.sh_addr + data->d_size - 1;    // shdr.sh_size + 4;

	    if (max_addr >= MAX_MEM_SIZE) {
		fprintf (stdout, "INTERNAL ERROR: max_addr (0x%0" PRIx64 ") > buffer size (0x%0" PRIx64 ")\n",
			 max_addr, MAX_MEM_SIZE);
		fprintf (stdout, "    Please increase the #define in this program, recompile, and run again\n");
		fprintf (stdout, "    Abandoning this run\n");
		exit (1);
	    }

	    if (shdr.sh_type != SHT_NOBITS) {
		memcpy (& (mem_buf [shdr.sh_addr]), data->d_buf, data->d_size);
	    }
	    fprintf (stdout, "addr %16" PRIx64 " to addr %16" PRIx64 "; size 0x%8lx (= %0ld) bytes\n",
		     shdr.sh_addr, shdr.sh_addr + data->d_size, data->d_size, data->d_size);

	}

	// If we find the symbol table, search for symbols of interest
	else if (shdr.sh_type == SHT_SYMTAB) {
	    fprintf (stdout, "Searching for addresses of '%s', '%s' and '%s' symbols\n",
		     start_symbol, exit_symbol, tohost_symbol);

 	    // Get the section data
	    data = elf_getdata (scn, data);

	    // Get the number of symbols in this section
	    int symbols = shdr.sh_size / shdr.sh_entsize;

	    // search for the uart_default symbols we need to potentially modify.
	    GElf_Sym sym;
	    int i;
	    for (i = 0; i < symbols; ++i) {
	        // get the symbol data
	        gelf_getsym (data, i, &sym);

		// get the name of the symbol
		char *name = elf_strptr (e, shdr.sh_link, sym.st_name);

		// Look for, and remember PC of the start symbol
		if (strcmp (name, start_symbol) == 0) {
		    pc_start = sym.st_value;
		}
		// Look for, and remember PC of the exit symbol
		else if (strcmp (name, exit_symbol) == 0) {
		    pc_exit = sym.st_value;
		}
		// Look for, and remember addr of 'tohost' symbol
		else if (strcmp (name, tohost_symbol) == 0) {
		    tohost_addr = sym.st_value;
		}
	    }

	    FILE *fp_symbol_table = fopen ("symbol_table.txt", "w");
	    if (fp_symbol_table != NULL) {
		fprintf (stdout, "Writing symbols to:    symbol_table.txt\n");
		if (pc_start == -1)
		    fprintf (stdout, "    No '_start' label found\n");
		else
		    fprintf (fp_symbol_table, "_start    0x%0" PRIx64 "\n", pc_start);

		if (pc_exit == -1)
		    fprintf (stdout, "    No 'exit' label found\n");
		else
		    fprintf (fp_symbol_table, "exit      0x%0" PRIx64 "\n", pc_exit);

		if (tohost_addr == -1)
		    fprintf (stdout, "    No 'tohost' symbol found\n");
		else
		    fprintf (fp_symbol_table, "tohost    0x%0" PRIx64 "\n", tohost_addr);

		fclose (fp_symbol_table);
	    }
	}
	else {
	    fprintf (stdout, "Ignored\n");
	}
    }

    elf_end (e);

    fprintf (stdout, "Min addr:            %16" PRIx64 " (hex)\n", min_addr);
    fprintf (stdout, "Max addr:            %16" PRIx64 " (hex)\n", max_addr);
}

// ================================================================
// Min and max byte addrs for various mem sizes

// #define BASE_ADDR_B  0x80000000lu
/*
// For 16 KB memory at 0x_8000_0000
#define MIN_MEM_ADDR_16KB BASE_ADDR_B
#define MAX_MEM_ADDR_16KB  (BASE_ADDR_B +    0x4000lu)

// For 64 KB memory at 0x_8000_0000
#define MIN_MEM_ADDR_32KB BASE_ADDR_B
#define MAX_MEM_ADDR_32KB  (BASE_ADDR_B +    0x8000lu)

// For 64 KB memory at 0x_8000_0000
#define MIN_MEM_ADDR_64KB BASE_ADDR_B
#define MAX_MEM_ADDR_64KB  (BASE_ADDR_B +    0x10000lu)

// For 128 KB memory at 0x_8000_0000
#define MIN_MEM_ADDR_128KB BASE_ADDR_B
#define MAX_MEM_ADDR_128KB (BASE_ADDR_B +    0x20000lu)

// For 256 KB memory at 0x_8000_0000
#define MIN_MEM_ADDR_256KB BASE_ADDR_B
#define MAX_MEM_ADDR_256KB (BASE_ADDR_B +    0x40000lu)

// For 512 KB memory at 0x_8000_0000
#define MIN_MEM_ADDR_512KB BASE_ADDR_B
#define MAX_MEM_ADDR_512KB (BASE_ADDR_B +    0x80000lu)

// For 1 MB memory at 0x_8000_0000
#define MIN_MEM_ADDR_1024KB  BASE_ADDR_B
#define MAX_MEM_ADDR_1024KB  (BASE_ADDR_B + 0x100000lu)

// For 16 MB memory at 0x_8000_0000
#define MIN_MEM_ADDR_16MB  BASE_ADDR_B
#define MAX_MEM_ADDR_16MB  (BASE_ADDR_B +  0x1000000lu)

// For 256 MB memory at 0x_8000_0000
#define MIN_MEM_ADDR_256MB  BASE_ADDR_B
#define MAX_MEM_ADDR_256MB (BASE_ADDR_B + 0x10000000lu)

// What is the maximum memory address? Use a preset size value.
#ifdef MEM_SIZE_16K
#define MAX_MEM_ADDR MAX_MEM_ADDR_16KB
#endif

#ifdef MEM_SIZE_32K
#define MAX_MEM_ADDR MAX_MEM_ADDR_32KB
#endif

#ifdef MEM_SIZE_64K
#define MAX_MEM_ADDR MAX_MEM_ADDR_64KB
#endif

#ifdef MEM_SIZE_128K
#define MAX_MEM_ADDR MAX_MEM_ADDR_128KB
#endif

#ifdef MEM_SIZE_256K
#define MAX_MEM_ADDR MAX_MEM_ADDR_256KB
#endif

#ifdef MEM_SIZE_512K
#define MAX_MEM_ADDR MAX_MEM_ADDR_512KB
#endif

#ifdef MEM_SIZE_1024K
#define MAX_MEM_ADDR MAX_MEM_ADDR_1024KB
#endif

#ifdef MEM_SIZE_16M
#define MAX_MEM_ADDR MAX_MEM_ADDR_16MB
#endif

#ifdef MEM_SIZE_256M
#define MAX_MEM_ADDR MAX_MEM_ADDR_256MB
#endif
*/

// ================================================================

// Write out from word containing base_addr to word containing addr2
void write_mem_hex_file (
      FILE *fp, uint64_t base_addr, uint64_t addr2, uint64_t max_addr)
{
    const uint64_t bits_per_raw_mem_word   = 32;
    uint64_t bytes_per_raw_mem_word  = bits_per_raw_mem_word / 8;    // 32
    uint64_t raw_mem_word_align_mask = (~ ((uint64_t) (bytes_per_raw_mem_word - 1)));

    fprintf (stdout, "Subtracting 0x%08" PRIx64 " base from addresses\n", base_addr);

    // Align the start and end addrs to raw mem words
    uint64_t a1 = (base_addr & raw_mem_word_align_mask);
    uint64_t a2 = ((addr2 + bytes_per_raw_mem_word - 1) & raw_mem_word_align_mask);

    fprintf (fp, "@%07" PRIx64 "    // raw_mem addr;  byte addr: %08" PRIx64 "\n",
	     ((a1 - base_addr) / bytes_per_raw_mem_word),
	     a1 - base_addr);
	     
    uint64_t addr;
    for (addr = a1; addr < a2; addr += bytes_per_raw_mem_word) {
	for (int j = (bytes_per_raw_mem_word - 1); j >= 0; j--)
	    fprintf (fp, "%02x", mem_buf [addr+j]);
	fprintf (fp, "    // raw_mem addr %08" PRIx64 ";  byte addr %08" PRIx64 "\n",
		 ((addr - base_addr) / bytes_per_raw_mem_word),
		 (addr  - base_addr));
    }

    // Write last word, if necessary, to avoid warnings about missing locations
    if (addr < (max_addr - bytes_per_raw_mem_word)) {
	addr = max_addr - bytes_per_raw_mem_word;
	fprintf (fp, "@%07" PRIx64 "    // last raw_mem addr;  byte addr: %08" PRIx64 "\n",
		 ((addr - base_addr) / bytes_per_raw_mem_word),
		 addr - base_addr);
	for (int j = (bytes_per_raw_mem_word - 1); j >= 0; j--)
	    fprintf (fp, "%02x", 0);
	fprintf (fp, "    // raw_mem addr %08" PRIx64 ";  byte addr %08" PRIx64 "\n",
		 ((addr - base_addr) / bytes_per_raw_mem_word),
		 (addr  - base_addr));
    }
}

// ================================================================

void print_usage (FILE *fp, char *ap)
{
    fprintf (fp, "Usage:\n");
    fprintf (fp, "    %s : \n", ap);
    fprintf (fp, "       -e <ELF filename> \n");
    fprintf (fp, "       -h <mem hex filename> \n");
    fprintf (fp, "       -m <mem size in KB> <default: 16> \n");
    fprintf (fp, "       -b <base address of memory> <default: 0xc0000000lu>\n");
    fprintf (fp, "Reads ELF file and writes a verilog hex 32-bit memory image file\n");
}

// ================================================================

int main (int argc, char *argv [])
{
   // ---- Command line parsing
   int opt;

   // elf and hex filenames
   char *elf_fn = malloc (255 * sizeof(char));
   char *hex_fn = malloc (255 * sizeof(char));

   uint64_t base_addr = 0xc0000000lu;
   uint64_t max_mem_addr = base_addr;
   uint64_t mem_size  = 16;

   while ((opt = getopt (argc, argv, "e:h:m:b:")) != -1)
      switch (opt) {
         case 'e' :
            elf_fn = optarg;
            break;
         case 'h' :
            hex_fn = optarg;
            break;
         case 'm' :
            mem_size = atoi (optarg);
            break;
         case 'b' :
            if (sscanf (optarg, "%lx", &base_addr) != 1) {
               fprintf (stderr, "ERR: -b expects a hex argument\n");
               return 1;
            }
            break;
         case '?' :
            print_usage (stderr, argv[0]);
            return 1;
         default :
            print_usage (stderr, argv[0]);
            return 1;
      }

    // Recompute the max_mem_addr based on CLA
    max_mem_addr = base_addr + (mem_size * 1024);

   // ---- Command line parsing

    // Zero out the memory buffer before loading the ELF file
    bzero (mem_buf, MAX_MEM_SIZE);

    c_mem_load_elf (elf_fn, "_start", "exit", "tohost");

    if ((min_addr < base_addr) || (max_mem_addr <= max_addr)) {
        fprintf (stderr, "elf addresses out of range (%lx - %lx)\n"
              , base_addr, max_mem_addr);
	exit (1);
    }

    FILE *fp_out = fopen (hex_fn, "w");
    if (fp_out == NULL) {
	fprintf (stderr, "ERROR: unable to open file '%s' for output\n", hex_fn);
	return 1;
    }

    fprintf (stdout, "Writing mem hex to file '%s'\n", hex_fn);
    write_mem_hex_file (fp_out, base_addr, max_addr, max_mem_addr);
    // write_mem_hex_file (fp_out, BASE_ADDR_B, MAX_MEM_ADDR);

    fclose (fp_out);
}
