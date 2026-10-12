#pragma once
#include <switch.h>
#include <cstdarg>
#include <string>
#include <vector>

u64 nowMs();
std::string ipToString(u32 ipNetOrder);
std::string toLower(std::string s);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char sep);

bool base64Decode(const std::string& in, std::vector<u8>& out);
std::string base64Encode(const u8* data, size_t len, bool pad);

// Kopiert hoechstens cap-1 Bytes und schneidet keine UTF-8-Zeichen in der Mitte ab.
void copyUtf8(char* dst, size_t cap, const char* src, size_t len);

inline u16 rd16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }
inline u32 rd32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
inline void wr16(u8* p, u16 v) { p[0] = v >> 8; p[1] = v & 0xFF; }
inline void wr32(u8* p, u32 v) { p[0] = v >> 24; p[1] = (v >> 16) & 0xFF; p[2] = (v >> 8) & 0xFF; p[3] = v & 0xFF; }

void logInit(bool enabled);
void logMsg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

struct Config {
    std::string name = "Switch";
    u8 mac[6] = {};
    float localVolume = 1.0f;
    u32 bufferMs = 350;   // so viel Ton wird vor dem Abspielen gepuffert
    bool log = true;
};
Config& config();
void configLoad();
void configSave();

// Einfacher RAII-Lock fuer libnx-Mutex.
struct Lock {
    Mutex& m;
    explicit Lock(Mutex& mm) : m(mm) { mutexLock(&m); }
    ~Lock() { mutexUnlock(&m); }
};
