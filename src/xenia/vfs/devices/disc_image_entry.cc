/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/devices/disc_image_entry.h"

#include <algorithm>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/vfs/devices/disc_image_file.h"

namespace xe {
namespace vfs {

DiscImageEntry::DiscImageEntry(
    Device* device, Entry* parent, const std::string_view path,
    const std::filesystem::path& host_path)
    : Entry(device, parent, path),
      host_path_(host_path),
      data_offset_(0),
      data_size_(0) {}

DiscImageEntry::~DiscImageEntry() = default;

std::unique_ptr<DiscImageEntry> DiscImageEntry::Create(
    Device* device, Entry* parent, const std::string_view name,
    const std::filesystem::path& host_path) {
  auto path = xe::utf8::join_guest_paths(parent->path(), name);
  auto entry =
      std::make_unique<DiscImageEntry>(device, parent, path, host_path);
  return std::move(entry);
}

X_STATUS DiscImageEntry::Open(uint32_t desired_access, File** out_file) {
  *out_file = new DiscImageFile(desired_access, this);
  return X_STATUS_SUCCESS;
}

namespace {
class OwnedMappedSlice final : public MappedMemory {
 public:
  OwnedMappedSlice(std::unique_ptr<MappedMemory> backing, size_t delta,
                   size_t length)
      : MappedMemory(backing->data() + delta, length),
        backing_(std::move(backing)) {}

 private:
  std::unique_ptr<MappedMemory> backing_;
};

class OwnedBufferMappedMemory final : public MappedMemory {
 public:
  explicit OwnedBufferMappedMemory(size_t length) : buffer_(length) {
    data_ = buffer_.data();
    size_ = buffer_.size();
  }

 private:
  std::vector<uint8_t> buffer_;
};
}  // namespace

std::unique_ptr<MappedMemory> DiscImageEntry::OpenMapped(
    MappedMemory::Mode mode, size_t offset, size_t length) {
  if (mode != MappedMemory::Mode::kRead || offset >= data_size_) {
    return nullptr;
  }

  const size_t real_offset = data_offset_ + offset;
  const size_t available = data_size_ - offset;
  const size_t real_length = length ? std::min(length, available) : available;

#if XE_PLATFORM_WINRT
  // Removable-storage files on Xbox UWP can be read normally but mapping a
  // range from the ISO may fail with ERROR_ACCESS_DENIED. Copy only the guest
  // file range requested by the loader instead of mapping the backing ISO.
  auto buffered = std::make_unique<OwnedBufferMappedMemory>(real_length);
  FILE* file = xe::filesystem::OpenFile(host_path_, "rb");
  if (!file || !xe::filesystem::Seek(
                   file, static_cast<int64_t>(real_offset), SEEK_SET)) {
    if (file) fclose(file);
    xe::LiveDebugWrite(fmt::format(
        "[DiscImageEntry] buffered map open/seek failed offset={} length={}\n",
        real_offset, real_length));
    return nullptr;
  }
  const size_t bytes_read =
      fread(buffered->data(), 1, real_length, file);
  fclose(file);
  if (bytes_read != real_length) {
    xe::LiveDebugWrite(fmt::format(
        "[DiscImageEntry] buffered map short read offset={} wanted={} got={}\n",
        real_offset, real_length, bytes_read));
    return nullptr;
  }
  xe::LiveDebugWrite(fmt::format(
      "[DiscImageEntry] buffered map succeeded offset={} length={}\n",
      real_offset, real_length));
  return buffered;
#else
  const size_t aligned_offset =
      real_offset & ~(memory::allocation_granularity() - 1);
  const size_t delta = real_offset - aligned_offset;
  auto backing = MappedMemory::Open(host_path_, mode, real_offset, real_length);
  if (!backing || backing->size() < delta + real_length) {
    return nullptr;
  }
  return std::make_unique<OwnedMappedSlice>(std::move(backing), delta,
                                             real_length);
#endif
}

}  // namespace vfs
}  // namespace xe
