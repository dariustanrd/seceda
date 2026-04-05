#include "config_catalog/simple_toml.hpp"

#include <toml.hpp>

#include <algorithm>
#include <sstream>
#include <utility>

namespace seceda::edge::simple_toml {
namespace {

std::string format_parse_error(const toml::parse_error & parse_error) {
    std::ostringstream stream;
    stream << parse_error.description();

    const auto begin = parse_error.source().begin;
    if (begin) {
        stream << " on line " << begin.line;
        if (begin.column > 0) {
            stream << ", column " << begin.column;
        }
    }

    return stream.str();
}

std::string unsupported_node_message(const toml::node & node) {
    std::ostringstream stream;
    stream << "Unsupported TOML value type: " << node.type();

    const auto begin = node.source().begin;
    if (begin) {
        stream << " on line " << begin.line;
        if (begin.column > 0) {
            stream << ", column " << begin.column;
        }
    }

    return stream.str();
}

bool convert_node(const toml::node & node, Value & out, std::string & error);

bool convert_array(const toml::array & array, Value & out, std::string & error) {
    std::vector<Value> items;
    items.reserve(array.size());

    for (const auto & item : array) {
        Value converted;
        if (!convert_node(item, converted, error)) {
            return false;
        }
        items.push_back(std::move(converted));
    }

    out = Value::array(std::move(items));
    return true;
}

bool convert_table(const toml::table & table, Value & out, std::string & error) {
    out = Value::table();

    for (const auto & [key, item] : table) {
        Value converted;
        if (!convert_node(item, converted, error)) {
            return false;
        }
        out.table_items.emplace(std::string(key.str()), std::move(converted));
    }

    return true;
}

bool convert_node(const toml::node & node, Value & out, std::string & error) {
    if (const auto * table = node.as_table()) {
        return convert_table(*table, out, error);
    }
    if (const auto * array = node.as_array()) {
        return convert_array(*array, out, error);
    }
    if (const auto value = node.value_exact<std::string>()) {
        out = Value::string(*value);
        return true;
    }
    if (const auto value = node.value_exact<std::int64_t>()) {
        out = Value::integer(*value);
        return true;
    }
    if (const auto value = node.value_exact<double>()) {
        out = Value::floating(*value);
        return true;
    }
    if (const auto value = node.value_exact<bool>()) {
        out = Value::boolean(*value);
        return true;
    }

    error = unsupported_node_message(node);
    return false;
}

}  // namespace

Value Value::string(std::string value) {
    Value out;
    out.kind = Kind::kString;
    out.string_value = std::move(value);
    return out;
}

Value Value::integer(std::int64_t value) {
    Value out;
    out.kind = Kind::kInteger;
    out.integer_value = value;
    return out;
}

Value Value::floating(double value) {
    Value out;
    out.kind = Kind::kFloat;
    out.float_value = value;
    return out;
}

Value Value::boolean(bool value) {
    Value out;
    out.kind = Kind::kBoolean;
    out.boolean_value = value;
    return out;
}

Value Value::array(std::vector<Value> items) {
    Value out;
    out.kind = Kind::kArray;
    out.array_items = std::move(items);
    return out;
}

Value Value::table() {
    Value out;
    out.kind = Kind::kTable;
    return out;
}

bool Value::is_string() const {
    return kind == Kind::kString;
}

bool Value::is_integer() const {
    return kind == Kind::kInteger;
}

bool Value::is_float() const {
    return kind == Kind::kFloat;
}

bool Value::is_boolean() const {
    return kind == Kind::kBoolean;
}

bool Value::is_array() const {
    return kind == Kind::kArray;
}

bool Value::is_table() const {
    return kind == Kind::kTable;
}

bool parse_document(const std::string & text, Value & root, std::string & error) {
    error.clear();

    try {
        const auto parsed = toml::parse(text);
        return convert_table(parsed, root, error);
    } catch (const toml::parse_error & parse_error) {
        error = format_parse_error(parse_error);
        return false;
    }
}

const Value * table_get(const Value & table, const std::string & key) {
    if (!table.is_table()) {
        return nullptr;
    }
    const auto it = table.table_items.find(key);
    return it == table.table_items.end() ? nullptr : &it->second;
}

Value * table_get(Value & table, const std::string & key) {
    if (!table.is_table()) {
        return nullptr;
    }
    const auto it = table.table_items.find(key);
    return it == table.table_items.end() ? nullptr : &it->second;
}

const Value * find_path(const Value & root, const std::vector<std::string> & path) {
    const Value * current = &root;
    for (const auto & segment : path) {
        current = table_get(*current, segment);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

std::vector<std::string> table_keys(const Value & table) {
    std::vector<std::string> keys;
    if (!table.is_table()) {
        return keys;
    }
    keys.reserve(table.table_items.size());
    for (const auto & [key, _] : table.table_items) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

}  // namespace seceda::edge::simple_toml
