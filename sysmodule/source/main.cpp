// sys-airplay: macht die Switch zum AirPlay-Lautsprecher (laeuft im Hintergrund).
#include <switch.h>

#include <arpa/inet.h>
#include <cstdio>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio.hpp"
#include "crypto.hpp"
#include "dacp.hpp"
#include "ipc.hpp"
#include "mdns.hpp"
#include "player.hpp"
#include "rtsp.hpp"
#include "state.hpp"
#include "util.hpp"

extern "C" {

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

#define INNER_HEAP_SIZE 0x100000
size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char nx_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = nx_inner_heap;
    fake_heap_end = nx_inner_heap + nx_inner_heap_size;
}

static void initOrAbort(Result rc) {
    if (R_FAILED(rc)) diagAbortWithResult(rc);
}

void __appInit(void) {
    initOrAbort(smInitialize());

    if (R_SUCCEEDED(setsysInitialize())) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    initOrAbort(fsInitialize());
    fsdevMountSdmc();

    if (R_FAILED(nifmInitialize(NifmServiceType_User))) initOrAbort(nifmInitialize(NifmServiceType_System));

    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    cfg.tcp_tx_buf_size = 0x1000;
    cfg.tcp_rx_buf_size = 0x1000;
    cfg.tcp_tx_buf_max_size = 0;
    cfg.tcp_rx_buf_max_size = 0;
    cfg.udp_tx_buf_size = 0x2400;
    cfg.udp_rx_buf_size = 0xA500;
    cfg.sb_efficiency = 4;
    cfg.num_bsd_sessions = 3;
    initOrAbort(socketInitialize(&cfg));
}

void __appExit(void) {
    socketExit();
    nifmExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

}  // extern "C"

static std::string hexMac() {
    char buf[13];
    const u8* m = config().mac;
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return buf;
}

static std::vector<std::string> txtRecords() {
    return {"txtvers=1", "ch=2",       "cn=0,1",     "et=0,1", "ek=1",       "md=0,2",          "sm=false",
            "sv=false",  "da=true",    "sr=44100",   "ss=16",  "pw=false",   "tp=UDP",          "vn=3",
            "vs=130.14", "am=Switch1", "sf=0x4"};
}

static int openListen(u16& portOut) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    for (u16 port = 7000; port < 7020; port++) {  // nicht 5000: dort laeuft oft ftpd
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (bind(fd, (sockaddr*)&a, sizeof(a)) == 0 && listen(fd, 2) == 0) {
            portOut = port;
            return fd;
        }
    }
    close(fd);
    return -1;
}

static u32 currentIp() {
    u32 ip = 0;
    if (R_FAILED(nifmGetCurrentIpAddress(&ip))) return 0;
    return ip;
}

int main(int argc, char** argv) {
    configLoad();
    logInit(config().log);
    logMsg("sys-airplay startet, Name \"%s\"", config().name.c_str());

    rsaInit();
    if (!audio::init()) logMsg("Audio-Initialisierung fehlgeschlagen");
    ipc::start();

    Mdns mdns;
    int listenFd = -1;
    u16 rtspPort = 0;
    std::unique_ptr<RtspConnection> conn;
    u32 ip = 0;
    u64 lastNetCheck = 0;
    bool rebuild = false;

    const std::string instance = hexMac() + "@" + config().name;
    const std::string host = "Switch-AirPlay-" + hexMac().substr(8);

    while (true) {
        u64 now = nowMs();

        // Netzwerk beobachten (WLAN-Wechsel, Standby, Flugmodus)
        if (now - lastNetCheck >= 2000) {
            lastNetCheck = now;
            u32 newIp = currentIp();
            if (newIp != ip || rebuild || mdns.failed() || (newIp && listenFd < 0)) {
                rebuild = false;
                logMsg("Netzwerk: %s", newIp ? ipToString(newIp).c_str() : "keine Verbindung");
                conn.reset();
                mdns.close();
                if (listenFd >= 0) close(listenFd);
                listenFd = -1;
                ip = newIp;
                state::setSwitchIp(ip);
                if (ip) {
                    listenFd = openListen(rtspPort);
                    if (listenFd >= 0 && mdns.open(ip)) {
                        mdns.setService(instance, host, rtspPort, txtRecords());
                        logMsg("AirPlay-Empfaenger \"%s\" auf Port %u", config().name.c_str(), rtspPort);
                    }
                }
            }
        }

        pollfd fds[3];
        int n = 0, iMdns = -1, iListen = -1, iConn = -1;
        if (mdns.fd() >= 0) { iMdns = n; fds[n++] = {mdns.fd(), POLLIN, 0}; }
        if (listenFd >= 0) { iListen = n; fds[n++] = {listenFd, POLLIN, 0}; }
        if (conn) { iConn = n; fds[n++] = {conn->fd(), POLLIN, 0}; }

        if (n == 0) {
            svcSleepThread(200'000'000ULL);
            continue;
        }
        int r = poll(fds, n, 100);
        if (r > 0) {
            if (iMdns >= 0 && (fds[iMdns].revents & POLLIN)) mdns.onReadable();

            if (iConn >= 0 && (fds[iConn].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (!conn->onReadable()) {
                    conn.reset();
                    mdns.stopDacp();
                }
            }

            if (iListen >= 0 && (fds[iListen].revents & POLLIN)) {
                sockaddr_in from = {};
                socklen_t fl = sizeof(from);
                int cfd = accept(listenFd, (sockaddr*)&from, &fl);
                if (cfd >= 0) {
                    // Neuer Sender uebernimmt (wie bei einem echten AirPlay-Lautsprecher)
                    conn.reset();
                    mdns.stopDacp();
                    sockaddr_in local = {};
                    socklen_t ll = sizeof(local);
                    getsockname(cfd, (sockaddr*)&local, &ll);
                    u32 localIp = local.sin_addr.s_addr ? local.sin_addr.s_addr : ip;
                    conn = std::make_unique<RtspConnection>(cfd, from.sin_addr.s_addr, localIp);
                } else {
                    // Socket kaputt (z.B. nach Standby): beim naechsten Netz-Check neu aufbauen
                    rebuild = true;
                    lastNetCheck = 0;
                }
            }
        }

        mdns.tick(nowMs());

        if (conn && !conn->dacpId().empty()) mdns.lookupDacp(conn->dacpId());
        state::setRemoteAvailable(conn && mdns.dacpPort() != 0);

        int cmd = state::takeRemote();
        if (cmd >= 0 && conn && mdns.dacpPort()) dacpSend(conn->clientIp(), mdns.dacpPort(), conn->activeRemote(), cmd);

        // iPhone-Lautstaerke vom Overlay-Regler, hoechstens ~6x pro Sekunde senden
        static u64 lastVolumeSent = 0;
        float wantedDb;
        if (nowMs() - lastVolumeSent >= 150 && state::takeSenderVolume(wantedDb)) {
            if (conn && mdns.dacpPort()) dacpSetVolume(conn->clientIp(), mdns.dacpPort(), conn->activeRemote(), wantedDb);
            lastVolumeSent = nowMs();
        }

        if (state::takeConfigDirty(2000)) configSave();
    }

    return 0;
}
