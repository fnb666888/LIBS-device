#include "libs/error.hpp"

namespace libs {

const char* to_string(ErrorKind kind) noexcept {
    switch (kind) {
        case ErrorKind::none: return "none";
        case ErrorKind::invalid_argument: return "invalid_argument";
        case ErrorKind::unsupported: return "unsupported";
        case ErrorKind::protocol: return "protocol";
        case ErrorKind::transport: return "transport";
        case ErrorKind::device: return "device";
        case ErrorKind::timeout: return "timeout";
        case ErrorKind::internal: return "internal";
    }
    return "internal";
}

}  // namespace libs
