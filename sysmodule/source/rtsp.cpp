#include "rtsp.hpp"
#include "audio.hpp"
#include "crypto.hpp"
#include "state.hpp"
#include "util.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

static const size_t kMaxRequest = 1024 * 1024;

RtspConnection::RtspConnection(int fd, u32 clientIp, u32 localIp)
    : m_fd(fd), m_clientIp(clientIp), m_localIp(localIp) {
    m_params.clientIp = clientIp;
    logMsg("RTSP: Verbindung von %s", ipToString(clientIp).c_str());
    state::setClient(true, clientIp);
}

RtspConnection::~RtspConnection() {
    player::teardown();
    if (m_fd >= 0) close(m_fd);
    state::setClient(false, 0);
    logMsg("RTSP: Verbindung beendet");
}

std::string RtspConnection::Request::header(const char* key) const {
    auto it = headers.find(key);
    return it == headers.end() ? std::string() : it->second;
}

bool RtspConnection::sendAll(const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        int n = send(m_fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) return false;
        off += n;
    }
    return true;
}

bool RtspConnection::onReadable() {
    char tmp[4096];
    int n = recv(m_fd, tmp, sizeof(tmp), 0);
    if (n <= 0) return false;
    m_buf.append(tmp, n);
    if (m_buf.size() > kMaxRequest) {
        logMsg("RTSP: Anfrage zu gross, Verbindung wird getrennt");
        return false;
    }
    return processBuffer() && !m_close;
}

bool RtspConnection::processBuffer() {
    while (true) {
        size_t end = m_buf.find("\r\n\r\n");
        if (end == std::string::npos) return true;

        Request r;
        std::vector<std::string> lines = split(m_buf.substr(0, end), '\n');
        std::vector<std::string> first = split(trim(lines[0]), ' ');
        if (first.size() < 2) return false;
        r.method = first[0];
        r.uri = first[1];
        for (size_t i = 1; i < lines.size(); i++) {
            size_t c = lines[i].find(':');
            if (c == std::string::npos) continue;
            r.headers[toLower(trim(lines[i].substr(0, c)))] = trim(lines[i].substr(c + 1));
        }
        size_t bodyLen = strtoul(r.header("content-length").c_str(), nullptr, 10);
        if (m_buf.size() < end + 4 + bodyLen) return true;  // Body noch nicht komplett
        r.body = m_buf.substr(end + 4, bodyLen);
        m_buf.erase(0, end + 4 + bodyLen);

        if (!sendAll(handle(r))) return false;
    }
}

std::string RtspConnection::handle(const Request& r) {
    int code = 200;
    std::string extra, body, ctype;

    std::string challenge = r.header("apple-challenge");
    if (!challenge.empty()) {
        std::vector<u8> c;
        if (base64Decode(challenge, c)) {
            std::string resp = rsaAppleResponse(c, m_localIp, config().mac);
            if (!resp.empty()) extra += "Apple-Response: " + resp + "\r\n";
        }
    }
    if (!r.header("dacp-id").empty()) m_dacpId = r.header("dacp-id");
    if (!r.header("active-remote").empty()) m_activeRemote = r.header("active-remote");

    if (r.method == "OPTIONS") {
        extra += "Public: ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER\r\n";
    } else if (r.method == "ANNOUNCE") {
        m_announced = parseAnnounce(r.body);
        if (!m_announced) code = 415;
        state::clearTrack();
    } else if (r.method == "SETUP") {
        std::string tr = r.header("transport");
        for (auto& part : split(tr, ';')) {
            size_t eq = part.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(part.substr(0, eq));
            u16 v = (u16)strtoul(part.substr(eq + 1).c_str(), nullptr, 10);
            if (k == "control_port") m_params.clientControlPort = v;
            else if (k == "timing_port") m_params.clientTimingPort = v;
        }
        u16 dp = 0, cp = 0, tp = 0;
        if (m_announced && player::setup(m_params, dp, cp, tp)) {
            char buf[200];
            snprintf(buf, sizeof(buf),
                     "Transport: RTP/AVP/UDP;unicast;mode=record;server_port=%u;control_port=%u;timing_port=%u\r\n"
                     "Session: 1\r\n",
                     dp, cp, tp);
            extra += buf;
        } else {
            code = 500;
        }
    } else if (r.method == "RECORD") {
        // nichts zu tun, der Empfang laeuft bereits seit SETUP
    } else if (r.method == "FLUSH") {
        std::string info = r.header("rtp-info");
        size_t p = info.find("seq=");
        if (p != std::string::npos) player::flush(true, (u16)strtoul(info.c_str() + p + 4, nullptr, 10));
        else player::flush(false, 0);
    } else if (r.method == "TEARDOWN") {
        player::teardown();
        extra += "Connection: close\r\n";
        m_close = true;
    } else if (r.method == "SET_PARAMETER") {
        handleSetParameter(r);
    } else if (r.method == "GET_PARAMETER") {
        if (r.body.find("volume") != std::string::npos) {
            char buf[64];
            snprintf(buf, sizeof(buf), "volume: %.6f\r\n", audio::senderVolumeDb());
            body = buf;
            ctype = "text/parameters";
        }
    } else if (r.method == "POST" && r.uri == "/feedback") {
        // AirPlay-2-Herzschlag, einfach bestaetigen
    } else {
        logMsg("RTSP: nicht unterstuetzt: %s %s", r.method.c_str(), r.uri.c_str());
        code = 501;
    }

    const char* text = code == 200 ? "OK" : code == 415 ? "Unsupported Media Type" : code == 500 ? "Internal Server Error" : "Not Implemented";
    std::string resp = "RTSP/1.0 " + std::to_string(code) + " " + text + "\r\n";
    resp += "CSeq: " + r.header("cseq") + "\r\n";
    resp += "Server: AirTunes/105.1\r\n";
    resp += "Audio-Jack-Status: connected; type=analog\r\n";
    resp += extra;
    if (!body.empty()) {
        resp += "Content-Type: " + ctype + "\r\n";
        resp += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    resp += "\r\n";
    resp += body;
    return resp;
}

bool RtspConnection::parseAnnounce(const std::string& sdp) {
    player::Params p;
    p.clientIp = m_clientIp;
    p.clientControlPort = m_params.clientControlPort;
    p.clientTimingPort = m_params.clientTimingPort;
    bool haveKey = false, haveIv = false, codecOk = false;

    for (auto& raw : split(sdp, '\n')) {
        std::string line = trim(raw);
        if (line.rfind("a=rtpmap:", 0) == 0) {
            if (line.find("AppleLossless") != std::string::npos) {
                p.alac = true;
                codecOk = true;
            } else if (line.find("L16") != std::string::npos) {
                p.alac = false;
                codecOk = true;
            } else {
                logMsg("ANNOUNCE: Codec nicht unterstuetzt: %s", line.c_str());
            }
        } else if (line.rfind("a=fmtp:", 0) == 0) {
            std::vector<std::string> t = split(line.substr(7), ' ');
            for (size_t i = 1; i < t.size() && i <= 11; i++) p.fmtp[i - 1] = strtoul(t[i].c_str(), nullptr, 10);
        } else if (line.rfind("a=rsaaeskey:", 0) == 0) {
            std::vector<u8> enc, key;
            if (base64Decode(line.substr(12), enc) && rsaDecryptAesKey(enc, key) && key.size() == 16) {
                memcpy(p.aesKey, key.data(), 16);
                haveKey = true;
            } else {
                logMsg("ANNOUNCE: AES-Schluessel konnte nicht entschluesselt werden");
                return false;
            }
        } else if (line.rfind("a=aesiv:", 0) == 0) {
            std::vector<u8> iv;
            if (base64Decode(line.substr(8), iv) && iv.size() == 16) {
                memcpy(p.aesIv, iv.data(), 16);
                haveIv = true;
            }
        }
    }
    if (!codecOk) return false;
    p.encrypted = haveKey && haveIv;
    m_params = p;
    logMsg("ANNOUNCE: %s, %u Frames/Paket, %s", p.alac ? "ALAC" : "L16", p.fmtp[0],
           p.encrypted ? "verschluesselt" : "unverschluesselt");
    return true;
}

static void parseDmap(const u8* p, size_t len, std::string& title, std::string& artist, std::string& album) {
    size_t off = 0;
    while (off + 8 <= len) {
        const u8* tag = p + off;
        u32 l = rd32(p + off + 4);
        off += 8;
        if (l > len - off) break;
        if (!memcmp(tag, "mlit", 4)) parseDmap(p + off, l, title, artist, album);
        else if (!memcmp(tag, "minm", 4)) title.assign((const char*)p + off, l);
        else if (!memcmp(tag, "asar", 4)) artist.assign((const char*)p + off, l);
        else if (!memcmp(tag, "asal", 4)) album.assign((const char*)p + off, l);
        off += l;
    }
}

void RtspConnection::handleSetParameter(const Request& r) {
    std::string ct = toLower(r.header("content-type"));
    if (ct.rfind("text/parameters", 0) == 0) {
        for (auto& raw : split(r.body, '\n')) {
            std::string line = trim(raw);
            if (line.rfind("volume:", 0) == 0) {
                float db = strtof(line.c_str() + 7, nullptr);
                audio::setSenderVolumeDb(db);
            } else if (line.rfind("progress:", 0) == 0) {
                std::vector<std::string> v = split(trim(line.substr(9)), '/');
                if (v.size() == 3)
                    state::setProgress((u32)strtoul(v[0].c_str(), nullptr, 10), (u32)strtoul(v[1].c_str(), nullptr, 10),
                                       (u32)strtoul(v[2].c_str(), nullptr, 10));
            }
        }
    } else if (ct.rfind("application/x-dmap-tagged", 0) == 0) {
        std::string title, artist, album;
        parseDmap((const u8*)r.body.data(), r.body.size(), title, artist, album);
        state::setTrack(title, artist, album);
        logMsg("Titel: %s - %s", artist.c_str(), title.c_str());
    }
    // Cover-Bilder (image/*) werden nicht angefordert und ignoriert
}
