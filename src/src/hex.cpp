#include "libs/hex.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace libs {

std::string to_hex(const Bytes& bytes) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i) out << ' ';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Bytes from_hex(std::string_view text) {
    Bytes out;
    int high = -1;
    for (const char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' || c == ':') continue;
        const int v = hex_value(c);
        if (v < 0) throw std::invalid_argument(std::string("invalid hex character: ") + c);
        if (high < 0) high = v;
        else { out.push_back(static_cast<std::uint8_t>((high << 4) | v)); high = -1; }
    }
    if (high >= 0) throw std::invalid_argument("hex string has an odd number of digits");
    return out;
}

}  // namespace libs
