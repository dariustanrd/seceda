#include "config_catalog/simple_toml.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

namespace seceda::edge::simple_toml {
namespace {

std::string trim_copy(const std::string & value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string strip_comment(const std::string & line) {
    bool in_string = false;
    bool escaped = false;
    int bracket_depth = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && in_string) {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string) {
            if (ch == '[') {
                ++bracket_depth;
                continue;
            }
            if (ch == ']' && bracket_depth > 0) {
                --bracket_depth;
                continue;
            }
            if (ch == '#' && bracket_depth == 0) {
                return line.substr(0, i);
            }
        }
    }
    return line;
}

bool split_key_value(const std::string & line, std::string & key, std::string & value) {
    bool in_string = false;
    bool escaped = false;
    int bracket_depth = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char ch = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && in_string) {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string) {
            if (ch == '[') {
                ++bracket_depth;
                continue;
            }
            if (ch == ']' && bracket_depth > 0) {
                --bracket_depth;
                continue;
            }
            if (ch == '=' && bracket_depth == 0) {
                key = trim_copy(line.substr(0, i));
                value = trim_copy(line.substr(i + 1));
                return !key.empty();
            }
        }
    }
    return false;
}

std::vector<std::string> split_top_level(
    const std::string & text,
    char delimiter,
    std::string & error) {
    std::vector<std::string> out;
    bool in_string = false;
    bool escaped = false;
    int bracket_depth = 0;
    std::size_t token_start = 0;

    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\' && in_string) {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (!in_string) {
            if (ch == '[') {
                ++bracket_depth;
                continue;
            }
            if (ch == ']') {
                --bracket_depth;
                if (bracket_depth < 0) {
                    error = "Unexpected closing bracket";
                    return {};
                }
                continue;
            }
            if (ch == delimiter && bracket_depth == 0) {
                out.push_back(trim_copy(text.substr(token_start, i - token_start)));
                token_start = i + 1;
            }
        }
    }

    if (in_string || bracket_depth != 0) {
        error = "Unterminated string or array";
        return {};
    }

    out.push_back(trim_copy(text.substr(token_start)));
    return out;
}

bool parse_string_literal(const std::string & raw, std::string & out, std::string & error) {
    if (raw.size() < 2 || raw.front() != '"' || raw.back() != '"') {
        error = "Expected a quoted string";
        return false;
    }

    out.clear();
    bool escaped = false;
    for (std::size_t i = 1; i + 1 < raw.size(); ++i) {
        const char ch = raw[i];
        if (!escaped) {
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            out.push_back(ch);
            continue;
        }

        switch (ch) {
            case 'n':
                out.push_back('\n');
                break;
            case 't':
                out.push_back('\t');
                break;
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            default:
                error = std::string("Unsupported escape sequence: \\") + ch;
                return false;
        }
        escaped = false;
    }

    if (escaped) {
        error = "Trailing escape in string";
        return false;
    }

    return true;
}

bool parse_value_inner(const std::string & raw, Value & out, std::string & error);

bool parse_array_literal(const std::string & raw, Value & out, std::string & error) {
    if (raw.size() < 2 || raw.front() != '[' || raw.back() != ']') {
        error = "Expected an array";
        return false;
    }

    const std::string inner = trim_copy(raw.substr(1, raw.size() - 2));
    if (inner.empty()) {
        out = Value::array();
        return true;
    }

    std::vector<Value> items;
    for (const auto & token : split_top_level(inner, ',', error)) {
        if (token.empty()) {
            error = "Array values must not be empty";
            return false;
        }
        Value item;
        if (!parse_value_inner(token, item, error)) {
            return false;
        }
        items.push_back(std::move(item));
    }

    out = Value::array(std::move(items));
    return true;
}

bool parse_value_inner(const std::string & raw, Value & out, std::string & error) {
    const std::string value = trim_copy(raw);
    if (value.empty()) {
        error = "Empty value";
        return false;
    }

    if (value.front() == '"') {
        std::string string_value;
        if (!parse_string_literal(value, string_value, error)) {
            return false;
        }
        out = Value::string(std::move(string_value));
        return true;
    }

    if (value.front() == '[') {
        return parse_array_literal(value, out, error);
    }

    if (value == "true") {
        out = Value::boolean(true);
        return true;
    }
    if (value == "false") {
        out = Value::boolean(false);
        return true;
    }

    try {
        std::size_t consumed = 0;
        if (value.find('.') != std::string::npos) {
            const double parsed = std::stod(value, &consumed);
            if (consumed != value.size()) {
                error = "Invalid floating-point value: " + value;
                return false;
            }
            out = Value::floating(parsed);
            return true;
        }

        const std::int64_t parsed = std::stoll(value, &consumed);
        if (consumed != value.size()) {
            error = "Invalid integer value: " + value;
            return false;
        }
        out = Value::integer(parsed);
        return true;
    } catch (const std::exception &) {
        error = "Unsupported value: " + value;
        return false;
    }
}

Value * ensure_table_child(Value & parent, const std::string & key, std::string & error) {
    auto [it, inserted] = parent.table_items.emplace(key, Value::table());
    if (!inserted && !it->second.is_table()) {
        error = "Key '" + key + "' is not a table";
        return nullptr;
    }
    return &it->second;
}

Value * ensure_array_table_child(Value & parent, const std::string & key, std::string & error) {
    auto [it, inserted] = parent.table_items.emplace(key, Value::array());
    if (!inserted && !it->second.is_array()) {
        error = "Key '" + key + "' is not an array";
        return nullptr;
    }
    it->second.array_items.push_back(Value::table());
    return &it->second.array_items.back();
}

std::vector<std::string> split_header_path(const std::string & text, std::string & error) {
    const auto parts = split_top_level(text, '.', error);
    if (!error.empty()) {
        return {};
    }

    for (const auto & part : parts) {
        if (part.empty()) {
            error = "Empty path segment";
            return {};
        }
    }
    return parts;
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
    root = Value::table();
    Value * current_table = &root;

    std::istringstream lines(text);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        const std::string trimmed = trim_copy(strip_comment(line));
        if (trimmed.empty()) {
            continue;
        }

        if (trimmed.front() == '[') {
            const bool is_array_table =
                trimmed.size() >= 4 && trimmed.rfind("[[", 0) == 0 &&
                trimmed.substr(trimmed.size() - 2) == "]]";
            const bool is_table =
                trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']';
            if (!is_table) {
                error = "Invalid table header on line " + std::to_string(line_number);
                return false;
            }

            const std::string inner = is_array_table
                ? trim_copy(trimmed.substr(2, trimmed.size() - 4))
                : trim_copy(trimmed.substr(1, trimmed.size() - 2));
            auto path = split_header_path(inner, error);
            if (!error.empty()) {
                error = "Invalid table header on line " + std::to_string(line_number) + ": " + error;
                return false;
            }

            Value * table = &root;
            for (std::size_t i = 0; i < path.size(); ++i) {
                const bool is_last = i + 1 == path.size();
                if (is_last && is_array_table) {
                    table = ensure_array_table_child(*table, path[i], error);
                } else {
                    table = ensure_table_child(*table, path[i], error);
                }

                if (table == nullptr) {
                    error =
                        "Invalid table header on line " + std::to_string(line_number) + ": " + error;
                    return false;
                }
            }

            current_table = table;
            continue;
        }

        std::string key;
        std::string value;
        if (!split_key_value(trimmed, key, value)) {
            error = "Expected key=value on line " + std::to_string(line_number);
            return false;
        }

        Value parsed;
        if (!parse_value_inner(value, parsed, error)) {
            error = "Invalid value for '" + key + "' on line " + std::to_string(line_number) +
                ": " + error;
            return false;
        }

        current_table->table_items[key] = std::move(parsed);
    }

    return true;
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
