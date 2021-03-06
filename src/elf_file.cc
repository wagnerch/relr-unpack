// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Implementation notes:
//
// We need to remove a piece from the ELF shared library.  However, we also
// want to avoid fixing DWARF cfi data and relative relocation addresses.
// So after packing we shift offets and starting address of the RX segment
// while preserving code/data vaddrs location.
// This requires some fixups for symtab/hash/gnu_hash dynamic section addresses.

#include "elf_file.h"

#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <string>
#include <vector>

#include "debug.h"
#include "elf_traits.h"
#include "libelf.h"
#include "packer.h"

namespace relocation_packer {

// Out-of-band dynamic tags used to indicate the offset and size of the
// android packed relocations section.
static constexpr int32_t DT_RELRSZ = 35;
static constexpr int32_t DT_RELR = 36;
static constexpr int32_t DT_RELRENT = 37;

static constexpr uint32_t SHT_RELR = 19;

static const size_t kPageSize = 4096;

// Alignment to preserve, in bytes.  This must be at least as large as the
// largest d_align and sh_addralign values found in the loaded file.
// Out of caution for RELRO page alignment, we preserve to a complete target
// page.  See http://www.airs.com/blog/archives/189.
static const size_t kPreserveAlignment = kPageSize;

// Get section data.  Checks that the section has exactly one data entry,
// so that the section size and the data size are the same.  True in
// practice for all sections we resize when packing or unpacking.  Done
// by ensuring that a call to elf_getdata(section, data) returns NULL as
// the next data entry.
static Elf_Data* GetSectionData(Elf_Scn* section) {
  Elf_Data* data = elf_getdata(section, NULL);
  CHECK(data && elf_getdata(section, data) == NULL);
  return data;
}

// Rewrite section data.  Allocates new data and makes it the data element's
// buffer.  Relies on program exit to free allocated data.
static void SetSectionData(Elf_Scn* section,
                               const void* section_data,
                               size_t size) {
  Elf_Data* data = GetSectionData(section);
  CHECK(size == data->d_size);
  uint8_t* area = new uint8_t[size];
  memcpy(area, section_data, size);
  data->d_buf = area;
}

// Verbose ELF header logging.
template <typename Ehdr>
static void VerboseLogElfHeader(const Ehdr* elf_header) {
  VLOG(1) << "e_phoff = " << elf_header->e_phoff;
  VLOG(1) << "e_shoff = " << elf_header->e_shoff;
  VLOG(1) << "e_ehsize = " << elf_header->e_ehsize;
  VLOG(1) << "e_phentsize = " << elf_header->e_phentsize;
  VLOG(1) << "e_phnum = " << elf_header->e_phnum;
  VLOG(1) << "e_shnum = " << elf_header->e_shnum;
  VLOG(1) << "e_shstrndx = " << elf_header->e_shstrndx;
}

// Verbose ELF program header logging.
template <typename Phdr>
static void VerboseLogProgramHeader(size_t program_header_index,
                             const Phdr* program_header) {
  std::string type;
  switch (program_header->p_type) {
    case PT_NULL: type = "NULL"; break;
    case PT_LOAD: type = "LOAD"; break;
    case PT_DYNAMIC: type = "DYNAMIC"; break;
    case PT_INTERP: type = "INTERP"; break;
    case PT_PHDR: type = "PHDR"; break;
    case PT_GNU_RELRO: type = "GNU_RELRO"; break;
    case PT_GNU_STACK: type = "GNU_STACK"; break;
    case PT_ARM_EXIDX: type = "EXIDX"; break;
    default: type = "(OTHER)"; break;
  }
  VLOG(1) << "phdr[" << program_header_index << "] : " << type;
  VLOG(1) << "  p_offset = " << program_header->p_offset;
  VLOG(1) << "  p_vaddr = " << program_header->p_vaddr;
  VLOG(1) << "  p_paddr = " << program_header->p_paddr;
  VLOG(1) << "  p_filesz = " << program_header->p_filesz;
  VLOG(1) << "  p_memsz = " << program_header->p_memsz;
  VLOG(1) << "  p_flags = " << program_header->p_flags;
  VLOG(1) << "  p_align = " << program_header->p_align;
}

// Verbose ELF section header logging.
template <typename Shdr>
static void VerboseLogSectionHeader(const std::string& section_name,
                             const Shdr* section_header) {
  VLOG(1) << "section " << section_name;
  VLOG(1) << "  sh_addr = " << section_header->sh_addr;
  VLOG(1) << "  sh_offset = " << section_header->sh_offset;
  VLOG(1) << "  sh_size = " << section_header->sh_size;
  VLOG(1) << "  sh_entsize = " << section_header->sh_entsize;
  VLOG(1) << "  sh_addralign = " << section_header->sh_addralign;
}

// Verbose ELF section data logging.
static void VerboseLogSectionData(const Elf_Data* data) {
  VLOG(1) << "  data";
  VLOG(1) << "    d_buf = " << data->d_buf;
  VLOG(1) << "    d_off = " << data->d_off;
  VLOG(1) << "    d_size = " << data->d_size;
  VLOG(1) << "    d_align = " << data->d_align;
}

// Load the complete ELF file into a memory image in libelf, and identify
// the .rel.dyn or .rela.dyn, .dynamic, and .android.rel.dyn or
// .android.rela.dyn sections.  No-op if the ELF file has already been loaded.
template <typename ELF>
bool ElfFile<ELF>::Load() {
  if (elf_)
    return true;

  Elf* elf = elf_begin(fd_, ELF_C_RDWR, NULL);
  CHECK(elf);

  if (elf_kind(elf) != ELF_K_ELF) {
    LOG(ERROR) << "File not in ELF format";
    return false;
  }

  auto elf_header = ELF::getehdr(elf);
  if (!elf_header) {
    LOG(ERROR) << "Failed to load ELF header: " << elf_errmsg(elf_errno());
    return false;
  }

  if (elf_header->e_type != ET_DYN) {
    LOG(ERROR) << "ELF file is not a shared object";
    return false;
  }

  // Require that our endianness matches that of the target, and that both
  // are little-endian.  Safe for all current build/target combinations.
  const int endian = elf_header->e_ident[EI_DATA];
  CHECK(endian == ELFDATA2LSB);
  CHECK(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__);

  const int file_class = elf_header->e_ident[EI_CLASS];
  VLOG(1) << "endian = " << endian << ", file class = " << file_class;
  VerboseLogElfHeader(elf_header);

  auto elf_program_header = ELF::getphdr(elf);
  CHECK(elf_program_header != nullptr);

  const typename ELF::Phdr* dynamic_program_header = NULL;
  for (size_t i = 0; i < elf_header->e_phnum; ++i) {
    auto program_header = &elf_program_header[i];
    VerboseLogProgramHeader(i, program_header);

    if (program_header->p_type == PT_DYNAMIC) {
      CHECK(dynamic_program_header == NULL);
      dynamic_program_header = program_header;
    }
  }
  CHECK(dynamic_program_header != nullptr);

  size_t string_index;
  elf_getshdrstrndx(elf, &string_index);

  // Notes of the dynamic relocations, packed relocations, and .dynamic
  // sections.  Found while iterating sections, and later stored in class
  // attributes.
  Elf_Scn* found_relocations_section = nullptr;
  Elf_Scn* found_dynamic_section = nullptr;

  // Notes of relocation section types seen.  We require one or the other of
  // these; both is unsupported.
  bool has_rel_relocations = false;
  bool has_rela_relocations = false;

  Elf_Scn* section = NULL;
  while ((section = elf_nextscn(elf, section)) != nullptr) {
    auto section_header = ELF::getshdr(section);
    std::string name = elf_strptr(elf, string_index, section_header->sh_name);
    VerboseLogSectionHeader(name, section_header);

    // Note relocation section types.
    if (section_header->sh_type == SHT_REL) {
      has_rel_relocations = true;
    }
    if (section_header->sh_type == SHT_RELA) {
      has_rela_relocations = true;
    }

    // Note special sections as we encounter them.
    if ((name == ".rel.dyn" || name == ".rela.dyn") &&
        section_header->sh_size > 0) {
      found_relocations_section = section;
    }
    if (section_header->sh_type == SHT_RELR) {
      relr_section_ = section;
    }

    if (section_header->sh_offset == dynamic_program_header->p_offset) {
      found_dynamic_section = section;
    }

    // Ensure we preserve alignment, repeated later for the data block(s).
    CHECK(section_header->sh_addralign <= kPreserveAlignment);

    Elf_Data* data = NULL;
    while ((data = elf_getdata(section, data)) != NULL) {
      CHECK(data->d_align <= kPreserveAlignment);
      VerboseLogSectionData(data);
    }
  }

  // Loading failed if we did not find the required special sections.
  if (!found_dynamic_section) {
    LOG(ERROR) << "Missing .dynamic section";
    return false;
  }

  if (found_relocations_section != nullptr) {
    // Loading failed if we could not identify the relocations type.
    if (!has_rel_relocations && !has_rela_relocations) {
      LOG(ERROR) << "No relocations sections found";
      return false;
    }
    if (has_rel_relocations && has_rela_relocations) {
      LOG(ERROR) << "Multiple relocations sections with different types found, "
                 << "not currently supported";
      return false;
    }
  }

  if (!relr_section_) {
    LOG(ERROR) << "Missing .relr.dyn section";
    return false;
  }

  elf_ = elf;
  relocations_section_ = found_relocations_section;
  dynamic_section_ = found_dynamic_section;
  relocations_type_ = has_rel_relocations ? REL : RELA;
  return true;
}

// Helper for ResizeSection().  Adjust the main ELF header for the hole.
template <typename ELF>
static void AdjustElfHeaderForHole(typename ELF::Ehdr* elf_header,
                                   typename ELF::Off hole_start,
                                   ssize_t hole_size) {
  if (elf_header->e_phoff > hole_start) {
    elf_header->e_phoff += hole_size;
    VLOG(1) << "e_phoff adjusted to " << elf_header->e_phoff;
  }
  if (elf_header->e_shoff > hole_start) {
    elf_header->e_shoff += hole_size;
    VLOG(1) << "e_shoff adjusted to " << elf_header->e_shoff;
  }
}

// Helper for ResizeSection().  Adjust all section headers for the hole.
template <typename ELF>
static void AdjustSectionHeadersForHole(Elf* elf,
                                        typename ELF::Off hole_start,
                                        ssize_t hole_size) {
  size_t string_index;
  elf_getshdrstrndx(elf, &string_index);

  Elf_Scn* section = NULL;
  while ((section = elf_nextscn(elf, section)) != NULL) {
    auto section_header = ELF::getshdr(section);
    std::string name = elf_strptr(elf, string_index, section_header->sh_name);

    if (section_header->sh_offset > hole_start) {
      section_header->sh_offset += hole_size;
      VLOG(1) << "section " << name
              << " sh_offset adjusted to " << section_header->sh_offset;
#if 0 // not sure this makes sense
    } else {
      section_header->sh_addr -= hole_size;
      VLOG(1) << "section " << name
              << " sh_addr adjusted to " << section_header->sh_addr;
#endif
    }
  }
}

// Helper for ResizeSection().  Adjust the offsets of any program headers
// that have offsets currently beyond the hole start, and adjust the
// virtual and physical addrs (and perhaps alignment) of the others.
template <typename ELF>
static void AdjustProgramHeaderFields(typename ELF::Phdr* program_headers,
                                      size_t count,
                                      typename ELF::Off hole_start,
                                      ssize_t hole_size) {
  for (size_t i = 0; i < count; ++i) {
    typename ELF::Phdr* program_header = &program_headers[i];

    // Do not adjust PT_GNU_STACK - it confuses gdb and results
    // in incorrect unwinding if the executable is stripped after
    // packing.
    if (program_header->p_type == PT_GNU_STACK) {
      continue;
    }

    if (program_header->p_offset > hole_start) {
      // The hole start is past this segment, so adjust offset.
      program_header->p_offset += hole_size;
      VLOG(1) << "phdr[" << i
              << "] p_offset adjusted to "<< program_header->p_offset;
    }
  }
}

// Helper for ResizeSection().  Find the first loadable segment in the
// file.  We expect it to map from file offset zero.
template <typename ELF>
static typename ELF::Phdr* FindLoadSegmentForHole(typename ELF::Phdr* program_headers,
                                                  size_t count,
                                                  typename ELF::Off hole_start) {
  for (size_t i = 0; i < count; ++i) {
    typename ELF::Phdr* program_header = &program_headers[i];

    if (program_header->p_type == PT_LOAD &&
        program_header->p_offset <= hole_start &&
        (program_header->p_offset + program_header->p_filesz) >= hole_start ) {
      return program_header;
    }
  }
  LOG(FATAL) << "Cannot locate a LOAD segment with hole_start=0x" << std::hex << hole_start;
  NOTREACHED();

  return nullptr;
}

// Helper for ResizeSection().  Rewrite program headers.
template <typename ELF>
static void RewriteProgramHeadersForHole(Elf* elf,
                                         typename ELF::Off hole_start,
                                         ssize_t hole_size) {
  const typename ELF::Ehdr* elf_header = ELF::getehdr(elf);
  CHECK(elf_header);

  typename ELF::Phdr* elf_program_header = ELF::getphdr(elf);
  CHECK(elf_program_header);

  const size_t program_header_count = elf_header->e_phnum;

  // Locate the segment that we can overwrite to form the new LOAD entry,
  // and the segment that we are going to split into two parts.
  typename ELF::Phdr* target_load_header =
      FindLoadSegmentForHole<ELF>(elf_program_header, program_header_count, hole_start);

  VLOG(1) << "phdr[" << target_load_header - elf_program_header << "] adjust";
  // Adjust PT_LOAD program header memsz and filesz
  target_load_header->p_filesz += hole_size;
  target_load_header->p_memsz += hole_size;

  // Adjust the offsets and p_vaddrs
  AdjustProgramHeaderFields<ELF>(elf_program_header,
                                 program_header_count,
                                 hole_start,
                                 hole_size);
}

// Helper for ResizeSection().  Locate and return the dynamic section.
template <typename ELF>
static Elf_Scn* GetDynamicSection(Elf* elf) {
  const typename ELF::Ehdr* elf_header = ELF::getehdr(elf);
  CHECK(elf_header);

  const typename ELF::Phdr* elf_program_header = ELF::getphdr(elf);
  CHECK(elf_program_header);

  // Find the program header that describes the dynamic section.
  const typename ELF::Phdr* dynamic_program_header = NULL;
  for (size_t i = 0; i < elf_header->e_phnum; ++i) {
    const typename ELF::Phdr* program_header = &elf_program_header[i];

    if (program_header->p_type == PT_DYNAMIC) {
      dynamic_program_header = program_header;
    }
  }
  CHECK(dynamic_program_header);

  // Now find the section with the same offset as this program header.
  Elf_Scn* dynamic_section = NULL;
  Elf_Scn* section = NULL;
  while ((section = elf_nextscn(elf, section)) != NULL) {
    typename ELF::Shdr* section_header = ELF::getshdr(section);

    if (section_header->sh_offset == dynamic_program_header->p_offset) {
      dynamic_section = section;
    }
  }
  CHECK(dynamic_section != NULL);

  return dynamic_section;
}

// Helper for ResizeSection().  Adjust the .dynamic section for the hole.
template <typename ELF>
void ElfFile<ELF>::AdjustDynamicSectionForHole(Elf_Scn* dynamic_section,
                                               typename ELF::Off hole_start,
                                               ssize_t hole_size) {
  Elf_Data* data = GetSectionData(dynamic_section);

  auto dynamic_base = reinterpret_cast<typename ELF::Dyn*>(data->d_buf);
  std::vector<typename ELF::Dyn> dynamics(
      dynamic_base,
      dynamic_base + data->d_size / sizeof(dynamics[0]));

  if (hole_size > 0) { // expanding
    hole_start += hole_size;
  }

  for (size_t i = 0; i < dynamics.size(); ++i) {
    typename ELF::Dyn* dynamic = &dynamics[i];
    const typename ELF::Sword tag = dynamic->d_tag;

#if 0 // not sure this makes sense
    // Any tags that hold offsets are adjustment candidates.
    const bool is_adjustable = (tag == DT_PLTGOT ||
                                tag == DT_HASH ||
                                tag == DT_GNU_HASH ||
                                tag == DT_STRTAB ||
                                tag == DT_SYMTAB ||
                                tag == DT_RELA ||
                                tag == DT_INIT ||
                                tag == DT_FINI ||
                                tag == DT_REL ||
                                tag == DT_JMPREL ||
                                tag == DT_INIT_ARRAY ||
                                tag == DT_FINI_ARRAY ||
                                tag == DT_VERSYM ||
                                tag == DT_VERNEED ||
                                tag == DT_VERDEF);

    if (is_adjustable && dynamic->d_un.d_ptr <= hole_start) {
      dynamic->d_un.d_ptr -= hole_size;
      VLOG(1) << "dynamic[" << i << "] " << dynamic->d_tag
              << " d_ptr adjusted to " << dynamic->d_un.d_ptr;
    }
#endif

    // DT_RELSZ or DT_RELASZ indicate the overall size of relocations.
    // Only one will be present.  Adjust by hole size.
    if (tag == DT_RELSZ || tag == DT_RELASZ) {
      dynamic->d_un.d_val += hole_size;
      VLOG(1) << "dynamic[" << i << "] " << dynamic->d_tag
              << " d_val adjusted to " << dynamic->d_un.d_val;
    }

    // Special case: DT_MIPS_RLD_MAP_REL stores the difference between dynamic
    // entry address and the address of the _r_debug (used by GDB)
    // since the dynamic section and target address are on the
    // different sides of the hole it needs to be adjusted accordingly
    if (tag == DT_MIPS_RLD_MAP_REL) {
      dynamic->d_un.d_val += hole_size;
      VLOG(1) << "dynamic[" << i << "] " << dynamic->d_tag
              << " d_val adjusted to " << dynamic->d_un.d_val;
    }

    // Ignore DT_RELCOUNT and DT_RELACOUNT: (1) nobody uses them and
    // technically (2) the relative relocation count is not changed.

    // DT_RELENT and DT_RELAENT don't change, ignore them as well.
  }

  void* section_data = &dynamics[0];
  size_t bytes = dynamics.size() * sizeof(dynamics[0]);
  SetSectionData(dynamic_section, section_data, bytes);
}

// Resize a section.  If the new size is larger than the current size, open
// up a hole by increasing file offsets that come after the hole.  If smaller
// than the current size, remove the hole by decreasing those offsets.
template <typename ELF>
void ElfFile<ELF>::ResizeSection(Elf* elf, Elf_Scn* section, size_t new_size) {

  size_t string_index;
  elf_getshdrstrndx(elf, &string_index);
  auto section_header = ELF::getshdr(section);
  std::string name = elf_strptr(elf, string_index, section_header->sh_name);

  if (section_header->sh_size == new_size) {
    return;
  }

  // Require that the section size and the data size are the same.  True
  // in practice for all sections we resize when packing or unpacking.
  Elf_Data* data = GetSectionData(section);
  CHECK(data->d_off == 0 && data->d_size == section_header->sh_size);

  // Require that the section is not zero-length (that is, has allocated
  // data that we can validly expand).
  CHECK(data->d_size && data->d_buf);

  const auto hole_start = section_header->sh_offset;
  const ssize_t hole_size = new_size - data->d_size;

  VLOG_IF(1, (hole_size > 0)) << "expand section (" << name << ") size: " <<
      data->d_size << " -> " << new_size;
  VLOG_IF(1, (hole_size < 0)) << "shrink section (" << name << ") size: " <<
      data->d_size << " -> " << new_size;

  // Resize the data and the section header.
  data->d_size += hole_size;
  section_header->sh_size += hole_size;

  // Add the hole size to all offsets in the ELF file that are after the
  // start of the hole.  If the hole size is positive we are expanding the
  // section to create a new hole; if negative, we are closing up a hole.

  // Start with the main ELF header.
  typename ELF::Ehdr* elf_header = ELF::getehdr(elf);
  AdjustElfHeaderForHole<ELF>(elf_header, hole_start, hole_size);

  // Adjust all section headers.
  AdjustSectionHeadersForHole<ELF>(elf, hole_start, hole_size);

  // Rewrite the program headers to either split or coalesce segments,
  // and adjust dynamic entries to match.
  RewriteProgramHeadersForHole<ELF>(elf, hole_start, hole_size);

  Elf_Scn* dynamic_section = GetDynamicSection<ELF>(elf);
  AdjustDynamicSectionForHole(dynamic_section, hole_start, hole_size);
}

// Find the first slot in a dynamics array with the given tag.  The array
// always ends with a free (unused) element, and which we exclude from the
// search.  Returns dynamics->size() if not found.
template <typename ELF>
static size_t FindDynamicEntry(typename ELF::Sword tag,
                               std::vector<typename ELF::Dyn>* dynamics) {
  // Loop until the penultimate entry.  We exclude the end sentinel.
  for (size_t i = 0; i < dynamics->size() - 1; ++i) {
    if (dynamics->at(i).d_tag == tag) {
      return i;
    }
  }

  // The tag was not found.
  return dynamics->size();
}

// Replace dynamic entry.
template <typename ELF>
static void ReplaceDynamicEntry(typename ELF::Sword tag,
                                const typename ELF::Dyn& dyn,
                                std::vector<typename ELF::Dyn>* dynamics) {
  const size_t slot = FindDynamicEntry<ELF>(tag, dynamics);
  if (slot == dynamics->size()) {
    LOG(FATAL) << "Dynamic slot is not found for tag=" << tag;
  }

  // Replace this entry with the one supplied.
  dynamics->at(slot) = dyn;
  VLOG(1) << "dynamic[" << slot << "] overwritten with " << dyn.d_tag;
}

// Find packed relative relocations in the packed android relocations
// section, unpack them, and rewrite the dynamic relocations section to
// contain unpacked data.
template <typename ELF>
bool ElfFile<ELF>::UnpackRelocations() {
  // Load the ELF file into libelf.
  if (!Load()) {
    LOG(ERROR) << "Failed to load as ELF";
    return false;
  }

  if (relocations_section_ == nullptr) {
    // There is nothing to do
    return true;
  }

  // Retrieve the current packed android relocations section data.
  Elf_Data* data = GetSectionData(relr_section_);

  // Convert data to a vector of bytes.
  const typename ELF::Relr* packed_base = reinterpret_cast<typename ELF::Relr*>(data->d_buf);
  std::vector<typename ELF::Relr> packed(
      packed_base,
      packed_base + data->d_size / sizeof(packed[0]));

  return UnpackTypedRelocations(packed);
}

// Helper for UnpackRelocations().  Rel type is one of ELF::Rel or ELF::Rela.
template <typename ELF>
bool ElfFile<ELF>::UnpackTypedRelocations(const std::vector<typename ELF::Relr>& packed) {
  // Retrieve the current dynamic relocations section data.
  Elf_Data* data = GetSectionData(relocations_section_);

  std::vector<typename ELF::Rela> relocations;
  if (relocations_type_ == REL) {
    // Convert data to a vector of relocations.
    const typename ELF::Rel* relocations_base = reinterpret_cast<typename ELF::Rel*>(data->d_buf);
    ConvertRelArrayToRelaVector(relocations_base,
        data->d_size / sizeof(typename ELF::Rel), &relocations);
  } else if (relocations_type_ == RELA) {
    // Convert data to a vector of relocations with addends.
    const typename ELF::Rela* relocations_base = reinterpret_cast<typename ELF::Rela*>(data->d_buf);
    relocations = std::vector<typename ELF::Rela>(
        relocations_base,
        relocations_base + data->d_size / sizeof(relocations[0]));
  } else {
    NOTREACHED();
  }

  LOG(INFO) << "Relocations      : " << relocations.size() << " entries";

  const size_t packed_bytes = (relocations.size() * sizeof(relocations[0])) + data->d_size;
  RelocationPacker<ELF> packer;
  packer.UnpackRelocations(packed, &relocations);

  // Unpack the data to re-materialize the relative relocations.
  LOG(INFO) << "Packed           : " << packed_bytes << " bytes";

  const size_t relocation_entry_size =
      relocations_type_ == REL ? sizeof(typename ELF::Rel) : sizeof(typename ELF::Rela);
  const size_t unpacked_bytes = relocations.size() * relocation_entry_size;
  LOG(INFO) << "Unpacked         : " << unpacked_bytes << " bytes";

  // If we found the same number of null relocation entries in the dynamic
  // relocations section as we hold as unpacked relative relocations, then
  // this is a padded file.

  const bool is_padded = packed_bytes == unpacked_bytes;

  // Unless padded, pre-apply relative relocations to account for the
  // hole, and pre-adjust all relocation offsets accordingly.

  if (!is_padded) {
    LOG(INFO) << "Expansion     : " << unpacked_bytes - packed_bytes << " bytes";
  }

  // Rewrite the current dynamic relocations section with unpacked version of
  // relocations.
  const void* section_data = nullptr;
  if (relocations_type_ == RELA) {
    section_data = &relocations[0];
  } else if (relocations_type_ == REL) {
    std::vector<typename ELF::Rel> rel_relocations;
    ConvertRelaVectorToRelVector(relocations, &rel_relocations);
    section_data = &rel_relocations[0];
  } else {
    NOTREACHED();
  }

  ResizeSection(elf_, relocations_section_, unpacked_bytes);
  SetSectionData(relocations_section_, section_data, unpacked_bytes);

  // Rewrite .dynamic to remove two tags describing packed android relocations.
  data = GetSectionData(dynamic_section_);
  const typename ELF::Dyn* dynamic_base = reinterpret_cast<typename ELF::Dyn*>(data->d_buf);
  std::vector<typename ELF::Dyn> dynamics(
      dynamic_base,
      dynamic_base + data->d_size / sizeof(dynamics[0]));
  {
    const typename ELF::Sword tag = DT_RELRSZ;
    const size_t slot = FindDynamicEntry<ELF>(tag, &dynamics);
    if (slot == dynamics.size()) {
      LOG(FATAL) << "Dynamic slot is not found for tag=" << tag;
    }

    dynamics.erase(dynamics.begin() + slot);
  }
  {
    const typename ELF::Sword tag = DT_RELR;
    const size_t slot = FindDynamicEntry<ELF>(tag, &dynamics);
    if (slot == dynamics.size()) {
      LOG(FATAL) << "Dynamic slot is not found for tag=" << tag;
    }

    dynamics.erase(dynamics.begin() + slot);
  }
  {
    const typename ELF::Sword tag = DT_RELRENT;
    const size_t slot = FindDynamicEntry<ELF>(tag, &dynamics);
    if (slot == dynamics.size()) {
      LOG(FATAL) << "Dynamic slot is not found for tag=" << tag;
    }

    dynamics.erase(dynamics.begin() + slot);
  }

  const void* dynamics_data = &dynamics[0];
  const size_t dynamics_bytes = dynamics.size() * sizeof(dynamics[0]);
  ResizeSection(elf_, dynamic_section_, dynamics_bytes);
  SetSectionData(dynamic_section_, dynamics_data, dynamics_bytes);

  Flush();
  return true;
}

// Flush rewritten shared object file data.
template <typename ELF>
void ElfFile<ELF>::Flush() {
  // Flag all ELF data held in memory as needing to be written back to the
  // file, and tell libelf that we have controlled the file layout.
  elf_flagelf(elf_, ELF_C_SET, ELF_F_DIRTY);
  elf_flagelf(elf_, ELF_C_SET, ELF_F_LAYOUT);

  // Write ELF data back to disk.
  const off_t file_bytes = elf_update(elf_, ELF_C_WRITE);
  if (file_bytes == -1) {
    LOG(ERROR) << "elf_update failed: " << elf_errmsg(elf_errno());
  }

  CHECK(file_bytes > 0);
  VLOG(1) << "elf_update returned: " << file_bytes;

  // Clean up libelf, and truncate the output file to the number of bytes
  // written by elf_update().
  elf_end(elf_);
  elf_ = NULL;
  const int truncate = ftruncate(fd_, file_bytes);
  CHECK(truncate == 0);
}

template <typename ELF>
void ElfFile<ELF>::ConvertRelArrayToRelaVector(const typename ELF::Rel* rel_array,
                                               size_t rel_array_size,
                                               std::vector<typename ELF::Rela>* rela_vector) {
  for (size_t i = 0; i<rel_array_size; ++i) {
    typename ELF::Rela rela;
    rela.r_offset = rel_array[i].r_offset;
    rela.r_info = rel_array[i].r_info;
    rela.r_addend = 0;
    rela_vector->push_back(rela);
  }
}

template <typename ELF>
void ElfFile<ELF>::ConvertRelaVectorToRelVector(const std::vector<typename ELF::Rela>& rela_vector,
                                                std::vector<typename ELF::Rel>* rel_vector) {
  for (auto rela : rela_vector) {
    typename ELF::Rel rel;
    rel.r_offset = rela.r_offset;
    rel.r_info = rela.r_info;
    CHECK(rela.r_addend == 0);
    rel_vector->push_back(rel);
  }
}

template class ElfFile<ELF32_traits>;
template class ElfFile<ELF64_traits>;

}  // namespace relocation_packer
