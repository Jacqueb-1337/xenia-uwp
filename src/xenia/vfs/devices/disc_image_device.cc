/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_device.h"

#include <vector>

#include "xenia/base/filesystem.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/vfs/devices/disc_image_entry.h"

namespace xe {
namespace vfs {

using namespace xe::literals;

const size_t kXESectorSize = 2_KiB;

DiscImageDevice::DiscImageDevice(const std::string_view mount_path,
                                 const std::filesystem::path& host_path)
    : Device(mount_path), name_("GDFX"), host_path_(host_path) {}

DiscImageDevice::~DiscImageDevice() = default;

bool DiscImageDevice::Initialize() {
  FILE* file = xe::filesystem::OpenFile(host_path_, "rb");
  if (!file) {
    xe::LiveDebugWrite(fmt::format(
        "[DiscImage] failed to open {}\n", xe::path_to_utf8(host_path_)));
    return false;
  }

  if (!xe::filesystem::Seek(file, 0, SEEK_END)) {
    fclose(file);
    return false;
  }
  const int64_t image_size = xe::filesystem::Tell(file);
  if (image_size <= 0 || !xe::filesystem::Seek(file, 0, SEEK_SET)) {
    fclose(file);
    return false;
  }
  file_size_ = static_cast<uint64_t>(image_size);
  xe::LiveDebugWrite(fmt::format(
      "[DiscImage] streaming parser opened {} ({} bytes)\n",
      xe::path_to_utf8(host_path_), file_size_));

  ParseState state = {};
  state.file = file;
  state.size = static_cast<size_t>(file_size_);
  auto result = Verify(&state);
  if (result != Error::kSuccess) {
    xe::LiveDebugWrite(fmt::format(
        "[DiscImage] header verify failed result={}\n",
        static_cast<int>(result)));
    fclose(file);
    return false;
  }

  std::vector<uint8_t> root_buffer(state.root_size);
  if (!ReadAt(&state, state.root_offset, root_buffer.data(), root_buffer.size())) {
    xe::LiveDebugWrite("[DiscImage] failed to read root directory table\n");
    fclose(file);
    return false;
  }

  result = ReadAllEntries(&state, root_buffer.data(), root_buffer.size());
  fclose(file);
  if (result != Error::kSuccess) {
    xe::LiveDebugWrite(fmt::format(
        "[DiscImage] failed to read GDFX entries result={}\n",
        static_cast<int>(result)));
    return false;
  }

  xe::LiveDebugWrite("[DiscImage] streaming GDFX mount initialized\n");
  return true;
}

void DiscImageDevice::Dump(StringBuffer* string_buffer) {
  auto global_lock = global_critical_region_.Acquire();
  root_entry_->Dump(string_buffer, 0);
}

Entry* DiscImageDevice::ResolvePath(const std::string_view path) {
  // The filesystem will have stripped our prefix off already, so the path will
  // be in the form:
  // some\PATH.foo
  XELOGFS("DiscImageDevice::ResolvePath({})", path);
  return root_entry_->ResolvePath(path);
}

bool DiscImageDevice::ReadAt(ParseState* state, size_t offset, void* buffer,
                             size_t length) {
  if (!state || !state->file || !buffer || offset > state->size ||
      length > state->size - offset) {
    return false;
  }
  if (!xe::filesystem::Seek(state->file, static_cast<int64_t>(offset),
                            SEEK_SET)) {
    return false;
  }
  return fread(buffer, 1, length, state->file) == length;
}

DiscImageDevice::Error DiscImageDevice::Verify(ParseState* state) {
  static const size_t likely_offsets[] = {
      0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000,
  };
  bool magic_found = false;
  for (size_t n = 0; n < xe::countof(likely_offsets); n++) {
    state->game_offset = likely_offsets[n];
    if (VerifyMagic(state, state->game_offset + (32 * kXESectorSize))) {
      magic_found = true;
      break;
    }
  }
  if (!magic_found) {
    return Error::kErrorFileMismatch;
  }

  const size_t fs_offset = state->game_offset + (32 * kXESectorSize);
  uint8_t fs_header[32] = {};
  if (!ReadAt(state, fs_offset, fs_header, sizeof(fs_header))) {
    return Error::kErrorReadError;
  }
  state->root_sector = xe::load<uint32_t>(fs_header + 20);
  state->root_size = xe::load<uint32_t>(fs_header + 24);
  state->root_offset =
      state->game_offset + (state->root_sector * kXESectorSize);
  if (state->root_size < 13 || state->root_size > 32_MiB ||
      state->root_offset > state->size ||
      state->root_size > state->size - state->root_offset) {
    return Error::kErrorDamagedFile;
  }

  xe::LiveDebugWrite(fmt::format(
      "[DiscImage] GDFX found game_offset=0x{:X} root_offset=0x{:X} "
      "root_size={}\n",
      state->game_offset, state->root_offset, state->root_size));
  return Error::kSuccess;
}

bool DiscImageDevice::VerifyMagic(ParseState* state, size_t offset) {
  char magic[20] = {};
  return ReadAt(state, offset, magic, sizeof(magic)) &&
         std::memcmp(magic, "MICROSOFT*XBOX*MEDIA", sizeof(magic)) == 0;
}

DiscImageDevice::Error DiscImageDevice::ReadAllEntries(
    ParseState* state, const uint8_t* root_buffer, size_t root_buffer_size) {
  auto root_entry = new DiscImageEntry(this, nullptr, "", host_path_);
  root_entry->attributes_ = kFileAttributeDirectory;
  root_entry_ = std::unique_ptr<Entry>(root_entry);

  if (!ReadEntry(state, root_buffer, root_buffer_size, 0, root_entry)) {
    return Error::kErrorOutOfMemory;
  }
  return Error::kSuccess;
}

bool DiscImageDevice::ReadEntry(ParseState* state, const uint8_t* buffer,
                                size_t buffer_size, uint16_t entry_ordinal,
                                DiscImageEntry* parent) {
  const size_t entry_offset = static_cast<size_t>(entry_ordinal) * 4;
  if (!buffer || entry_offset > buffer_size ||
      buffer_size - entry_offset < 14) {
    return false;
  }
  const uint8_t* p = buffer + entry_offset;

  uint16_t node_l = xe::load<uint16_t>(p + 0);
  uint16_t node_r = xe::load<uint16_t>(p + 2);
  size_t sector = xe::load<uint32_t>(p + 4);
  size_t length = xe::load<uint32_t>(p + 8);
  uint8_t attributes = xe::load<uint8_t>(p + 12);
  uint8_t name_length = xe::load<uint8_t>(p + 13);
  if (static_cast<size_t>(14 + name_length) > buffer_size - entry_offset) {
    return false;
  }
  auto name_buffer = reinterpret_cast<const char*>(p + 14);

  if (node_l &&
      !ReadEntry(state, buffer, buffer_size, node_l, parent)) {
    return false;
  }

  auto name = std::string(name_buffer, name_length);
  auto entry = DiscImageEntry::Create(this, parent, name, host_path_);
  entry->attributes_ = attributes | kFileAttributeReadOnly;
  entry->size_ = length;
  entry->allocation_size_ = xe::round_up(length, bytes_per_sector());
  entry->create_timestamp_ = 10000 * 11644473600000LL;
  entry->access_timestamp_ = 10000 * 11644473600000LL;
  entry->write_timestamp_ = 10000 * 11644473600000LL;

  const size_t data_offset =
      state->game_offset + (sector * kXESectorSize);
  if (data_offset > state->size || length > state->size - data_offset) {
    return false;
  }

  if (attributes & kFileAttributeDirectory) {
    entry->data_offset_ = 0;
    entry->data_size_ = 0;
    if (length) {
      if (length > 32_MiB) {
        return false;
      }
      std::vector<uint8_t> child_buffer(length);
      if (!ReadAt(state, data_offset, child_buffer.data(), child_buffer.size()) ||
          !ReadEntry(state, child_buffer.data(), child_buffer.size(), 0,
                     entry.get())) {
        return false;
      }
    }
  } else {
    entry->data_offset_ = data_offset;
    entry->data_size_ = length;
  }

  parent->children_.emplace_back(std::move(entry));

  if (node_r &&
      !ReadEntry(state, buffer, buffer_size, node_r, parent)) {
    return false;
  }
  return true;
}

}  // namespace vfs
}  // namespace xe
