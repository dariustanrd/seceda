#pragma once

#include <algorithm>
#include <cctype>
#include <string>

namespace seceda::edge::text_utils {

inline std::string trim_copy(const std::string & value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

inline std::string to_lower_ascii_copy(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

}  // namespace seceda::edge::text_utils
