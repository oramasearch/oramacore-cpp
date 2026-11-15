// SPDX-License-Identifier: MIT

#pragma once
// oramacore::Result<T> = std::expected<T, oramacore::Error>
// No exceptions cross the public API boundary (Orama Platform D7/D16).
#include <expected>
#include <string>
#include <utility>

namespace oramacore {

struct Error {
    std::string message;
};

template <typename T>
using Result = std::expected<T, Error>;

inline std::unexpected<Error> fail(std::string msg) {
    return std::unexpected(Error{std::move(msg)});
}

}  // namespace oramacore
