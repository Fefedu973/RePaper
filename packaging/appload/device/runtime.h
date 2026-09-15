#pragma once
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

namespace repaper {
inline int framebufferKey() {
    const char *value = std::getenv("QTFB_KEY");
    if (!value || !*value || std::strspn(value, "0123456789") != std::strlen(value))
        throw std::runtime_error("Launch this application through AppLoad (missing or invalid QTFB_KEY).");
    errno = 0;
    const unsigned long key = std::strtoul(value, nullptr, 10);
    if (errno || key > INT_MAX)
        throw std::runtime_error("Invalid AppLoad framebuffer key.");
    return static_cast<int>(key);
}
inline void fail(const char *operation) {
    throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
}
inline void setEnvironment(const char *name, const std::string &value) {
    if (setenv(name, value.c_str(), 1) != 0) fail("setenv");
}
}
