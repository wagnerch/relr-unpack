# relr-unpack
The goal of this utility is to unpack SHT_RELR relocations into SHT_REL or SHT_RELA relocations, to permit converting ChromeOS binaries into something usable on a dynamic linker that does not support SHT_RELR relocations.

This utility currently does not produce a usable binary, it does unpack SHT_RELR relocations and insert them into a SHT_REL/A relocation table, but does not correctly align PT_LOAD program headers for the binary suitable to be loaded.  List of items to do:
* align program headers (PT_LOAD), ((p_vaddr - p_offset) & (p_align - 1)) == 0
* cascade offsets into subsequent sections/program headers, i also think we have to address vaddr/paddr in program headers & sections, but not quite sure.
* probably need to rewrite how the Android tool was handling adjustments.

Anyone with experience in how ELF dynamic executables should be structured properly, and what can be adjusted and what can not would be helpful.

