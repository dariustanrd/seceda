#pragma once

#include "runtime/contracts.hpp"

#include <string>

namespace seceda::edge {

class RuntimeConfigParser {
public:
    static bool parse(
        int argc,
        char ** argv,
        DaemonConfig & config,
        std::string & error,
        bool & show_help);

    static std::string help_text(const char * program_name);
};

}  // namespace seceda::edge
