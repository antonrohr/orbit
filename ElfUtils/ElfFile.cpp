// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ElfUtils/ElfFile.h"

#include <string_view>
#include <vector>

#include "OrbitBase/Logging.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "llvm/DebugInfo/Symbolize/Symbolize.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "outcome.hpp"
#include "symbol.pb.h"

namespace ElfUtils {

namespace {

using orbit_grpc_protos::LineInfo;
using orbit_grpc_protos::ModuleSymbols;
using orbit_grpc_protos::SymbolInfo;

template <typename ElfT>
class ElfFileImpl : public ElfFile {
 public:
  ElfFileImpl(std::string_view file_path,
              llvm::object::OwningBinary<llvm::object::ObjectFile>&& owning_binary);

  [[nodiscard]] ErrorMessageOr<ModuleSymbols> LoadSymbols() override;
  [[nodiscard]] ErrorMessageOr<uint64_t> GetLoadBias() const override;
  [[nodiscard]] bool HasSymtab() const override;
  [[nodiscard]] bool HasDebugInfo() const override;
  [[nodiscard]] bool Is64Bit() const override;
  [[nodiscard]] std::string GetBuildId() const override;
  [[nodiscard]] std::string GetFilePath() const override;
  [[nodiscard]] ErrorMessageOr<LineInfo> GetLineInfo(uint64_t address) override;

 private:
  void InitSections();

  const std::string file_path_;
  llvm::object::OwningBinary<llvm::object::ObjectFile> owning_binary_;
  llvm::object::ELFObjectFile<ElfT>* object_file_;
  llvm::symbolize::LLVMSymbolizer symbolizer_;
  std::string build_id_;
  bool has_symtab_section_;
  bool has_debug_info_section_;
};

template <typename ElfT>
ElfFileImpl<ElfT>::ElfFileImpl(std::string_view file_path,
                               llvm::object::OwningBinary<llvm::object::ObjectFile>&& owning_binary)
    : file_path_(file_path),
      owning_binary_(std::move(owning_binary)),
      has_symtab_section_(false),
      has_debug_info_section_(false) {
  object_file_ = llvm::dyn_cast<llvm::object::ELFObjectFile<ElfT>>(owning_binary_.getBinary());
  InitSections();
}

template <typename ElfT>
void ElfFileImpl<ElfT>::InitSections() {
  const llvm::object::ELFFile<ElfT>* elf_file = object_file_->getELFFile();

  llvm::Expected<typename ElfT::ShdrRange> sections_or_err = elf_file->sections();
  if (!sections_or_err) {
    LOG("Unable to load sections");
    return;
  }

  for (const typename ElfT::Shdr& section : sections_or_err.get()) {
    llvm::Expected<llvm::StringRef> name_or_error = elf_file->getSectionName(&section);
    if (!name_or_error) {
      LOG("Unable to get section name");
      continue;
    }
    llvm::StringRef name = name_or_error.get();

    if (name.str() == ".symtab") {
      has_symtab_section_ = true;
    }

    if (name.str() == ".debug_info") {
      has_debug_info_section_ = true;
    }

    if (name.str() == ".note.gnu.build-id" && section.sh_type == llvm::ELF::SHT_NOTE) {
      llvm::Error error = llvm::Error::success();
      for (const typename ElfT::Note& note : elf_file->notes(section, error)) {
        if (note.getType() != llvm::ELF::NT_GNU_BUILD_ID) continue;

        llvm::ArrayRef<uint8_t> desc = note.getDesc();
        for (const uint8_t& byte : desc) {
          absl::StrAppend(&build_id_, absl::Hex(byte, absl::kZeroPad2));
        }
      }
      if (error) {
        LOG("Error while reading elf notes");
      }
    }
  }
}

template <typename ElfT>
ErrorMessageOr<ModuleSymbols> ElfFileImpl<ElfT>::LoadSymbols() {
  // TODO: if we want to use other sections than .symtab in the future for
  //       example .dynsym, than we have to change this.
  if (!has_symtab_section_) {
    return ErrorMessage("ELF file does not have a .symtab section.");
  }
  bool symbols_added = false;

  ModuleSymbols module_symbols;

  for (const llvm::object::ELFSymbolRef& symbol_ref : object_file_->symbols()) {
    if ((symbol_ref.getFlags() & llvm::object::BasicSymbolRef::SF_Undefined) != 0) {
      continue;
    }
    std::string name = symbol_ref.getName() ? symbol_ref.getName().get() : "";
    std::string demangled_name = llvm::demangle(name);

    // Unknown type - skip and generate a warning
    if (!symbol_ref.getType()) {
      LOG("WARNING: Type is not set for symbol \"%s\" in \"%s\", skipping.", name.c_str(),
          file_path_.c_str());
      continue;
    }

    // Limit list of symbols to functions. Ignore sections and variables.
    if (symbol_ref.getType().get() != llvm::object::SymbolRef::ST_Function) {
      continue;
    }

    uint64_t symbol_address = symbol_ref.getValue();
    SymbolInfo* symbol_info = module_symbols.add_symbol_infos();
    symbol_info->set_name(name);
    symbol_info->set_demangled_name(demangled_name);
    symbol_info->set_address(symbol_address);
    symbol_info->set_size(symbol_ref.getSize());

    symbols_added = true;
  }
  if (!symbols_added) {
    return ErrorMessage(
        "Unable to load symbols from ELF file, not even a single symbol of "
        "type function found.");
  }
  return module_symbols;
}

template <typename ElfT>
ErrorMessageOr<uint64_t> ElfFileImpl<ElfT>::GetLoadBias() const {
  const llvm::object::ELFFile<ElfT>* elf_file = object_file_->getELFFile();

  uint64_t min_vaddr = UINT64_MAX;
  bool pt_load_found = false;
  llvm::Expected<typename ElfT::PhdrRange> range = elf_file->program_headers();

  if (!range) {
    std::string error = absl::StrFormat(
        "Unable to get load bias of ELF file: \"%s\". No program headers "
        "found.",
        file_path_);
    ERROR("%s", error.c_str());
    return ErrorMessage(std::move(error));
  }

  for (const typename ElfT::Phdr& phdr : range.get()) {
    if (phdr.p_type != llvm::ELF::PT_LOAD) {
      continue;
    }
    pt_load_found = true;

    if (min_vaddr > phdr.p_vaddr) {
      min_vaddr = phdr.p_vaddr;
    }
  }

  if (!pt_load_found) {
    std::string error = absl::StrFormat(
        "Unable to get load bias of ELF file: \"%s\". No PT_LOAD program "
        "headers found.",
        file_path_);
    ERROR("%s", error.c_str());
    return ErrorMessage(std::move(error));
  }
  return min_vaddr;
}

template <typename ElfT>
bool ElfFileImpl<ElfT>::HasSymtab() const {
  return has_symtab_section_;
}

template <typename ElfT>
bool ElfFileImpl<ElfT>::HasDebugInfo() const {
  return has_debug_info_section_;
}

template <typename ElfT>
std::string ElfFileImpl<ElfT>::GetBuildId() const {
  return build_id_;
}

template <typename ElfT>
std::string ElfFileImpl<ElfT>::GetFilePath() const {
  return file_path_;
}

template <typename ElfT>
ErrorMessageOr<LineInfo> ElfUtils::ElfFileImpl<ElfT>::GetLineInfo(uint64_t address) {
  CHECK(has_debug_info_section_);
  auto line_info_or_error = symbolizer_.symbolizeCode(
      *object_file_, {address, llvm::object::SectionedAddress::UndefSection});
  if (!line_info_or_error) {
    return ErrorMessage(absl::StrFormat(
        "Unable to get line number info for \"%s\", address=0x%x: %s", object_file_->getFileName(),
        address, llvm::toString(line_info_or_error.takeError())));
  }

  auto& symbolizer_line_info = line_info_or_error.get();

  // This is what symbolizer returns in case of an error. We convert it to a ErrorMessage here.
  if (symbolizer_line_info.FileName == "<invalid>" && symbolizer_line_info.Line == 0) {
    return ErrorMessage(absl::StrFormat("Unable to get line info for address=0x%x", address));
  }

  LineInfo line_info;
  line_info.set_source_file(symbolizer_line_info.FileName);
  line_info.set_source_line(symbolizer_line_info.Line);

  return line_info;
}

template <>
bool ElfFileImpl<llvm::object::ELF64LE>::Is64Bit() const {
  return true;
}

template <>
bool ElfFileImpl<llvm::object::ELF32LE>::Is64Bit() const {
  return false;
}

}  // namespace

ErrorMessageOr<std::unique_ptr<ElfFile>> ElfFile::CreateFromBuffer(std::string_view file_path,
                                                                   const void* buf, size_t len) {
  std::unique_ptr<llvm::MemoryBuffer> buffer = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(static_cast<const char*>(buf), len), llvm::StringRef("buffer name"), false);
  llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> object_file_or_error =
      llvm::object::ObjectFile::createObjectFile(buffer->getMemBufferRef());

  if (!object_file_or_error) {
    return ErrorMessage(absl::StrFormat("Unable to load ELF file \"%s\": %s", file_path,
                                        llvm::toString(object_file_or_error.takeError())));
  }

  return ElfFile::Create(file_path, llvm::object::OwningBinary<llvm::object::ObjectFile>(
                                        std::move(object_file_or_error.get()), std::move(buffer)));
}

ErrorMessageOr<std::unique_ptr<ElfFile>> ElfFile::Create(std::string_view file_path) {
  // TODO(hebecker): Remove this explicit construction of StringRef when we
  // switch to LLVM10.
  const llvm::StringRef file_path_llvm{file_path.data(), file_path.size()};

  llvm::Expected<llvm::object::OwningBinary<llvm::object::ObjectFile>> object_file_or_error =
      llvm::object::ObjectFile::createObjectFile(file_path_llvm);

  if (!object_file_or_error) {
    return ErrorMessage(absl::StrFormat("Unable to load ELF file \"%s\": %s", file_path,
                                        llvm::toString(object_file_or_error.takeError())));
  }

  llvm::object::OwningBinary<llvm::object::ObjectFile>& file = object_file_or_error.get();

  return ElfFile::Create(file_path, std::move(file));
}

ErrorMessageOr<std::unique_ptr<ElfFile>> ElfFile::Create(
    std::string_view file_path, llvm::object::OwningBinary<llvm::object::ObjectFile>&& file) {
  llvm::object::ObjectFile* object_file = file.getBinary();

  std::unique_ptr<ElfFile> result;

  // Create appropriate ElfFile implementation
  if (llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(object_file) != nullptr) {
    result = std::unique_ptr<ElfFile>(
        new ElfFileImpl<llvm::object::ELF32LE>(file_path, std::move(file)));
  } else if (llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(object_file) != nullptr) {
    result = std::unique_ptr<ElfFile>(
        new ElfFileImpl<llvm::object::ELF64LE>(file_path, std::move(file)));
  } else {
    return ErrorMessage(absl::StrFormat(
        "Unable to load \"%s\": Big-endian architectures are not supported.", file_path));
  }

  return result;
}

}  // namespace ElfUtils
