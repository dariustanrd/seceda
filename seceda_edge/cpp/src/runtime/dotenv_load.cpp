#include "runtime/dotenv_load.hpp"

#include "file_utils/read_text_file.hpp"
#include "text_utils/normalize.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

namespace seceda::edge {
namespace {

namespace fs = std::filesystem;
namespace text = seceda::edge::text_utils;

bool is_valid_env_key(const std::string & key) {
    if (key.empty()) {
        return false;
    }
    const unsigned char c0 = static_cast<unsigned char>(key[0]);
    if (!(std::isalpha(c0) != 0 || c0 == '_')) {
        return false;
    }
    for (std::size_t i = 1; i < key.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(key[i]);
        if (!(std::isalnum(c) != 0 || c == '_')) {
            return false;
        }
    }
    return true;
}

void apply_env_pair(const std::string & key, const std::string & value) {
#if defined(_WIN32) && defined(_MSC_VER)
    if (std::getenv(key.c_str()) != nullptr) {
        return;
    }
    _putenv_s(key.c_str(), value.c_str());
#else
    ::setenv(key.c_str(), value.c_str(), 0);
#endif
}

std::string parse_double_quoted_value(std::string raw) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        return raw;
    }
    raw = raw.substr(1, raw.size() - 2);
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            switch (raw[i + 1]) {
            case 'n':
                out += '\n';
                ++i;
                continue;
            case 'r':
                out += '\r';
                ++i;
                continue;
            case 't':
                out += '\t';
                ++i;
                continue;
            case '\\':
                out += '\\';
                ++i;
                continue;
            case '"':
                out += '"';
                ++i;
                continue;
            default:
                break;
            }
        }
        out += raw[i];
    }
    return out;
}

std::string strip_export_prefix(std::string line) {
    static const char kPrefix[] = "export ";
    line = text::trim_copy(line);
    if (line.size() >= sizeof(kPrefix) - 1 &&
        line.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0) {
        return text::trim_copy(line.substr(sizeof(kPrefix) - 1));
    }
    return line;
}

bool parse_and_apply_dotenv_content(std::string content, std::string & error) {
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB && static_cast<unsigned char>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    std::istringstream stream(content);
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(stream, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        line = text::trim_copy(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        line = strip_export_prefix(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            error = "Invalid .env line " + std::to_string(line_no) + ": expected KEY=value";
            return false;
        }

        std::string key = text::trim_copy(line.substr(0, eq));
        std::string value = text::trim_copy(line.substr(eq + 1));

        if (!is_valid_env_key(key)) {
            error = "Invalid .env line " + std::to_string(line_no) + ": bad variable name";
            return false;
        }

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = parse_double_quoted_value(value);
        }

        apply_env_pair(key, value);
    }
    return true;
}

void collect_dotenv_candidates(
    const std::string & resolved_config_path,
    bool no_config_file,
    std::vector<fs::path> & out) {
    if (!no_config_file && !resolved_config_path.empty()) {
        fs::path dir = fs::path(resolved_config_path).parent_path();
        for (int depth = 0; depth < 32; ++depth) {
            out.push_back(dir / ".env");
            if (dir.empty() || !dir.has_parent_path()) {
                break;
            }
            fs::path parent = dir.parent_path();
            if (parent == dir) {
                break;
            }
            dir = parent;
        }
    }

    std::error_code ec;
    const fs::path cwd = fs::current_path(ec);
    if (!ec) {
        out.push_back(cwd / ".env");
    }
}

}  // namespace

bool load_dotenv_for_config_context(
    const std::string & resolved_config_path,
    bool no_config_file,
    std::string & error) {
    error.clear();

    std::vector<fs::path> candidates;
    collect_dotenv_candidates(resolved_config_path, no_config_file, candidates);

    for (const fs::path & candidate : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(candidate, ec) || ec) {
            continue;
        }

        std::string content;
        if (!file_utils::read_text_file(candidate.string(), content, error)) {
            return false;
        }
        if (!parse_and_apply_dotenv_content(std::move(content), error)) {
            return false;
        }
        return true;
    }

    return true;
}

}  // namespace seceda::edge
