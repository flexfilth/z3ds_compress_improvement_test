#include "frontend_common.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr std::array<const char*, 4> kSupportedExtensions = {".cia", ".cci", ".cxi", ".3dsx"};
constexpr std::array<const char*, 4> kCompressedExtensions = {".zcia", ".zcci", ".zcxi", ".z3dsx"};

} // namespace

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool IsSupportedExtension(std::string_view ext) {
    std::string lower(ext);
    lower = ToLower(lower);
    for (const auto* candidate : kSupportedExtensions) {
        if (lower == candidate) {
            return true;
        }
    }
    return false;
}

bool IsCompressedExtension(std::string_view ext) {
    std::string lower(ext);
    lower = ToLower(lower);
    for (const auto* candidate : kCompressedExtensions) {
        if (lower == candidate) {
            return true;
        }
    }
    return false;
}

std::string GenerateOutputFilename(const std::filesystem::path& input_path) {
    std::string extension = ToLower(input_path.extension().string());
    std::string base_name = input_path.stem().string();

    std::string z3ds_extension;
    if (extension == ".cia") {
        z3ds_extension = ".zcia";
    } else if (extension == ".cci") {
        z3ds_extension = ".zcci";
    } else if (extension == ".cxi") {
        z3ds_extension = ".zcxi";
    } else if (extension == ".3dsx") {
        z3ds_extension = ".z3dsx";
    } else {
        z3ds_extension = ".z3ds";
    }

    return (input_path.parent_path() / (base_name + z3ds_extension)).string();
}

std::string GenerateDecompressedFilename(const std::filesystem::path& input_path) {
    std::string extension = ToLower(input_path.extension().string());
    std::string base_name = input_path.stem().string();

    if (extension == ".zcia") {
        return (input_path.parent_path() / (base_name + ".cia")).string();
    }
    if (extension == ".zcci") {
        return (input_path.parent_path() / (base_name + ".cci")).string();
    }
    if (extension == ".zcxi") {
        return (input_path.parent_path() / (base_name + ".cxi")).string();
    }
    if (extension == ".z3dsx") {
        return (input_path.parent_path() / (base_name + ".3dsx")).string();
    }
    return (input_path.parent_path() / (base_name + ".bin")).string();
}

std::vector<std::filesystem::path> CollectInputFiles(const std::filesystem::path& root, bool recursive) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(root)) {
        return files;
    }

    auto iteratorFactory = [&](auto&& callback) {
        if (recursive) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
                callback(entry);
            }
        } else {
            for (const auto& entry : std::filesystem::directory_iterator(root)) {
                callback(entry);
            }
        }
    };

    iteratorFactory([&](const auto& entry) {
        if (!entry.is_regular_file()) {
            return;
        }
        const auto& path = entry.path();
        if (IsSupportedExtension(path.extension().string())) {
            files.push_back(path);
        }
    });

    std::sort(files.begin(), files.end());
    return files;
}

FileJobReport CompressSingleFile(const std::filesystem::path& input_path,
                                 const std::filesystem::path& output_path,
                                 size_t frame_size_override,
                                 int compression_level,
                                 unsigned int worker_count,
                                 ProgressCallback progress) {
    FileJobReport report;
    report.input_path = input_path;
    report.output_path = output_path;

    if (!std::filesystem::exists(input_path)) {
        report.error_message = "Input file does not exist";
        return report;
    }

    std::error_code ec;
    report.input_size = std::filesystem::file_size(input_path, ec);
    if (ec) {
        report.error_message = "Failed to query input size: " + ec.message();
        return report;
    }

    report.detected_magic = DetectFileMagic(input_path.string());
    size_t frame_size = frame_size_override;
    if (frame_size == 0) {
        frame_size = GetDefaultFrameSize(report.detected_magic, input_path.extension().string());
    }
    report.frame_size_bytes = frame_size;

    auto start_time = std::chrono::high_resolution_clock::now();
    bool success = CompressZ3DSFile(input_path.string(), output_path.string(), report.detected_magic, frame_size,
                                    progress, {}, compression_level, worker_count);
    auto end_time = std::chrono::high_resolution_clock::now();
    report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    if (success) {
        report.success = true;
        report.output_size = std::filesystem::file_size(output_path, ec);
        if (ec) {
            report.error_message = "Compressed but failed to query output size: " + ec.message();
        }
    } else {
        std::filesystem::remove(output_path, ec);
        report.error_message = "Compression failed";
    }

    return report;
}

FileJobReport DecompressSingleFile(const std::filesystem::path& input_path,
                                   const std::filesystem::path& output_path,
                                   ProgressCallback progress) {
    FileJobReport report;
    report.input_path = input_path;
    report.output_path = output_path;

    if (!std::filesystem::exists(input_path)) {
        report.error_message = "Input file does not exist";
        return report;
    }

    std::error_code ec;
    report.input_size = std::filesystem::file_size(input_path, ec);
    if (ec) {
        report.error_message = "Failed to query input size: " + ec.message();
        return report;
    }

    auto start_time = std::chrono::high_resolution_clock::now();
    bool success = DecompressZ3DSFile(input_path.string(), output_path.string(), progress);
    auto end_time = std::chrono::high_resolution_clock::now();
    report.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    if (success) {
        report.success = true;
        report.output_size = std::filesystem::file_size(output_path, ec);
        if (ec) {
            report.error_message = "Decompressed but failed to query output size: " + ec.message();
        }
    } else {
        std::filesystem::remove(output_path, ec);
        report.error_message = "Decompression failed";
    }

    return report;
}
