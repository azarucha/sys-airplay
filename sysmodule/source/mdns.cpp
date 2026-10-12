#include "mdns.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static const u32 kMdnsGroup = htonl(0xE00000FB);  // 224.0.0.251
static const u16 kMdnsPort = 5353;

namespace {

struct Writer {
    u8* p;
    size_t cap;
    size_t len = 0;
    bool ok = true;

    void byte(u8 v) {
        if (len + 1 > cap) { ok = false; return; }
        p[len++] = v;
    }
    void u16be(u16 v) { byte(v >> 8); byte(v & 0xFF); }
    void u32be(u32 v) { u16be(v >> 16); u16be(v & 0xFFFF); }
    void bytes(const void* d, size_t n) {
        if (len + n > cap) { ok = false; return; }
        memcpy(p + len, d, n);
        len += n;
    }
    void name(const std::vector<std::string>& labels) {
        for (auto& l : labels) {
            size_t n = l.size() > 63 ? 63 : l.size();
            byte((u8)n);
            bytes(l.data(), n);
        }
        byte(0);
    }
    // Beginnt einen Record und gibt die Position des RDLENGTH-Felds zurueck.
    size_t record(const std::vector<std::string>& owner, u16 type, u16 cls, u32 ttl) {
        name(owner);
        u16be(type);
        u16be(cls);
        u32be(ttl);
        size_t at = len;
        u16be(0);
        return at;
    }
    void endRecord(size_t at) {
        if (!ok) return;
        u16 rdlen = (u16)(len - at - 2);
        p[at] = rdlen >> 8;
        p[at + 1] = rdlen & 0xFF;
    }
};

bool readName(const u8* pkt, size_t len, size_t& off, std::string& out) {
    out.clear();
    size_t pos = off;
    bool jumped = false;
    for (int hops = 0; hops < 32;) {
        if (pos >= len) return false;
        u8 l = pkt[pos];
        if ((l & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return false;
            size_t ptr = ((l & 0x3F) << 8) | pkt[pos + 1];
            if (!jumped) off = pos + 2;
            jumped = true;
            pos = ptr;
            hops++;
            continue;
        }
        if (l == 0) {
            if (!jumped) off = pos + 1;
            return true;
        }
        if (pos + 1 + l > len) return false;
        if (!out.empty()) out += '.';
        out.append((const char*)pkt + pos + 1, l);
        pos += 1 + l;
    }
    return false;
}

}  // namespace

bool Mdns::open(u32 ip) {
    close();
    m_ip = ip;
    m_failed = false;

    m_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) {
        logMsg("mDNS: socket() fehlgeschlagen");
        m_failed = true;
        return false;
    }
    int one = 1;
    setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(kMdnsPort);
    if (bind(m_fd, (sockaddr*)&a, sizeof(a)) != 0) {
        logMsg("mDNS: Port 5353 belegt");
        ::close(m_fd);
        m_fd = -1;
        m_failed = true;
        return false;
    }

    ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = kMdnsGroup;
    mreq.imr_interface.s_addr = ip;
    if (setsockopt(m_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0)
        logMsg("mDNS: Multicast-Gruppe nicht beigetreten, nur periodische Ankuendigungen");

    u8 ttl = 255;
    setsockopt(m_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    m_announceCount = 0;
    m_nextAnnounce = 0;
    m_dacpPort = 0;
    m_nextDacpQuery = 0;
    logMsg("mDNS bereit auf %s", ipToString(ip).c_str());
    return true;
}

void Mdns::close() {
    if (m_fd >= 0) ::close(m_fd);
    m_fd = -1;
}

void Mdns::setService(const std::string& instance, const std::string& host, u16 port,
                      const std::vector<std::string>& txt) {
    m_instance = instance;
    m_host = host;
    m_port = port;
    m_txt = txt;
    m_svcLower = "_raop._tcp.local";
    m_instLower = toLower(instance) + "." + m_svcLower;
    m_hostLower = toLower(host) + ".local";
    m_announceCount = 0;
    m_nextAnnounce = 0;
}

size_t Mdns::buildResponse(u8* out, size_t cap, u32 ttlScale) const {
    const std::vector<std::string> svc = {"_raop", "_tcp", "local"};
    const std::vector<std::string> inst = {m_instance, "_raop", "_tcp", "local"};
    const std::vector<std::string> host = {m_host, "local"};
    const std::vector<std::string> meta = {"_services", "_dns-sd", "_udp", "local"};

    Writer w{out, cap};
    w.u16be(0);       // ID
    w.u16be(0x8400);  // Antwort, autoritativ
    w.u16be(0);       // Fragen
    w.u16be(5);       // Antworten
    w.u16be(0);
    w.u16be(0);

    size_t at = w.record(svc, 12, 0x0001, 4500 * ttlScale);  // PTR
    w.name(inst);
    w.endRecord(at);

    at = w.record(inst, 33, 0x8001, 120 * ttlScale);  // SRV
    w.u16be(0);
    w.u16be(0);
    w.u16be(m_port);
    w.name(host);
    w.endRecord(at);

    at = w.record(inst, 16, 0x8001, 4500 * ttlScale);  // TXT
    for (auto& t : m_txt) {
        w.byte((u8)t.size());
        w.bytes(t.data(), t.size());
    }
    w.endRecord(at);

    at = w.record(host, 1, 0x8001, 120 * ttlScale);  // A
    w.bytes(&m_ip, 4);
    w.endRecord(at);

    at = w.record(meta, 12, 0x0001, 4500 * ttlScale);  // Dienst-Aufzaehlung
    w.name(svc);
    w.endRecord(at);

    return w.ok ? w.len : 0;
}

void Mdns::sendTo(const u8* data, size_t len, u32 ip, u16 port) {
    if (m_fd < 0 || len == 0) return;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    if (sendto(m_fd, data, len, 0, (sockaddr*)&a, sizeof(a)) < 0) {
        logMsg("mDNS: sendto fehlgeschlagen, Netzwerk wird neu aufgebaut");
        m_failed = true;
    }
}

void Mdns::goodbye() {
    u8 buf[1024];
    size_t n = buildResponse(buf, sizeof(buf), 0);  // TTL 0 = "Dienst weg"
    sendTo(buf, n, kMdnsGroup, kMdnsPort);
}

void Mdns::tick(u64 now) {
    if (m_fd < 0 || m_instance.empty()) return;
    if (now >= m_nextAnnounce) {
        u8 buf[1024];
        size_t n = buildResponse(buf, sizeof(buf), 1);
        sendTo(buf, n, kMdnsGroup, kMdnsPort);
        // Start: 1 s, 2 s, 4 s ... danach jede Minute (falls Anfragen uns nicht erreichen)
        m_announceCount++;
        u64 delay = m_announceCount < 4 ? (1000ULL << (m_announceCount - 1)) : 60000ULL;
        m_nextAnnounce = now + delay;
    }
    if (!m_dacpLabel.empty() && m_dacpPort == 0 && now >= m_nextDacpQuery) {
        sendDacpQuery();
        m_nextDacpQuery = now + 3000;
    }
}

void Mdns::lookupDacp(const std::string& dacpId) {
    std::string label = "iTunes_Ctrl_" + dacpId;
    if (label == m_dacpLabel) return;
    m_dacpLabel = label;
    m_dacpLower = toLower(label) + "._dacp._tcp.local";
    m_dacpPort = 0;
    m_nextDacpQuery = 0;
}

void Mdns::stopDacp() {
    m_dacpLabel.clear();
    m_dacpLower.clear();
    m_dacpPort = 0;
}

void Mdns::sendDacpQuery() {
    u8 buf[512];
    Writer w{buf, sizeof(buf)};
    w.u16be(0);
    w.u16be(0);
    w.u16be(1);
    w.u16be(0);
    w.u16be(0);
    w.u16be(0);
    w.name({m_dacpLabel, "_dacp", "_tcp", "local"});
    w.u16be(33);      // SRV
    w.u16be(0x8001);  // IN, Antwort bitte per Unicast
    if (w.ok) sendTo(buf, w.len, kMdnsGroup, kMdnsPort);
}

void Mdns::onReadable() {
    u8 buf[1500];
    sockaddr_in from = {};
    socklen_t fl = sizeof(from);
    int n = recvfrom(m_fd, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
    if (n < 12) return;
    size_t len = (size_t)n;

    u16 flags = rd16(buf + 2);
    u16 qd = rd16(buf + 4), an = rd16(buf + 6), ns = rd16(buf + 8), ar = rd16(buf + 10);
    size_t off = 12;
    std::string name;

    if (flags & 0x8000) {
        // Antwort von jemand anderem: nach dem DACP-SRV-Record des iPhones suchen
        if (m_dacpLower.empty() || m_dacpPort) return;
        for (int i = 0; i < qd; i++) {
            if (!readName(buf, len, off, name)) return;
            off += 4;
        }
        for (int i = 0; i < an + ns + ar; i++) {
            if (!readName(buf, len, off, name) || off + 10 > len) return;
            u16 type = rd16(buf + off);
            u16 rdlen = rd16(buf + off + 8);
            off += 10;
            if (off + rdlen > len) return;
            if (type == 33 && rdlen >= 6 && toLower(name) == m_dacpLower) {
                m_dacpPort = rd16(buf + off + 4);
                logMsg("DACP-Fernsteuerung gefunden auf Port %u", m_dacpPort);
            }
            off += rdlen;
        }
        return;
    }

    if (m_instance.empty()) return;
    bool match = false, unicast = false;
    for (int i = 0; i < qd; i++) {
        if (!readName(buf, len, off, name) || off + 4 > len) return;
        u16 cls = rd16(buf + off + 2);
        off += 4;
        std::string l = toLower(name);
        if (l == m_svcLower || l == m_instLower || l == m_hostLower || l == "_services._dns-sd._udp.local") {
            match = true;
            if (cls & 0x8000) unicast = true;
        }
    }
    if (!match) return;

    u8 out[1024];
    size_t outLen = buildResponse(out, sizeof(out), 1);
    if (!outLen) return;
    u16 srcPort = ntohs(from.sin_port);
    if (srcPort != kMdnsPort) {
        // "Legacy"-Anfrage: Antwort direkt an den Absender, mit dessen ID
        out[0] = buf[0];
        out[1] = buf[1];
        sendTo(out, outLen, from.sin_addr.s_addr, srcPort);
    } else if (unicast) {
        sendTo(out, outLen, from.sin_addr.s_addr, kMdnsPort);
    } else {
        u64 now = nowMs();
        if (now - m_lastResponse < 500) return;  // nicht fluten
        m_lastResponse = now;
        sendTo(out, outLen, kMdnsGroup, kMdnsPort);
    }
}
