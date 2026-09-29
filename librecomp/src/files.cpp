#include "files.hpp"

#include <iterator>
#include <vector>

constexpr std::u8string_view backup_suffix = u8".bak";
constexpr std::u8string_view temp_suffix = u8".temp";

// std::filesystem::copy_file fails on Vita's newlib (it creates the target but copies nothing); stream copy works there.
static void copy_file_overwrite(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {
#ifdef __vita__
    std::ifstream in{ from, std::ios_base::binary };
    std::vector<char> data{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    std::ofstream out{ to, std::ios_base::binary | std::ios_base::trunc };
    if (!in.good() && !in.eof()) {
        ec = std::make_error_code(std::errc::io_error);
        return;
    }
    out.write(data.data(), (std::streamsize)data.size());
    out.close();
    ec = out.good() ? std::error_code{} : std::make_error_code(std::errc::io_error);
#else
    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
#endif
}

std::ifstream recomp::open_input_backup_file(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::filesystem::path backup_path{filepath};
    backup_path += backup_suffix;
    return std::ifstream{backup_path, mode};
}

std::ifstream recomp::open_input_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::ifstream ret{filepath, mode};

    // Check if the file failed to open and open the corresponding backup file instead if so.
    if (!ret.good()) {
        return open_input_backup_file(filepath, mode);
    }

    return ret;
}

std::ofstream recomp::open_output_file_with_backup(const std::filesystem::path& filepath, std::ios_base::openmode mode) {
    std::filesystem::path temp_path{filepath};
    temp_path += temp_suffix;
    std::ofstream temp_file_out{ temp_path, mode };

    return temp_file_out;
}

bool recomp::finalize_output_file_with_backup(const std::filesystem::path& filepath) {
    std::filesystem::path backup_path{filepath};
    backup_path += backup_suffix;

    std::filesystem::path temp_path{filepath};
    temp_path += temp_suffix;

    std::error_code ec;
    if (std::filesystem::exists(filepath, ec)) {
        copy_file_overwrite(filepath, backup_path, ec);
        if (ec) {
            return false;
        }
    }
    copy_file_overwrite(temp_path, filepath, ec);
    if (ec) {
        return false;
    }
    std::filesystem::remove(temp_path, ec);
    return true;
}
