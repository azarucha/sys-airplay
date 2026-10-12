#pragma once
#include <switch.h>
#include <map>
#include <string>

#include "player.hpp"

// Eine RTSP-Verbindung vom iPhone/Mac (AirPlay 1 / RAOP).
class RtspConnection {
public:
    RtspConnection(int fd, u32 clientIp, u32 localIp);
    ~RtspConnection();

    int fd() const { return m_fd; }
    u32 clientIp() const { return m_clientIp; }
    const std::string& dacpId() const { return m_dacpId; }
    const std::string& activeRemote() const { return m_activeRemote; }

    // false = Verbindung beendet
    bool onReadable();

private:
    struct Request {
        std::string method, uri;
        std::map<std::string, std::string> headers;  // Schluessel klein geschrieben
        std::string body;
        std::string header(const char* key) const;
    };

    bool processBuffer();
    std::string handle(const Request& r);
    bool parseAnnounce(const std::string& sdp);
    void handleSetParameter(const Request& r);
    bool sendAll(const std::string& data);

    int m_fd;
    u32 m_clientIp;
    u32 m_localIp;
    std::string m_buf;
    bool m_close = false;

    player::Params m_params;
    bool m_announced = false;
    std::string m_dacpId, m_activeRemote;
};
