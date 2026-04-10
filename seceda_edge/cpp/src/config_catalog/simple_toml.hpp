#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace seceda::edge::simple_toml {

struct Value {
    enum class Kind {
        kNone,
        kString,
        kInteger,
        kFloat,
        kBoolean,
        kArray,
        kTable,
    };

    Kind kind = Kind::kNone;
    std::string string_value;
    std::int64_t integer_value = 0;
    double float_value = 0.0;
    bool boolean_value = false;
    std::vector<Value> array_items;
    std::map<std::string, Value> table_items;

    static Value string(std::string value);
    static Value integer(std::int64_t value);
    static Value floating(double value);
    static Value boolean(bool value);
    static Value array(std::vector<Value> items = {});
    static Value table();

    bool is_string() const;
    bool is_integer() const;
    bool is_float() const;
    bool is_boolean() const;
    bool is_array() const;
    bool is_table() const;
};

bool parse_document(const std::string & text, Value & root, std::string & error);

const Value * table_get(const Value & table, const std::string & key);

Value * table_get(Value & table, const std::string & key);

const Value * find_path(const Value & root, const std::vector<std::string> & path);

std::vector<std::string> table_keys(const Value & table);

}  // namespace seceda::edge::simple_toml
