#pragma once

#include <fstream>
#include <sstream>
#include <string>

namespace seceda::edge::file_utils {

inline bool read_text_file(const std::string & path, std::string & content, std::string & error) {
    std::ifstream in(path);
    if (!in) {
        error = "Unable to open file: " + path;
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    content = buffer.str();
    return true;
}

}  // namespace seceda::edge::file_utils
