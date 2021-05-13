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
    const std::vector<uint8_t>& packed,
    std::vector<typename ELF::Rela>* relocations) {

  std::vector<typename ELF::Addr> packed_words;
  CHECK(packed.size() > 4 &&
        packed[0] == 'A' &&
        packed[1] == 'P' &&
        packed[2] == 'S' &&
        packed[3] == '2');
}

template class RelocationPacker<ELF32_traits>;
template class RelocationPacker<ELF64_traits>;

}  // namespace relocation_packer
