#include "z3ds_compression.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <zstd.h>

// XXH64 implementation to match ZSTD seekable format specification
static u64 XXH64(const void* data, size_t len, u64 seed = 0) {
    const u8* p = static_cast<const u8*>(data);
    const u8* const end = p + len;
    u64 h64;
    
    const u64 prime64_1 = 11400714785074694791ULL;
    const u64 prime64_2 = 14029467366897019727ULL;
    const u64 prime64_3 =  1609587929392839161ULL;
    const u64 prime64_4 =  9650029242287828579ULL;
    const u64 prime64_5 =  2870177450012600261ULL;
    
    if (len >= 32) {
        const u8* const limit = end - 32;
        u64 v1 = seed + prime64_1 + prime64_2;
        u64 v2 = seed + prime64_2;
        u64 v3 = seed + 0;
        u64 v4 = seed - prime64_1;
        
        auto read64 = [](const u8* ptr) -> u64 {
            u64 val;
            memcpy(&val, ptr, sizeof(val));
            return val; // Assuming little-endian
        };
        
        auto rotl64 = [](u64 x, int r) -> u64 {
            return (x << r) | (x >> (64 - r));
        };
        
        do {
            v1 = rotl64(v1 + read64(p) * prime64_2, 31) * prime64_1;
            p += 8;
            v2 = rotl64(v2 + read64(p) * prime64_2, 31) * prime64_1;
            p += 8;
            v3 = rotl64(v3 + read64(p) * prime64_2, 31) * prime64_1;
            p += 8;
            v4 = rotl64(v4 + read64(p) * prime64_2, 31) * prime64_1;
            p += 8;
        } while (p <= limit);
        
        h64 = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        
        auto merge64 = [&](u64 acc, u64 val) -> u64 {
            val = rotl64(val * prime64_2, 31) * prime64_1;
            acc ^= val;
            acc = acc * prime64_1 + prime64_4;
            return acc;
        };
        
        h64 = merge64(h64, v1);
        h64 = merge64(h64, v2);
        h64 = merge64(h64, v3);
        h64 = merge64(h64, v4);
    } else {
        h64 = seed + prime64_5;
    }
    
    h64 += len;
    
    auto rotl64 = [](u64 x, int r) -> u64 {
        return (x << r) | (x >> (64 - r));
    };
    
    while (p + 8 <= end) {
        u64 k1;
        memcpy(&k1, p, sizeof(k1));
        k1 *= prime64_2;
        k1 = rotl64(k1, 31);
        k1 *= prime64_1;
        h64 ^= k1;
        h64 = rotl64(h64, 27) * prime64_1 + prime64_4;
        p += 8;
    }
    
    if (p + 4 <= end) {
        u32 k1;
        memcpy(&k1, p, sizeof(k1));
        h64 ^= (u64)k1 * prime64_1;
        h64 = rotl64(h64, 23) * prime64_2 + prime64_3;
        p += 4;
    }
    
    while (p < end) {
        u8 k1 = *p++;
        h64 ^= k1 * prime64_5;
        h64 = rotl64(h64, 11) * prime64_1;
    }
    
    h64 ^= h64 >> 33;
    h64 *= prime64_2;
    h64 ^= h64 >> 29;
    h64 *= prime64_3;
    h64 ^= h64 >> 32;
    
    return h64;
}

// We'll implement seekable compression using standard ZSTD with custom framing
// This is a simplified version that creates seekable frames manually

void Z3DSMetadata::Add(const std::string& name, const std::string& data) {
    items[name] = std::vector<u8>(data.begin(), data.end());
}

void Z3DSMetadata::Add(const std::string& name, const std::vector<u8>& data) {
    items[name] = data;
}

std::vector<u8> Z3DSMetadata::AsBinary() const {
    if (items.empty()) {
        return {};
    }
    
    std::ostringstream out(std::ios::binary);
    
    // Write version
    u8 version = METADATA_VERSION;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    
    // Write items
    for (const auto& [name, data] : items) {
        Item item{
            .type = Item::TYPE_BINARY,
            .name_len = static_cast<u8>(std::min<size_t>(0xFF, name.size())),
            .data_len = static_cast<u16>(std::min<size_t>(0xFFFF, data.size())),
        };
        
        out.write(reinterpret_cast<const char*>(&item), sizeof(item));
        out.write(name.data(), item.name_len);
        out.write(reinterpret_cast<const char*>(data.data()), item.data_len);
    }
    
    // Write end item
    Item end{};
    out.write(reinterpret_cast<const char*>(&end), sizeof(end));
    
    std::string out_str = out.str();
    return std::vector<u8>(out_str.begin(), out_str.end());
}

std::array<u8, 4> DetectFileMagic(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        return {'U', 'N', 'K', 'N'}; // Unknown
    }
    
    // First check for 3DSX format (magic at start)
    std::array<u8, 4> magic{};
    file.read(reinterpret_cast<char*>(magic.data()), 4);
    
    if (file.gcount() >= 4) {
        if (magic == std::array<u8, 4>{'3', 'D', 'S', 'X'}) {
            return magic;
        }
    }
    
    // Check for NCCH format (CXI) - magic at offset 0x100
    file.seekg(0x100, std::ios::beg);
    file.read(reinterpret_cast<char*>(magic.data()), 4);
    
    if (file.gcount() >= 4) {
        if (magic == std::array<u8, 4>{'N', 'C', 'C', 'H'}) {
            return magic;
        }
        if (magic == std::array<u8, 4>{'N', 'C', 'S', 'D'}) {
            return magic;
        }
    }
    
    // Check for CIA files - they don't have a standard magic, try to detect by structure
    // CIA files start with a certificate chain, we can check for ASN.1 structure
    file.seekg(0x00, std::ios::beg);
    file.read(reinterpret_cast<char*>(magic.data()), 4);
    
    if (file.gcount() >= 4) {
        // CIA files often start with 0x30 (ASN.1 SEQUENCE) for certificate
        if (magic[0] == 0x30) {
            // Additional heuristic: check file extension
            std::string ext = filename.substr(filename.find_last_of('.'));
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".cia") {
                return {'N', 'C', 'S', 'D'}; // Treat CIA as NCSD for frame size purposes
            }
        }
    }
    
    return {'U', 'N', 'K', 'N'};
}

namespace {

u16 ReadLE16(const u8* data) {
    return static_cast<u16>(data[0] | (static_cast<u16>(data[1]) << 8));
}

u32 ReadLE32(const u8* data) {
    return static_cast<u32>(data[0]) |
           (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) |
           (static_cast<u32>(data[3]) << 24);
}

u64 ReadLE64(const u8* data) {
    u64 value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<u64>(data[i]) << (i * 8);
    }
    return value;
}

bool ReadHeader(std::ifstream& input, Z3DSFileHeader& header) {
    std::array<u8, sizeof(Z3DSFileHeader)> raw{};
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char*>(raw.data()), raw.size());
    if (input.gcount() != static_cast<std::streamsize>(raw.size())) {
        return false;
    }

    std::copy(raw.begin(), raw.begin() + 4, header.magic.begin());
    std::copy(raw.begin() + 4, raw.begin() + 8, header.underlying_magic.begin());
    header.version = raw[8];
    header.reserved = raw[9];
    header.header_size = ReadLE16(raw.data() + 10);
    header.metadata_size = ReadLE32(raw.data() + 12);
    header.compressed_size = ReadLE64(raw.data() + 16);
    header.uncompressed_size = ReadLE64(raw.data() + 24);
    return true;
}

} // namespace

size_t GetDefaultFrameSize(const std::array<u8, 4>& magic, std::string_view extension) {
    std::string ext_lower(extension);
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (ext_lower == ".cia") {
        return 32ULL * 1024 * 1024; // Per Azahar recommendation
    }

    if (magic == std::array<u8, 4>{'N', 'C', 'C', 'H'} ||
        magic == std::array<u8, 4>{'N', 'C', 'S', 'D'} ||
        ext_lower == ".cci" || ext_lower == ".cxi" || ext_lower == ".3dsx") {
        return 256ULL * 1024; // 256KB default for these formats
    }

    return 1024ULL * 1024; // Fallback 1MB
}

std::string GetCurrentTimeISO() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto tm = *std::gmtime(&time_t);
    
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

// Proper seekable ZSTD compression implementation
class SeekableZSTDCompressor {
private:
    struct SeekEntry {
        u32 compressed_size;
        u32 decompressed_size;
        u32 checksum; // XXH64 lower 32 bits
    };
    
    std::ofstream& output;
    size_t frame_size;
    ZSTD_CCtx* cctx;
    std::vector<u8> frame_buffer;
    size_t current_frame_pos;
    u64 total_compressed;
    std::vector<SeekEntry> seek_entries;
    bool use_checksums;
    int compression_level;

public:
    SeekableZSTDCompressor(std::ofstream& out, size_t frame_sz, bool checksums,
                           int level, unsigned int worker_count)
        : output(out), frame_size(frame_sz), current_frame_pos(0), total_compressed(0),
          seek_entries(), use_checksums(checksums), compression_level(level) {
        cctx = ZSTD_createCCtx();
        frame_buffer.reserve(frame_size);
        if (cctx) {
            ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, compression_level);
            if (worker_count > 1) {
                ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, worker_count);
                // Allow overlapping to keep throughput high when multi-threading
                ZSTD_CCtx_setParameter(cctx, ZSTD_c_overlapLog, 3);
            }
        }
    }
    
    ~SeekableZSTDCompressor() {
        if (cctx) {
            ZSTD_freeCCtx(cctx);
        }
    }
    
    bool WriteData(const u8* data, size_t size) {
        size_t remaining = size;
        const u8* ptr = data;
        
        while (remaining > 0) {
            size_t to_copy = std::min(remaining, frame_size - current_frame_pos);
            
            // Add data to current frame buffer
            frame_buffer.insert(frame_buffer.end(), ptr, ptr + to_copy);
            current_frame_pos += to_copy;
            ptr += to_copy;
            remaining -= to_copy;
            
            // If frame is full, compress and write it
            if (current_frame_pos >= frame_size) {
                if (!FlushFrame()) {
                    return false;
                }
            }
        }
        
        return true;
    }
    
    bool Finish() {
        // Flush any remaining data
        if (!frame_buffer.empty()) {
            if (!FlushFrame()) {
                return false;
            }
        }
        
        // Write seek table as skippable frame
        return WriteSeekTable();
    }
    
    u64 GetTotalCompressed() const {
        return total_compressed;
    }
    
    size_t GetFrameCount() const {
        return seek_entries.size();
    }
    
private:
    bool FlushFrame() {
        if (frame_buffer.empty()) {
            return true;
        }
        
        // Calculate checksum if enabled (use least significant 32 bits of XXH64)
        u32 checksum = 0;
        if (use_checksums) {
            u64 hash = XXH64(frame_buffer.data(), frame_buffer.size(), 0);
            checksum = static_cast<u32>(hash & 0xFFFFFFFF);
        }
        
        // Compress the frame
        size_t const compressed_bound = ZSTD_compressBound(frame_buffer.size());
        std::vector<u8> compressed_buffer(compressed_bound);
        
        size_t compressed_size = ZSTD_compressCCtx(cctx,
            compressed_buffer.data(), compressed_buffer.size(),
            frame_buffer.data(), frame_buffer.size(),
            compression_level);
            
        if (ZSTD_isError(compressed_size)) {
            std::cerr << "Compression error: " << ZSTD_getErrorName(compressed_size) << std::endl;
            return false;
        }
        
        // Write compressed frame
        output.write(reinterpret_cast<const char*>(compressed_buffer.data()), compressed_size);
        if (!output.good()) {
            return false;
        }
        
        // Record seek entry
        SeekEntry entry{
            .compressed_size = static_cast<u32>(compressed_size),
            .decompressed_size = static_cast<u32>(frame_buffer.size()),
            .checksum = checksum
        };
        seek_entries.push_back(entry);
        
        total_compressed += compressed_size;
        
        // Reset frame buffer
        frame_buffer.clear();
        current_frame_pos = 0;
        
        return true;
    }
    
    bool WriteSeekTable() {
        if (seek_entries.empty()) {
            return true;
        }
        
        // Calculate seek table size
        size_t entry_size = use_checksums ? 12 : 8; // 4+4+4 or 4+4 bytes per entry
        size_t table_size = seek_entries.size() * entry_size + 9; // +9 for footer
        
        // Helper function to write little-endian values
        auto write_le32 = [&](u32 value) {
            u8 bytes[4] = {
                static_cast<u8>(value & 0xFF),
                static_cast<u8>((value >> 8) & 0xFF),
                static_cast<u8>((value >> 16) & 0xFF),
                static_cast<u8>((value >> 24) & 0xFF)
            };
            output.write(reinterpret_cast<const char*>(bytes), 4);
        };
        
        // Write skippable frame header (little-endian)
        u32 skippable_magic = 0x184D2A5E; // ZSTD skippable frame magic
        u32 frame_size = static_cast<u32>(table_size);
        
        write_le32(skippable_magic);
        write_le32(frame_size);
        
        // Write seek table entries (little-endian)
        for (const auto& entry : seek_entries) {
            write_le32(entry.compressed_size);
            write_le32(entry.decompressed_size);
            if (use_checksums) {
                write_le32(entry.checksum);
            }
        }
        
        // Write seek table footer (little-endian)
        u32 num_frames = static_cast<u32>(seek_entries.size());
        u8 descriptor = use_checksums ? 0x80 : 0x00; // bit 7 = checksum flag
        u32 seekable_magic = 0x8F92EAB1; // Seekable ZSTD magic
        
        write_le32(num_frames);
        output.write(reinterpret_cast<const char*>(&descriptor), 1);
        write_le32(seekable_magic);
        
        if (!output.good()) {
            std::cerr << "Error writing seek table" << std::endl;
            return false;
        }
        
        total_compressed += 8 + table_size; // skippable frame header + table
        return true;
    }
};

bool CompressZ3DSFile(const std::string& src_file, const std::string& dst_file,
                      const std::array<u8, 4>& underlying_magic, size_t frame_size,
                      ProgressCallback update_callback,
                      const std::unordered_map<std::string, std::vector<u8>>& metadata,
                      int compression_level, unsigned int worker_count,
                      size_t read_chunk_size) {
    
    // Open source file
    std::ifstream input(src_file, std::ios::binary);
    if (!input.is_open()) {
        std::cerr << "Error: Could not open source file: " << src_file << std::endl;
        return false;
    }
    
    // Get file size
    input.seekg(0, std::ios::end);
    u64 uncompressed_size = input.tellg();
    input.seekg(0, std::ios::beg);
    
    // Open output file
    std::ofstream output(dst_file, std::ios::binary);
    if (!output.is_open()) {
        std::cerr << "Error: Could not create output file: " << dst_file << std::endl;
        return false;
    }
    
    // Create Z3DS header
    Z3DSFileHeader header;
    header.underlying_magic = underlying_magic;
    header.uncompressed_size = uncompressed_size;
    
    // Create metadata
    Z3DSMetadata meta;
    meta.Add("compressor", "Z3DS CLI Tool v1.0");
    meta.Add("date", GetCurrentTimeISO());
    meta.Add("maxframesize", std::to_string(frame_size));
    
    // Add user metadata
    for (const auto& [key, value] : metadata) {
        meta.Add(key, value);
    }
    
    auto metadata_binary = meta.AsBinary();
    header.metadata_size = ((metadata_binary.size() + 15) / 16) * 16; // Align to 16 bytes
    
    // Write header (will be updated later with compressed size)
    size_t header_pos = output.tellp();
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    
    // Write metadata
    output.write(reinterpret_cast<const char*>(metadata_binary.data()), metadata_binary.size());
    
    // Pad metadata to 16-byte boundary
    size_t padding = header.metadata_size - metadata_binary.size();
    std::vector<u8> pad(padding, 0);
    output.write(reinterpret_cast<const char*>(pad.data()), padding);
    
    // Start compression with proper seekable ZSTD format
    SeekableZSTDCompressor compressor(output, frame_size, true, compression_level, worker_count);

    // Compress file in chunks
    const size_t buffer_size = std::max<size_t>(read_chunk_size, 64 * 1024);
    std::vector<u8> buffer(buffer_size);
    size_t processed = 0;
    
    while (input.good() && processed < uncompressed_size) {
        size_t to_read = std::min(buffer_size, static_cast<size_t>(uncompressed_size - processed));
        input.read(reinterpret_cast<char*>(buffer.data()), to_read);
        size_t read_size = input.gcount();
        
        if (read_size == 0) break;
        
        if (!compressor.WriteData(buffer.data(), read_size)) {
            std::cerr << "Error during compression" << std::endl;
            return false;
        }
        
        processed += read_size;
        
        if (update_callback) {
            update_callback(processed, uncompressed_size);
        }
    }
    
    // Finish compression
    if (!compressor.Finish()) {
        std::cerr << "Error finishing compression" << std::endl;
        return false;
    }
    
    // Update header with compressed size (ensure little-endian)
    header.compressed_size = compressor.GetTotalCompressed();
    output.seekp(header_pos);
    
    // Write header with proper little-endian byte ordering
    output.write(reinterpret_cast<const char*>(header.magic.data()), 4);
    output.write(reinterpret_cast<const char*>(header.underlying_magic.data()), 4);
    output.write(reinterpret_cast<const char*>(&header.version), 1);
    output.write(reinterpret_cast<const char*>(&header.reserved), 1);
    
    // Write 16-bit and larger fields in little-endian
    auto write_le16 = [&](u16 value) {
        u8 bytes[2] = {
            static_cast<u8>(value & 0xFF),
            static_cast<u8>((value >> 8) & 0xFF)
        };
        output.write(reinterpret_cast<const char*>(bytes), 2);
    };
    
    auto write_le32 = [&](u32 value) {
        u8 bytes[4] = {
            static_cast<u8>(value & 0xFF),
            static_cast<u8>((value >> 8) & 0xFF),
            static_cast<u8>((value >> 16) & 0xFF),
            static_cast<u8>((value >> 24) & 0xFF)
        };
        output.write(reinterpret_cast<const char*>(bytes), 4);
    };
    
    auto write_le64 = [&](u64 value) {
        u8 bytes[8] = {
            static_cast<u8>(value & 0xFF),
            static_cast<u8>((value >> 8) & 0xFF),
            static_cast<u8>((value >> 16) & 0xFF),
            static_cast<u8>((value >> 24) & 0xFF),
            static_cast<u8>((value >> 32) & 0xFF),
            static_cast<u8>((value >> 40) & 0xFF),
            static_cast<u8>((value >> 48) & 0xFF),
            static_cast<u8>((value >> 56) & 0xFF)
        };
        output.write(reinterpret_cast<const char*>(bytes), 8);
    };
    
    write_le16(header.header_size);
    write_le32(header.metadata_size);
    write_le64(header.compressed_size);
    write_le64(header.uncompressed_size);
    
    if (!output.good()) {
        std::cerr << "Error writing final header" << std::endl;
        return false;
    }
    
    std::cout << "\nCreated " << compressor.GetFrameCount() << " seekable frames" << std::endl;
    
    return true;
}

bool DecompressZ3DSFile(const std::string& src_file, const std::string& dst_file,
                        ProgressCallback update_callback, size_t read_chunk_size) {
    std::ifstream input(src_file, std::ios::binary);
    if (!input.is_open()) {
        std::cerr << "Error: Could not open compressed source file: " << src_file << std::endl;
        return false;
    }

    Z3DSFileHeader header;
    if (!ReadHeader(input, header)) {
        std::cerr << "Error: Failed to read Z3DS header" << std::endl;
        return false;
    }

    if (header.magic != Z3DSFileHeader::EXPECTED_MAGIC) {
        std::cerr << "Error: Invalid Z3DS magic" << std::endl;
        return false;
    }

    if (header.version != Z3DSFileHeader::EXPECTED_VERSION) {
        std::cerr << "Error: Unsupported Z3DS version" << std::endl;
        return false;
    }

    std::ofstream output(dst_file, std::ios::binary);
    if (!output.is_open()) {
        std::cerr << "Error: Could not create destination file: " << dst_file << std::endl;
        return false;
    }

    // Skip header and metadata to reach data start.
    std::streamoff data_start = static_cast<std::streamoff>(header.header_size) +
                               static_cast<std::streamoff>(header.metadata_size);
    input.seekg(data_start, std::ios::beg);

    input.seekg(0, std::ios::end);
    std::streamoff file_size = input.tellg();
    if (data_start + static_cast<std::streamoff>(header.compressed_size) > file_size) {
        std::cerr << "Error: Truncated Z3DS file" << std::endl;
        return false;
    }

    // Parse seekable footer to locate skippable frame size.
    std::streamoff footer_pos = data_start + static_cast<std::streamoff>(header.compressed_size) - 9;
    input.seekg(footer_pos, std::ios::beg);

    auto read_u32 = [&]() -> std::optional<u32> {
        u8 buf[4];
        input.read(reinterpret_cast<char*>(buf), 4);
        if (input.gcount() != 4) {
            return std::nullopt;
        }
        return ReadLE32(buf);
    };

    auto num_frames_opt = read_u32();
    if (!num_frames_opt) {
        std::cerr << "Error: Failed to read frame count from seek table" << std::endl;
        return false;
    }
    u32 num_frames = *num_frames_opt;
    u8 descriptor = 0;
    input.read(reinterpret_cast<char*>(&descriptor), 1);
    auto seekable_magic_opt = read_u32();
    if (!seekable_magic_opt) {
        std::cerr << "Error: Failed to read seek table magic" << std::endl;
        return false;
    }
    u32 seekable_magic = *seekable_magic_opt;

    if (seekable_magic != 0x8F92EAB1) {
        std::cerr << "Error: Invalid seek table magic" << std::endl;
        return false;
    }

    size_t entry_size = (descriptor & 0x80) ? 12 : 8;
    size_t table_size = static_cast<size_t>(num_frames) * entry_size + 9;
    size_t skippable_total = 8 + table_size;

    if (header.compressed_size < skippable_total) {
        std::cerr << "Error: Invalid compressed size in header" << std::endl;
        return false;
    }

    size_t payload_size = static_cast<size_t>(header.compressed_size - skippable_total);

    input.seekg(data_start, std::ios::beg);

    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) {
        std::cerr << "Error: Unable to allocate ZSTD decompression context" << std::endl;
        return false;
    }

    const size_t in_buffer_size = std::max<size_t>(read_chunk_size, 64 * 1024);
    std::vector<u8> in_buffer(in_buffer_size);
    std::vector<u8> out_buffer(1 * 1024 * 1024);

    size_t remaining = payload_size;
    size_t written = 0;

    while (remaining > 0) {
        size_t to_read = std::min(in_buffer_size, remaining);
        input.read(reinterpret_cast<char*>(in_buffer.data()), to_read);
        size_t read_size = input.gcount();
        if (read_size == 0) {
            break;
        }

        remaining -= read_size;

        ZSTD_inBuffer in{in_buffer.data(), read_size, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer out{out_buffer.data(), out_buffer.size(), 0};
            size_t ret = ZSTD_decompressStream(dctx, &out, &in);
            if (ZSTD_isError(ret)) {
                std::cerr << "Decompression error: " << ZSTD_getErrorName(ret) << std::endl;
                ZSTD_freeDCtx(dctx);
                return false;
            }
            if (out.pos > 0) {
                output.write(reinterpret_cast<const char*>(out_buffer.data()), out.pos);
                written += out.pos;
                if (update_callback) {
                    update_callback(written, static_cast<size_t>(header.uncompressed_size));
                }
            }
        }
    }

    ZSTD_outBuffer out{out_buffer.data(), out_buffer.size(), 0};
    ZSTD_inBuffer empty_in{nullptr, 0, 0};
    size_t ret = 0;
    do {
        out.pos = 0;
        ret = ZSTD_decompressStream(dctx, &out, &empty_in);
        if (ZSTD_isError(ret)) {
            std::cerr << "Decompression error while flushing: " << ZSTD_getErrorName(ret) << std::endl;
            ZSTD_freeDCtx(dctx);
            return false;
        }
        if (out.pos > 0) {
            output.write(reinterpret_cast<const char*>(out_buffer.data()), out.pos);
            written += out.pos;
        }
    } while (ret != 0);

    ZSTD_freeDCtx(dctx);

    if (written != header.uncompressed_size) {
        std::cerr << "Warning: Decompressed size mismatch (expected " << header.uncompressed_size
                  << ", got " << written << ")" << std::endl;
    }

    return true;
}
