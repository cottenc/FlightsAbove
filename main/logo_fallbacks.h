#pragma once

#include <cstddef>
#include <cstdint>

struct BuiltinLogoFallback {
    const char* key;
    const uint8_t* data;
    size_t size;
};

const BuiltinLogoFallback* builtin_logo_fallbacks(size_t* count);
