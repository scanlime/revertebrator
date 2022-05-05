#pragma once

#include <JuceHeader.h>

class ZipReader64 {
public:
  inline ZipReader64(const juce::File &file)
      : stream(file), fileSize(file.getSize()) {
    juce::BufferedInputStream buf(&stream, 8192, false);
    EndOfCentralDirectory eocd;
    if (eocd.search(buf, fileSize)) {
      auto pos = eocd.dirOffset;
      auto end = std::min(fileSize, pos + eocd.dirSize);
      while (pos < end) {
        buf.setPosition(pos);
        CentralFileHeader fileHeader;
        if (fileHeader.read(buf)) {
          pos = buf.getPosition();
          files.set(fileHeader.name, fileHeader.content);
        } else {
          break;
        }
      }
    }
  }

  inline ~ZipReader64() {}
  inline bool openedOk() const { return stream.openedOk(); }

  inline juce::Range<juce::int64> getByteRange(const juce::String &name) {
    auto f = files[name];
    if (f.compressed > 0 && f.compressed == f.uncompressed) {
      return juce::Range<juce::int64>(f.offset, f.offset + f.uncompressed);
    } else {
      return juce::Range<juce::int64>();
    }
  }

  inline std::unique_ptr<juce::InputStream> open(const juce::String &name) {
    auto f = files[name];
    if (f.compressed > 0) {
      auto region =
          new juce::SubregionStream(&stream, f.offset, f.compressed, false);
      if (f.uncompressed != f.compressed) {
        auto gz = new juce::GZIPDecompressorInputStream(
            region, true,
            juce::GZIPDecompressorInputStream::Format::deflateFormat,
            f.uncompressed);
        return std::unique_ptr<juce::InputStream>(gz);
      } else {
        return std::unique_ptr<juce::InputStream>(region);
      }
    }
    return nullptr;
  }

private:
  struct FileInfo {
    juce::int64 compressed{0}, uncompressed{0}, offset{0};
  };

  struct EndOfCentralDirectory {
    juce::int64 dirOffset{0}, dirSize{0}, totalDirEntries{0};

    inline bool read(juce::InputStream &in, juce::int64 pos,
                     juce::int64 fileSize) {
      in.setPosition(pos);

      // Optional 64-bit EOCD locator may appear before the EOCD
      uint32_t eocd64Locator_signature = in.readInt();
      uint32_t eocd64Locator_disk = in.readInt();
      uint64_t eocd64Locator_offset = in.readInt64();
      uint32_t eocd64Locator_totalDisks = in.readInt();

      // Original EOCD
      uint32_t eocd_signature = in.readInt();
      uint16_t eocd_thisDisk = in.readShort();
      uint16_t eocd_dirDisk = in.readShort();
      uint16_t eocd_diskDirEntries = in.readShort();
      uint16_t eocd_totalDirEntries = in.readShort();
      uint32_t eocd_dirSize = in.readInt();
      uint32_t eocd_dirOffset = in.readInt();
      uint16_t eocd_commentLen = in.readShort();

      if (eocd_signature == 0x06054b50 && eocd_thisDisk == 0 &&
          eocd_dirDisk == 0 && eocd_diskDirEntries == eocd_totalDirEntries &&
          in.getPosition() + eocd_commentLen <= fileSize) {
        dirOffset = eocd_dirOffset;
        dirSize = eocd_dirSize;
        totalDirEntries = eocd_totalDirEntries;
      } else {
        return false;
      }

      if (eocd64Locator_signature == 0x07064b50 && eocd64Locator_disk == 0 &&
          eocd64Locator_totalDisks == 1 && eocd64Locator_offset < pos) {
        in.setPosition(eocd64Locator_offset);

        // Optional EOCD64
        uint32_t eocd64_signature = in.readInt();
        uint64_t eocd64_size = in.readInt64();
        uint16_t eocd64_versionMadeBy = in.readShort();
        uint16_t eocd64_versionToExtract = in.readShort();
        uint32_t eocd64_thisDisk = in.readInt();
        uint32_t eocd64_dirDisk = in.readInt();
        uint64_t eocd64_diskDirEntries = in.readInt64();
        uint64_t eocd64_totalDirEntries = in.readInt64();
        uint64_t eocd64_dirSize = in.readInt64();
        uint64_t eocd64_dirOffset = in.readInt64();

        if (eocd64_signature == 0x06064b50 && eocd64_thisDisk == 0 &&
            eocd64_dirDisk == 0 &&
            eocd64_diskDirEntries == eocd64_totalDirEntries) {
          // If EOCD64 looks good, it overrides original EOCD
          dirOffset = eocd64_dirOffset;
          dirSize = eocd64_dirSize;
          totalDirEntries = eocd64_totalDirEntries;
        }
      }
      return true;
    }

    inline bool search(juce::InputStream &in, juce::int64 fileSize) {
      auto first = std::max<juce::int64>(0, fileSize - (22 + 20));
      auto last = std::max<juce::int64>(0, fileSize - (128 * 1024));
      for (auto pos = first; pos >= last; pos--) {
        if (read(in, pos, fileSize)) {
          return true;
        }
      }
      return false;
    }
  };

  struct LocalFileHeader {
    juce::int64 fileDataOffset{0};

    inline bool read(juce::InputStream &in) {
      // We won't bother validating most of these or loading 64-bit extensions,
      // the copy in the central directory is used instead.
      uint32_t file_signature = in.readInt();
      uint16_t file_versionToExtract = in.readShort();
      uint16_t file_bits = in.readShort();
      uint16_t file_compressType = in.readShort();
      uint16_t file_time = in.readShort();
      uint16_t file_date = in.readShort();
      uint32_t file_crc = in.readInt();
      uint32_t file_compressedSize = in.readInt();
      uint32_t file_uncompressedSize = in.readInt();
      uint16_t file_nameLen = in.readShort();
      uint16_t file_extraLen = in.readShort();

      if (file_signature == 0x04034b50) {
        fileDataOffset = in.getPosition() + file_nameLen + file_extraLen;
        return true;
      } else {
        return false;
      }
    }
  };

  struct CentralFileHeader {
    juce::String name;
    FileInfo content;
    juce::int64 localHeaderOffset{0};

    inline bool read(juce::InputStream &in) {
      uint32_t file_signature = in.readInt();
      uint16_t file_versionMadeBy = in.readShort();
      uint16_t file_versionToExtract = in.readShort();
      uint16_t file_bits = in.readShort();
      uint16_t file_compressType = in.readShort();
      uint16_t file_time = in.readShort();
      uint16_t file_date = in.readShort();
      uint32_t file_crc = in.readInt();
      uint32_t file_compressedSize = in.readInt();
      uint32_t file_uncompressedSize = in.readInt();
      uint16_t file_nameLen = in.readShort();
      uint16_t file_extraLen = in.readShort();
      uint16_t file_commentLen = in.readShort();
      uint16_t file_diskNum = in.readShort();
      uint16_t file_intAttrs = in.readShort();
      uint16_t file_extAttrs = in.readInt();
      uint32_t file_offset = in.readInt();

      if (file_signature == 0x02014b50 && file_diskNum == 0) {
        content.compressed = file_compressedSize;
        content.uncompressed = file_uncompressedSize;
        localHeaderOffset = file_offset;
      } else {
        return false;
      }

      {
        juce::MemoryBlock nameUtf8;
        in.readIntoMemoryBlock(nameUtf8, file_nameLen);
        name = nameUtf8.toString();
      }

      // Search through extension headers, we need the 64-bit info
      auto pos = in.getPosition();
      auto nextHeaderAt = pos + file_extraLen + file_commentLen;
      while (pos + 4 <= nextHeaderAt) {
        uint16_t extra_id = in.readShort();
        uint16_t extra_size = in.readShort();
        if (extra_id == 0x0001) {
          // 64-bit extra file data
          if (extra_size >= 8 * 1) {
            content.uncompressed = in.readInt64();
          }
          if (extra_size >= 8 * 2) {
            content.compressed = in.readInt64();
          }
          if (extra_size >= 8 * 3) {
            localHeaderOffset = in.readInt64();
          }
        }
        pos += 4 + extra_size;
        in.setPosition(pos);
      }

      // Now that both 32-bit and 64-bit sizes have been parsed,
      // check if the compression type is consistent with supported values.
      bool isUncompressed =
          file_compressType == 0 && content.compressed == content.uncompressed;
      bool isDeflate =
          file_compressType == 8 && content.compressed != content.uncompressed;
      if (!isUncompressed && !isDeflate) {
        return false;
      }

      // Now we're done with the central directory, get the actual
      // file offset from the beginning of its local header.
      LocalFileHeader local;
      in.setPosition(localHeaderOffset);
      if (!local.read(in)) {
        return false;
      }
      content.offset = local.fileDataOffset;

      // Skip comment, position back at next central directory header
      in.setPosition(nextHeaderAt);
      return true;
    }
  };

  juce::FileInputStream stream;
  juce::int64 fileSize;
  juce::HashMap<juce::String, FileInfo> files;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ZipReader64)
};
