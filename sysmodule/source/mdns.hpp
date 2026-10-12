#pragma once
#include <switch.h>
#include <string>
#include <vector>

// Minimaler mDNS-Responder: macht die Switch als "_raop._tcp" (AirPlay-Lautsprecher)
// im WLAN sichtbar und findet den Fernsteuerungsdienst (DACP) des iPhones.
class Mdns {
public:
    bool open(u32 ipNetOrder);
    void close();
    int fd() const { return m_fd; }
    bool failed() const { return m_failed; }

    void setService(const std::string& instance, const std::string& host, u16 port,
                    const std::vector<std::string>& txt);
    void onReadable();
    void tick(u64 nowMs);
    void goodbye();

    void lookupDacp(const std::string& dacpId);
    void stopDacp();
    u16 dacpPort() const { return m_dacpPort; }

private:
    size_t buildResponse(u8* out, size_t cap, u32 ttlScale) const;
    void sendTo(const u8* data, size_t len, u32 ip, u16 port);
    void sendDacpQuery();

    int m_fd = -1;
    bool m_failed = false;
    u32 m_ip = 0;

    std::string m_instance, m_host;
    std::string m_svcLower, m_instLower, m_hostLower;
    u16 m_port = 0;
    std::vector<std::string> m_txt;

    int m_announceCount = 0;
    u64 m_nextAnnounce = 0;
    u64 m_lastResponse = 0;

    std::string m_dacpLabel, m_dacpLower;
    u16 m_dacpPort = 0;
    u64 m_nextDacpQuery = 0;
};
