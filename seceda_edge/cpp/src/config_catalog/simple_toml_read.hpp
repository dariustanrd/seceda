#pragma once

#include "config_catalog/simple_toml.hpp"

#include <string>
#include <vector>

namespace seceda::edge::simple_toml_read {

using simple_toml::Value;

inline bool read_optional_string(
    const Value & table,
    const char * key,
    std::string & out,
    std::string & error) {
    const Value * const value = simple_toml::table_get(table, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_string()) {
        error = std::string("Expected string for key: ") + key;
        return false;
    }
    out = value->string_value;
    return true;
}

inline bool read_required_string(
    const Value & table,
    const char * key,
    std::string & out,
    std::string & error,
    bool allow_empty = false) {
    if (!read_optional_string(table, key, out, error)) {
        return false;
    }
    if (!allow_empty && out.empty()) {
        error = std::string("Missing required string key: ") + key;
        return false;
    }
    return true;
}

template <typename T>
bool read_optional_integer(const Value & table, const char * key, T & out, std::string & error) {
    const Value * const value = simple_toml::table_get(table, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_integer()) {
        error = std::string("Expected integer for key: ") + key;
        return false;
    }
    out = static_cast<T>(value->integer_value);
    return true;
}

inline bool read_optional_float(const Value & table, const char * key, float & out, std::string & error) {
    const Value * const value = simple_toml::table_get(table, key);
    if (value == nullptr) {
        return true;
    }
    if (value->is_integer()) {
        out = static_cast<float>(value->integer_value);
        return true;
    }
    if (value->is_float()) {
        out = static_cast<float>(value->float_value);
        return true;
    }
    error = std::string("Expected number for key: ") + key;
    return false;
}

inline bool read_optional_bool(const Value & table, const char * key, bool & out, std::string & error) {
    const Value * const value = simple_toml::table_get(table, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_boolean()) {
        error = std::string("Expected boolean for key: ") + key;
        return false;
    }
    out = value->boolean_value;
    return true;
}

inline bool read_optional_string_array(
    const Value & table,
    const char * key,
    std::vector<std::string> & out,
    std::string & error) {
    const Value * const value = simple_toml::table_get(table, key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_array()) {
        error = std::string("Expected array for key: ") + key;
        return false;
    }

    out.clear();
    for (const auto & item : value->array_items) {
        if (!item.is_string()) {
            error = std::string("Expected string array for key: ") + key;
            return false;
        }
        out.push_back(item.string_value);
    }
    return true;
}

}  // namespace seceda::edge::simple_toml_read
