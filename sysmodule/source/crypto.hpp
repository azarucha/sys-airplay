#pragma once
#include <switch.h>
#include <string>
#include <vector>

bool rsaInit();
// AES-Sitzungsschluessel aus "a=rsaaeskey:" entschluesseln (RSA-OAEP/SHA-1).
bool rsaDecryptAesKey(const std::vector<u8>& cipher, std::vector<u8>& keyOut);
// Antwort auf "Apple-Challenge" erzeugen (RSA-PKCS#1-v1.5-Signatur, base64 ohne '=').
std::string rsaAppleResponse(const std::vector<u8>& challenge, u32 localIpNetOrder, const u8 mac[6]);
