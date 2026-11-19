#pragma once

#include <cstdint>

struct FsrVersion
{
    uint32_t major = 0, minor = 0, patch = 0;
    bool valid = false;
};

inline FsrVersion ParseFsrProviderVersion(const char* name)
{
    if (!name) return {};
    const char* p = name;
    uint32_t values[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (*p < '0' || *p > '9') return {};
        do {
            const unsigned digit = (unsigned)(*p++ - '0');
            if (values[i] > (1000000u - digit) / 10u) return {};
            values[i] = values[i] * 10u + digit;
        } while (*p >= '0' && *p <= '9');
        if (i == 1 && *p != '.') break; // patch number is optional
        if (i < 2 && *p++ != '.') return {};
    }
    if (*p && *p != ' ' && *p != '-' && *p != '+') return {};
    return { values[0], values[1], values[2], true };
}

inline bool NewerFsr4Provider(FsrVersion candidate, FsrVersion current)
{
    if (!candidate.valid || candidate.major != 4) return false;
    if (!current.valid) return true;
    if (candidate.minor != current.minor) return candidate.minor > current.minor;
    return candidate.patch > current.patch;
}
