// SPDX-License-Identifier: GPL-3.0-only
//
// gs.key handling. The file layout matches wfb-ng's keygen so a key pair
// generated here can be dropped straight onto an air unit, and a gs.key taken
// off an existing SBC ground station works here unmodified.

#include <sodium.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "openipc/wfb_receiver.h"

namespace openipc::gs {
namespace {

constexpr size_t kKeyFileSize = crypto_box_SECRETKEYBYTES + crypto_box_PUBLICKEYBYTES;

bool WriteFile(const std::string& path, const uint8_t* data, size_t size, std::string* error) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        if (error != nullptr) {
            *error = "unable to open " + path + " for writing";
        }
        return false;
    }
    const bool ok = std::fwrite(data, size, 1, file) == 1;
    std::fclose(file);
    if (!ok && error != nullptr) {
        *error = "short write to " + path;
    }
    return ok;
}

}  // namespace

KeyValidation ValidateGsKey(const std::string& path) {
    KeyValidation result;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        result.error = "cannot open key file";
        return result;
    }

    uint8_t buffer[kKeyFileSize + 1];
    const size_t read = std::fread(buffer, 1, sizeof(buffer), file);
    std::fclose(file);

    if (read < kKeyFileSize) {
        result.error = "key file is too short: expected " + std::to_string(kKeyFileSize) +
                       " bytes, got " + std::to_string(read);
        return result;
    }
    if (read > kKeyFileSize) {
        result.error = "key file is too long: expected exactly " + std::to_string(kKeyFileSize) +
                       " bytes";
        return result;
    }
    result.ok = true;
    return result;
}

bool GenerateKeyPair(const std::string& gs_key_path, const std::string& drone_key_path,
                     std::string* error) {
    if (sodium_init() < 0) {
        if (error != nullptr) {
            *error = "libsodium initialisation failed";
        }
        return false;
    }

    uint8_t gs_public[crypto_box_PUBLICKEYBYTES];
    uint8_t gs_secret[crypto_box_SECRETKEYBYTES];
    uint8_t drone_public[crypto_box_PUBLICKEYBYTES];
    uint8_t drone_secret[crypto_box_SECRETKEYBYTES];

    if (crypto_box_keypair(gs_public, gs_secret) != 0 ||
        crypto_box_keypair(drone_public, drone_secret) != 0) {
        if (error != nullptr) {
            *error = "key generation failed";
        }
        return false;
    }

    // gs.key: the ground station's own secret, then the air unit's public key.
    uint8_t gs_file[kKeyFileSize];
    std::memcpy(gs_file, gs_secret, sizeof(gs_secret));
    std::memcpy(gs_file + sizeof(gs_secret), drone_public, sizeof(drone_public));

    // drone.key is the mirror image, and must be copied onto the air unit.
    uint8_t drone_file[kKeyFileSize];
    std::memcpy(drone_file, drone_secret, sizeof(drone_secret));
    std::memcpy(drone_file + sizeof(drone_secret), gs_public, sizeof(gs_public));

    const bool ok = WriteFile(gs_key_path, gs_file, sizeof(gs_file), error) &&
                    WriteFile(drone_key_path, drone_file, sizeof(drone_file), error);

    sodium_memzero(gs_secret, sizeof(gs_secret));
    sodium_memzero(drone_secret, sizeof(drone_secret));
    sodium_memzero(gs_file, sizeof(gs_file));
    sodium_memzero(drone_file, sizeof(drone_file));
    return ok;
}

}  // namespace openipc::gs
