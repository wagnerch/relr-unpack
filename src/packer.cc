// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "packer.h"

#include <vector>

#include "debug.h"
#include "elf_traits.h"

namespace relocation_packer {

// Unpack relative relocations from a run-length encoded packed
// representation.
template <typename ELF>
void RelocationPacker<ELF>::UnpackRelocations(
    const std::vector<typename ELF::Relr>& packed,
    std::vector<typename ELF::Rela>* relocations) {

  typename ELF::Addr base = 0;
  for (unsigned int i=0; i < packed.size(); i++) {
    typename ELF::Relr entry = packed.at(i);
    if ((entry & 1) == 0) {
      typename ELF::Rela relocation;
      relocation.r_offset = entry;
      relocation.r_info = R_ARM_RELATIVE;
      relocation.r_addend = 0;
      relocations->push_back(relocation);
      base = entry + sizeof(typename ELF::Addr);
      continue;
    }

    typename ELF::Addr offset = base;
    while (entry != 0) {
      entry >>= 1;
      if ((entry & 1) != 0) {
        typename ELF::Rela relocation;
        relocation.r_offset = offset;
        relocation.r_info = R_ARM_RELATIVE;
        relocation.r_addend = 0;
        relocations->push_back(relocation);
      }
      offset += sizeof(typename ELF::Addr);
    }
    base += (8 * sizeof(typename ELF::Addr) - 1) * sizeof(typename ELF::Addr);
  }
}

template class RelocationPacker<ELF32_traits>;
template class RelocationPacker<ELF64_traits>;

}  // namespace relocation_packer
