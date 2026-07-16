#pragma once
#include <Arduino.h>
#include <esp_random.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

#include "MeshTypes.h"

// Application-layer crypto for mesh config pushes (issue #252): an ephemeral
// X25519 ECDH exchange between the two devices in a push derives a one-time
// AES-256-GCM key, so mqttPassword/githubToken/apPassword/WiFi passwords never
// cross the air in the clear. No pre-shared key or pairing step is needed, and
// the derived key is forward-secret since the ephemeral ECDH keys are never
// persisted beyond a single push.
namespace MeshCrypto {

static constexpr uint8_t AES_KEY_LEN = 32;  // AES-256
static constexpr uint8_t NONCE_LEN = 12;    // AES-GCM nonce
static constexpr uint8_t TAG_LEN = 16;      // AES-GCM auth tag

inline int _rng(void*, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}

// Starts an ephemeral ECDH exchange, writing our public key into `pubOut`.
// `ctx` is left set up for finishExchange() and must eventually be freed by it
// (or, on failure here, is already freed).
inline bool beginExchange(mbedtls_ecdh_context& ctx, uint8_t pubOut[ECDH_PUBKEY_LEN]) {
    mbedtls_ecdh_init(&ctx);
    size_t olen = 0;
    bool ok = mbedtls_ecdh_setup(&ctx, MBEDTLS_ECP_DP_CURVE25519) == 0 &&
              mbedtls_ecdh_make_public(&ctx, &olen, pubOut, ECDH_PUBKEY_LEN, _rng, nullptr) == 0 &&
              olen == ECDH_PUBKEY_LEN;
    if (!ok) mbedtls_ecdh_free(&ctx);
    return ok;
}

// Completes the exchange given the peer's public key, deriving a 32-byte
// AES-256 key via SHA-256(shared secret). Always frees `ctx`.
inline bool finishExchange(mbedtls_ecdh_context& ctx, const uint8_t theirPub[ECDH_PUBKEY_LEN],
                           uint8_t outKey[AES_KEY_LEN]) {
    uint8_t secret[32];
    size_t olen = 0;
    bool ok = mbedtls_ecdh_read_public(&ctx, theirPub, ECDH_PUBKEY_LEN) == 0 &&
              mbedtls_ecdh_calc_secret(&ctx, &olen, secret, sizeof(secret), _rng, nullptr) == 0;
    if (ok) {
        mbedtls_sha256_context sha;
        mbedtls_sha256_init(&sha);
        mbedtls_sha256_starts(&sha, 0);
        mbedtls_sha256_update(&sha, secret, olen);
        mbedtls_sha256_finish(&sha, outKey);
        mbedtls_sha256_free(&sha);
    }
    memset(secret, 0, sizeof(secret));
    mbedtls_ecdh_free(&ctx);
    return ok;
}

// Encrypts `plain` (len bytes) under `key`, writing a NONCE_LEN-byte random
// nonce + ciphertext + TAG_LEN-byte tag into `out`
// (must be >= len + NONCE_LEN + TAG_LEN bytes).
inline bool encrypt(const uint8_t key[AES_KEY_LEN], const uint8_t* plain, size_t len, uint8_t* out,
                    size_t& outLen) {
    uint8_t* nonce = out;
    uint8_t* cipher = out + NONCE_LEN;
    uint8_t* tag = cipher + len;
    esp_fill_random(nonce, NONCE_LEN);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, AES_KEY_LEN * 8) == 0 &&
              mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, len, nonce, NONCE_LEN, nullptr,
                                        0, plain, cipher, TAG_LEN, tag) == 0;
    mbedtls_gcm_free(&gcm);
    if (ok) outLen = NONCE_LEN + len + TAG_LEN;
    return ok;
}

// Decrypts a nonce+ciphertext+tag blob produced by encrypt(). `outPlain` must
// be >= len - NONCE_LEN - TAG_LEN bytes. Fails on a wrong key or tampered data.
inline bool decrypt(const uint8_t key[AES_KEY_LEN], const uint8_t* in, size_t len,
                    uint8_t* outPlain, size_t& outLen) {
    if (len < (size_t)(NONCE_LEN + TAG_LEN)) return false;
    const uint8_t* nonce = in;
    const uint8_t* cipher = in + NONCE_LEN;
    size_t cipherLen = len - NONCE_LEN - TAG_LEN;
    const uint8_t* tag = cipher + cipherLen;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, AES_KEY_LEN * 8) == 0 &&
              mbedtls_gcm_auth_decrypt(&gcm, cipherLen, nonce, NONCE_LEN, nullptr, 0, tag, TAG_LEN,
                                       cipher, outPlain) == 0;
    mbedtls_gcm_free(&gcm);
    if (ok) outLen = cipherLen;
    return ok;
}

}  // namespace MeshCrypto
