#pragma once

#include <nlohmann/json.hpp>

#include <type_traits>

namespace seceda::edge::json_utils {

using json = nlohmann::json;

template <typename T>
bool read_optional_integer(const json & object, const char * key, T & out) {
    static_assert(std::is_integral_v<T>, "read_optional_integer requires an integral output type");

    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_number_integer()) {
        return false;
    }
    out = it->get<T>();
    return true;
}

inline bool read_optional_float(const json & object, const char * key, float & out) {
    const auto it = object.find(key);
    if (it == object.end()) {
        return true;
    }
    if (!it->is_number()) {
        return false;
    }
    out = it->get<float>();
    return true;
}

}  // namespace seceda::edge::json_utils
