#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rea_jni_args {
// JNI jvalue layout. JNI jlong is long long, including on Android AArch64.
using JLong = long long;
union JValue {
    std::uint8_t z;
    std::int8_t b;
    std::uint16_t c;
    std::int16_t s;
    std::int32_t i;
    JLong j;
    float f;
    double d;
    void* l;
};
static_assert(sizeof(JLong) == 8 && sizeof(JValue) == 8,
              "This decoder requires the JNI 64-bit jvalue layout");

struct Argument {
    char type = 0; // B/C/S/Z/I/J/F/D/L; arrays are references (L).
    JValue value{};
};
struct Descriptor {
    std::vector<char> parameters;
    char result = 0;
};
using Arguments = std::vector<Argument>;

inline bool FieldType(const char*& cursor, char& type, bool allowVoid,
                      std::string& error)
{
    if (!*cursor) { error = "truncated field descriptor"; return false; }
    char token = *cursor++;
    if (token == 'V') {
        if (!allowVoid) { error = "void parameter/array element"; return false; }
        type = token; return true;
    }
    if (token == '[') {
        unsigned dimensions = 1;
        while (*cursor == '[') { ++cursor; ++dimensions; }
        if (dimensions > 255) { error = "array has more than 255 dimensions"; return false; }
        char component = 0;
        if (!FieldType(cursor, component, false, error)) return false;
        type = 'L'; return true;
    }
    if (token == 'L') {
        const char* start = cursor;
        while (*cursor && *cursor != ';') {
            if (*cursor == '[' || *cursor == '(' || *cursor == ')' || *cursor == '.') {
                error = "invalid object descriptor"; return false;
            }
            ++cursor;
        }
        if (*cursor != ';' || cursor == start) { error = "unterminated/empty object descriptor"; return false; }
        ++cursor; type = 'L'; return true;
    }
    switch (token) {
    case 'B': case 'C': case 'S': case 'Z': case 'I': case 'J': case 'F': case 'D':
        type = token; return true;
    default: error = "unknown field descriptor"; return false;
    }
}

inline bool Parse(const char* signature, Descriptor& descriptor, std::string& error)
{
    descriptor = Descriptor{}; error.clear();
    if (!signature || *signature != '(') { error = "missing method descriptor '('"; return false; }
    const char* cursor = signature + 1;
    unsigned slots = 0;
    while (*cursor && *cursor != ')') {
        char type = 0;
        if (!FieldType(cursor, type, false, error)) return false;
        slots += type == 'J' || type == 'D' ? 2 : 1;
        // This accepts the descriptor's maximum static parameter slot count.
        if (slots > 255) { error = "too many parameter slots"; return false; }
        descriptor.parameters.push_back(type);
    }
    if (*cursor != ')') { error = "missing method descriptor ')'"; return false; }
    ++cursor;
    if (!FieldType(cursor, descriptor.result, true, error)) return false;
    if (*cursor) { error = "trailing method descriptor data"; return false; }
    return true;
}

// Caller passes the pointer to the real va_list, never a scan of guessed object
// values. Validate the WHOLE descriptor before touching the list. va_copy keeps
// the caller's cursor intact, including AArch64's separate GP and FP save areas.
inline bool DecodeV(const char* signature, va_list* source, Arguments& arguments,
                    std::string& error)
{
    arguments.clear(); Descriptor descriptor;
    if (!Parse(signature, descriptor, error)) return false;
    if (descriptor.parameters.empty()) return true; // no read/copy, source may be null
    if (!source) { error = "null va_list for nonempty arguments"; return false; }
    arguments.resize(descriptor.parameters.size());
    va_list copy;
    va_copy(copy, *source);
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto& argument = arguments[index]; argument.type = descriptor.parameters[index];
        switch (argument.type) {
        case 'L': argument.value.l = va_arg(copy, void*); break;
        case 'Z': argument.value.z = static_cast<std::uint8_t>(va_arg(copy, int)); break;
        case 'B': argument.value.b = static_cast<std::int8_t>(va_arg(copy, int)); break;
        case 'C': argument.value.c = static_cast<std::uint16_t>(va_arg(copy, int)); break;
        case 'S': argument.value.s = static_cast<std::int16_t>(va_arg(copy, int)); break;
        case 'I': argument.value.i = va_arg(copy, int); break;
        case 'J': argument.value.j = va_arg(copy, JLong); break;
        case 'F': argument.value.f = static_cast<float>(va_arg(copy, double)); break;
        case 'D': argument.value.d = va_arg(copy, double); break;
        }
    }
    va_end(copy);
    return true;
}

// A calls use jvalue slots, with no C default promotions. For integration with
// a real jvalue header, copy its eight-byte slots into JValue or use this exact
// union as the host's jvalue definition. Do not cast unrelated object types.
inline bool DecodeA(const char* signature, const JValue* source, Arguments& arguments,
                    std::string& error)
{
    arguments.clear(); Descriptor descriptor;
    if (!Parse(signature, descriptor, error)) return false;
    if (descriptor.parameters.empty()) return true;
    if (!source) { error = "null jvalue array for nonempty arguments"; return false; }
    arguments.resize(descriptor.parameters.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        auto& argument = arguments[index]; argument.type = descriptor.parameters[index];
        switch (argument.type) {
        case 'L': argument.value.l = source[index].l; break;
        case 'Z': argument.value.z = source[index].z; break;
        case 'B': argument.value.b = source[index].b; break;
        case 'C': argument.value.c = source[index].c; break;
        case 'S': argument.value.s = source[index].s; break;
        case 'I': argument.value.i = source[index].i; break;
        case 'J': argument.value.j = source[index].j; break;
        case 'F': argument.value.f = source[index].f; break;
        case 'D': argument.value.d = source[index].d; break;
        }
    }
    return true;
}
} // namespace rea_jni_args
