#include "frontend_common.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <system_error>

namespace {

void RenderProgressBar(const std::string& label, std::size_t processed, std::size_t total) {
    double percentage = total > 0 ? static_cast<double>(processed) / static_cast<double>(total) * 100.0 : 100.0;
    constexpr int bar_width = 50;
    int filled = static_cast<int>((percentage / 100.0) * bar_width);

    std::cout << '\r' << "[" << label << "] Progress: [";
    for (int i = 0; i < bar_width; ++i) {
        if (i < filled) {
            std::cout << '=';
        } else if (i == filled) {
            std::cout << '>';
        } else {
            std::cout << ' ';
        }
    }
    std::cout << "] " << std::fixed << std::setprecision(1) << percentage << "% (" << processed
              << "/" << total << " bytes)";
    std::cout.flush();
}

ProgressCallback MakeProgressCallback(std::string label) {
    if (label.empty()) {
        label = "current";
    }
    return [label = std::move(label)](std::size_t processed, std::size_t total) {
        RenderProgressBar(label, processed, total);
    };
}

void ShowUsage(const char* program_name) {
    std::cout << "Z3DS ROM Compressor - CLI Version\n";
    std::cout << "Based on Azahar Emulator's compression format\n\n";
    std::cout << "Usage: " << program_name << " <input> [output_file] [options]\n\n";
    std::cout << "Arguments:\n";
    std::cout << "  input         Input ROM file or directory (when --batch is used)\n";
    std::cout << "  output_file   Output Z3DS file (single-file mode only)\n\n";
    std::cout << "Options:\n";
    std::cout << "  --batch              Treat input as a directory and process all supported files\n";
    std::cout << "  --no-recursive       Don't recurse into subdirectories while batching\n";
    std::cout << "  --delete-source      Delete the source file after successful compression\n";
    std::cout << "  --frame-size SIZE    Set compression frame size in bytes (default: auto)\n";
    std::cout << "  --level N            ZSTD compression level (default: 15)\n";
    std::cout << "  --threads N          Number of worker threads (default: CPU cores)\n";
    std::cout << "  --decompress         Treat the input as a .zcia/.zcci/... file and restore the original\n";
    std::cout << "  --help, -h           Show this help message\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << program_name << " game.cia\n";
    std::cout << "  " << program_name << " game.cci game_compressed.zcci\n";
    std::cout << "  " << program_name << " /games --batch --delete-source\n";
    std::cout << "  " << program_name << " archive.zcia --decompress\n";
}

void PrintCompressionReport(const FileJobReport& report) {
    if (!report.success) {
        std::cerr << '\n' << "Compression failed: " << report.error_message << "\n";
        return;
    }

    double ratio = 0.0;
    if (report.input_size > 0) {
        ratio = static_cast<double>(report.output_size) / static_cast<double>(report.input_size) * 100.0;
    }

    std::cout << '\n';
    std::cout << "Compression completed successfully!\n";
    std::cout << "Original size: " << report.input_size << " bytes\n";
    std::cout << "Compressed size: " << report.output_size << " bytes\n";
    std::cout << "Compression ratio: " << std::fixed << std::setprecision(1) << ratio << "%\n";
    std::cout << "Frame size: " << report.frame_size_bytes << " bytes\n";
    std::cout << "Time taken: " << report.duration.count() << " ms\n";
}

void PrintDecompressionReport(const FileJobReport& report) {
    if (!report.success) {
        std::cerr << '\n' << "Decompression failed: " << report.error_message << "\n";
        return;
    }

    std::cout << '\n';
    std::cout << "Decompression completed successfully!\n";
    std::cout << "Restored size: " << report.output_size << " bytes\n";
    std::cout << "Time taken: " << report.duration.count() << " ms\n";
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        ShowUsage(argv[0]);
        return 1;
    }

    std::string input_path_argument;
    std::string output_file_argument;
    size_t frame_size = 0; // 0 means auto-detect
    int compression_level = 15;
    unsigned int worker_count = std::thread::hardware_concurrency();
    if (worker_count == 0) {
        worker_count = 1;
    }

    bool batch_mode = false;
    bool recursive = true;
    bool delete_source = false;
    bool decompress_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            ShowUsage(argv[0]);
            return 0;
        } else if (arg == "--frame-size") {
            if (i + 1 < argc) {
                frame_size = std::stoull(argv[++i]);
            } else {
                std::cerr << "Error: --frame-size requires a value\n";
                return 1;
            }
        } else if (arg == "--level") {
            if (i + 1 < argc) {
                compression_level = std::stoi(argv[++i]);
            } else {
                std::cerr << "Error: --level requires a value\n";
                return 1;
            }
        } else if (arg == "--threads") {
            if (i + 1 < argc) {
                worker_count = static_cast<unsigned int>(std::stoul(argv[++i]));
                if (worker_count == 0) {
                    worker_count = 1;
                }
            } else {
                std::cerr << "Error: --threads requires a value\n";
                return 1;
            }
        } else if (arg == "--batch") {
            batch_mode = true;
        } else if (arg == "--no-recursive") {
            recursive = false;
        } else if (arg == "--delete-source") {
            delete_source = true;
        } else if (arg == "--decompress") {
            decompress_mode = true;
        } else if (input_path_argument.empty()) {
            input_path_argument = arg;
        } else if (output_file_argument.empty() && !batch_mode) {
            output_file_argument = arg;
        } else {
            std::cerr << "Error: Too many arguments\n";
            ShowUsage(argv[0]);
            return 1;
        }
    }

    if (input_path_argument.empty()) {
        std::cerr << "Error: No input path provided\n";
        ShowUsage(argv[0]);
        return 1;
    }

    if (batch_mode && decompress_mode) {
        std::cerr << "Error: --batch cannot be combined with --decompress\n";
        return 1;
    }

    if (batch_mode) {
        if (!output_file_argument.empty()) {
            std::cerr << "Error: Output file cannot be specified when using --batch\n";
            return 1;
        }

        std::filesystem::path input_dir(input_path_argument);
        if (!std::filesystem::exists(input_dir) || !std::filesystem::is_directory(input_dir)) {
            std::cerr << "Error: Input path must be an existing directory when using --batch\n";
            return 1;
        }

        auto files = CollectInputFiles(input_dir, recursive);
        if (files.empty()) {
            std::cout << "No supported files found under " << input_dir << '\n';
            return 0;
        }

        std::cout << "Found " << files.size() << " files to compress" << (recursive ? " (recursive)" : "") << "\n";

        size_t success_count = 0;
        size_t failure_count = 0;

        for (const auto& file : files) {
            auto output_path = std::filesystem::path(GenerateOutputFilename(file));
            auto progress = MakeProgressCallback(file.filename().string());
            auto report = CompressSingleFile(file, output_path, frame_size, compression_level, worker_count, progress);
            if (report.success) {
                ++success_count;
                PrintCompressionReport(report);
                if (delete_source) {
                    std::error_code ec;
                    std::filesystem::remove(file, ec);
                    if (ec) {
                        std::cerr << "Warning: Failed to delete " << file << ": " << ec.message() << '\n';
                    }
                }
            } else {
                ++failure_count;
                std::cerr << '\n' << "Failed to compress " << file << ": " << report.error_message << '\n';
            }
        }

        std::cout << "\nBatch summary: " << success_count << " succeeded, " << failure_count << " failed" << '\n';
        return failure_count == 0 ? 0 : 2;
    }

    std::filesystem::path input_file(input_path_argument);
    if (!std::filesystem::exists(input_file) || !std::filesystem::is_regular_file(input_file)) {
        std::cerr << "Error: Input file does not exist or is not a regular file\n";
        return 1;
    }

    std::filesystem::path output_file;
    if (output_file_argument.empty()) {
        output_file = decompress_mode ? GenerateDecompressedFilename(input_file)
                                      : GenerateOutputFilename(input_file);
    } else {
        output_file = output_file_argument;
    }

    bool success = false;
    if (decompress_mode) {
        auto progress = MakeProgressCallback(input_file.filename().string());
        auto report = DecompressSingleFile(input_file, output_file, progress);
        PrintDecompressionReport(report);
        success = report.success;
    } else {
        auto progress = MakeProgressCallback(input_file.filename().string());
        auto report = CompressSingleFile(input_file, output_file, frame_size, compression_level, worker_count, progress);
        PrintCompressionReport(report);
        success = report.success;
    }
    return success ? 0 : 1;
}
