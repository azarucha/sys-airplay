#include "util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#define CONFIG_DIR  "sdmc:/config/sys-airplay"
#define CONFIG_FILE CONFIG_DIR "/config.ini"
#define LOG_FILE    CONFIG_DIR "/log.txt"

u64 nowMs() {
    return armTicksToNs(armGetSystemTick()) / 1000000ULL;
}

std::string ipToString(u32 ip) {
    const u8* b = (const u8*)&ip;
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    return buf;
}

std::string toLower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool base64Decode(const std::string& in, std::vector<u8>& out) {
    out.clear();
    u32 acc = 0;
    int bits = 0;
    for (char c : in) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=' || c == ' ' || c == '\r' || c == '\n') continue;
        else return false;
        acc = (acc << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((acc >> bits) & 0xFF);
        }
    }
    return true;
}

std::string base64Encode(const u8* data, size_t len, bool pad) {
    std::string out;
    size_t i = 0;
    while (i + 2 < len) {
        u32 v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
        i += 3;
    }
    if (i < len) {
        u32 v = data[i] << 16;
        if (i + 1 < len) v |= data[i + 1] << 8;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        if (i + 1 < len) out += kB64[(v >> 6) & 63];
        else if (pad) out += '=';
        if (pad) out += '=';
    }
    return out;
}

void copyUtf8(char* dst, size_t cap, const char* src, size_t len) {
    if (cap == 0) return;
    size_t n = len < cap - 1 ? len : cap - 1;
    if (n < len) {
        // nicht mitten in einer Multibyte-Sequenz abschneiden
        while (n > 0 && ((u8)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

// ---------------------------------------------------------------- Log

static Mutex g_logLock;
static bool g_logEnabled = false;

void logInit(bool enabled) {
    g_logEnabled = enabled;
    if (!enabled) return;
    struct stat st;
    if (stat(LOG_FILE, &st) == 0 && st.st_size > 256 * 1024) remove(LOG_FILE);
}

void logMsg(const char* fmt, ...) {
    if (!g_logEnabled) return;
    Lock l(g_logLock);
    FILE* f = fopen(LOG_FILE, "a");
    if (!f) return;
    u64 t = nowMs();
    fprintf(f, "[%6llu.%03llu] ", (unsigned long long)(t / 1000), (unsigned long long)(t % 1000));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ---------------------------------------------------------------- Config

static Config g_config;

Config& config() { return g_config; }

void configLoad() {
    mkdir("sdmc:/config", 0777);
    mkdir(CONFIG_DIR, 0777);

    bool haveMac = false;
    FILE* f = fopen(CONFIG_FILE, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            std::string s = trim(line);
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string key = toLower(trim(s.substr(0, eq)));
            std::string val = trim(s.substr(eq + 1));
            if (key == "name" && !val.empty()) {
                g_config.name = val.substr(0, 50);
            } else if (key == "volume") {
                float v = strtof(val.c_str(), nullptr);
                g_config.localVolume = v < 0 ? 0 : (v > 1 ? 1 : v);
            } else if (key == "buffer_ms") {
                long v = strtol(val.c_str(), nullptr, 10);
                g_config.bufferMs = v < 100 ? 100 : (v > 2000 ? 2000 : (u32)v);
            } else if (key == "log") {
                g_config.log = val == "1" || toLower(val) == "true";
            } else if (key == "mac" && val.size() == 17) {
                unsigned m[6];
                if (sscanf(val.c_str(), "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
                    for (int i = 0; i < 6; i++) g_config.mac[i] = (u8)m[i];
                    haveMac = true;
                }
            }
        }
        fclose(f);
    }

    if (!haveMac) {
        // Eigene, stabile Pseudo-MAC fuer den AirPlay-Namen (lokal administriert).
        randomGet(g_config.mac, sizeof(g_config.mac));
        g_config.mac[0] = (g_config.mac[0] & 0xFC) | 0x02;
        configSave();
    }
}

void configSave() {
    FILE* f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    const u8* m = g_config.mac;
    fprintf(f, "# sys-airplay Einstellungen\n");
    fprintf(f, "# Name, unter dem die Switch auf dem iPhone erscheint\n");
    fprintf(f, "name=%s\n", g_config.name.c_str());
    fprintf(f, "# Lautstaerke auf der Switch (0.0 - 1.0)\n");
    fprintf(f, "volume=%.2f\n", g_config.localVolume);
    fprintf(f, "# Puffer in Millisekunden. Bei Aussetzern im WLAN erhoehen (z.B. 600).\n");
    fprintf(f, "buffer_ms=%u\n", g_config.bufferMs);
    fprintf(f, "# Protokoll nach sdmc:/config/sys-airplay/log.txt (1 = an, 0 = aus)\n");
    fprintf(f, "log=%d\n", g_config.log ? 1 : 0);
    fprintf(f, "mac=%02X:%02X:%02X:%02X:%02X:%02X\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    fclose(f);
}
