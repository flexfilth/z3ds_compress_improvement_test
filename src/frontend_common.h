#pragma once

#include "z3ds_compression.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct FileJobReport {
    bool success = false;
    bool skipped = false;
    std::string error_message;
    std::filesystem::path input_path;
    std::filesystem::path output_path;
    std::array<u8, 4> detected_magic{};
    size_t frame_size_bytes = 0;
    std::uintmax_t input_size = 0;
    std::uintmax_t output_size = 0;
    std::chrono::milliseconds duration{0};
};

std::string ToLower(std::string value);
bool IsSupportedExtension(std::string_view ext);
bool IsCompressedExtension(std::string_view ext);
std::string GenerateOutputFilename(const std::filesystem::path& input_path);
std::string GenerateDecompressedFilename(const std::filesystem::path& input_path);
std::vector<std::filesystem::path> CollectInputFiles(const std::filesystem::path& root, bool recursive);

FileJobReport CompressSingleFile(const std::filesystem::path& input_path,
                                 const std::filesystem::path& output_path,
                                 size_t frame_size_override,
                                 int compression_level,
                                 unsigned int worker_count,
                                 ProgressCallback progress);

FileJobReport DecompressSingleFile(const std::filesystem::path& input_path,
                                   const std::filesystem::path& output_path,
                                   ProgressCallback progress);
