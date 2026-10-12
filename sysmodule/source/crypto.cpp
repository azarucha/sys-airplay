#include "crypto.hpp"
#include "util.hpp"
#include "airport_key.h"

#include <cstring>
#include <mbedtls/rsa.h>
#include <mbedtls/version.h>

static mbedtls_rsa_context g_rsa;
static bool g_rsaReady = false;
static const size_t kKeyBytes = 256;

static int rng(void*, unsigned char* out, size_t len) {
    randomGet(out, len);
    return 0;
}

bool rsaInit() {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_rsa_init(&g_rsa);
#else
    mbedtls_rsa_init(&g_rsa, MBEDTLS_RSA_PKCS_V15, 0);
#endif
    int rc = mbedtls_rsa_import_raw(&g_rsa, kKeyN, sizeof(kKeyN), kKeyP, sizeof(kKeyP), kKeyQ, sizeof(kKeyQ),
                                    kKeyD, sizeof(kKeyD), kKeyE, sizeof(kKeyE));
    if (rc == 0) rc = mbedtls_rsa_complete(&g_rsa);
    if (rc != 0) {
        logMsg("RSA-Init fehlgeschlagen: -0x%04x", -rc);
        return false;
    }
    g_rsaReady = true;
    return true;
}

static bool rsaPrivate(const u8* in, u8* out) {
    if (!g_rsaReady) return false;
    int rc = mbedtls_rsa_private(&g_rsa, rng, nullptr, in, out);
    if (rc != 0) logMsg("RSA-Operation fehlgeschlagen: -0x%04x", -rc);
    return rc == 0;
}

static void mgf1Sha1(const u8* seed, size_t seedLen, u8* mask, size_t maskLen) {
    u8 block[kKeyBytes + 4];
    u8 hash[SHA1_HASH_SIZE];
    memcpy(block, seed, seedLen);
    for (u32 counter = 0, done = 0; done < maskLen; counter++) {
        wr32(block + seedLen, counter);
        sha1CalculateHash(hash, block, seedLen + 4);
        size_t n = maskLen - done < SHA1_HASH_SIZE ? maskLen - done : SHA1_HASH_SIZE;
        memcpy(mask + done, hash, n);
        done += n;
    }
}

bool rsaDecryptAesKey(const std::vector<u8>& cipher, std::vector<u8>& keyOut) {
    if (cipher.size() != kKeyBytes) {
        logMsg("rsaaeskey hat falsche Laenge %zu", cipher.size());
        return false;
    }
    u8 em[kKeyBytes];
    if (!rsaPrivate(cipher.data(), em)) return false;

    // OAEP-Dekodierung (SHA-1, leeres Label), siehe RFC 8017 7.1.2
    const size_t h = SHA1_HASH_SIZE;
    const size_t dbLen = kKeyBytes - h - 1;
    u8 seed[SHA1_HASH_SIZE], db[kKeyBytes], mask[kKeyBytes];
    if (em[0] != 0) return false;
    mgf1Sha1(em + 1 + h, dbLen, mask, h);
    for (size_t i = 0; i < h; i++) seed[i] = em[1 + i] ^ mask[i];
    mgf1Sha1(seed, h, mask, dbLen);
    for (size_t i = 0; i < dbLen; i++) db[i] = em[1 + h + i] ^ mask[i];

    u8 emptyHash[SHA1_HASH_SIZE];
    sha1CalculateHash(emptyHash, "", 0);
    if (memcmp(db, emptyHash, h) != 0) {
        logMsg("OAEP: Label-Hash passt nicht");
        return false;
    }
    size_t i = h;
    while (i < dbLen && db[i] == 0) i++;
    if (i >= dbLen || db[i] != 0x01) return false;
    i++;
    keyOut.assign(db + i, db + dbLen);
    return true;
}

std::string rsaAppleResponse(const std::vector<u8>& challenge, u32 localIp, const u8 mac[6]) {
    // challenge(16) | lokale IPv4(4) | MAC(6), mit Nullen auf 32 Byte aufgefuellt
    u8 msg[32] = {};
    size_t n = challenge.size() > 16 ? 16 : challenge.size();
    memcpy(msg, challenge.data(), n);
    memcpy(msg + 16, &localIp, 4);
    memcpy(msg + 20, mac, 6);

    // PKCS#1 v1.5, Blocktyp 1: 00 01 FF..FF 00 | msg
    u8 em[kKeyBytes];
    memset(em, 0xFF, sizeof(em));
    em[0] = 0x00;
    em[1] = 0x01;
    em[kKeyBytes - sizeof(msg) - 1] = 0x00;
    memcpy(em + kKeyBytes - sizeof(msg), msg, sizeof(msg));

    u8 sig[kKeyBytes];
    if (!rsaPrivate(em, sig)) return "";
    return base64Encode(sig, sizeof(sig), false);
}
