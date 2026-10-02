#include "Hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <fstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace hs::crypto {

std::optional<std::string> sha256File(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;

    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return std::nullopt;
    }

    DWORD hashSize = 0, cbData = 0;
    if (BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hashSize, sizeof(hashSize), &cbData, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    std::vector<char> buf(64 * 1024);
    bool ok = true;
    while (in.read(buf.data(), buf.size()) || in.gcount() > 0) {
        std::streamsize got = in.gcount();
        if (BCryptHashData(hash, (PUCHAR)buf.data(), (ULONG)got, 0) != 0) { ok = false; break; }
    }

    std::vector<BYTE> digest(hashSize);
    if (ok && BCryptFinishHash(hash, digest.data(), hashSize, 0) != 0) ok = false;

    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (!ok) return std::nullopt;

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(hashSize * 2);
    for (BYTE b : digest) {
        out.push_back(hex[(b >> 4) & 0xF]);
        out.push_back(hex[b & 0xF]);
    }
    return out;
}

} // namespace hs::crypto
