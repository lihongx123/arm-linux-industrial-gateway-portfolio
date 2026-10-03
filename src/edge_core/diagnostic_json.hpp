#pragma once

#include <cstdio>
#include <string>

namespace mqmgateway::edge {
inline std::string diagnosticJsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) {
            char escaped[7];
            std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(c));
            out += escaped;
        } else out += static_cast<char>(c);
    }
    return out;
}
}  // namespace mqmgateway::edge
