// DACP: Fernsteuerung des Senders (Play/Pause, Titel vor/zurueck, Lautstaerke).
#include "dacp.hpp"
#include "util.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airplay_ipc.h"

static const char* commandPath(int cmd) {
    switch (cmd) {
        case AirplayRemote_PlayPause: return "playpause";
        case AirplayRemote_Next: return "nextitem";
        case AirplayRemote_Prev: return "previtem";
        case AirplayRemote_VolUp: return "volumeup";
        case AirplayRemote_VolDown: return "volumedown";
        default: return nullptr;
    }
}

static bool dacpRequest(u32 ip, u16 port, const std::string& activeRemote, const char* path) {
    if (!port || activeRemote.empty()) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ip;
    a.sin_port = htons(port);
    int rc = connect(fd, (sockaddr*)&a, sizeof(a));
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return false;
    }
    pollfd p = {fd, POLLOUT, 0};
    if (rc != 0 && (poll(&p, 1, 1500) <= 0 || !(p.revents & POLLOUT))) {
        logMsg("DACP: Verbindung zu Port %u fehlgeschlagen", port);
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, flags);

    char req[256];
    int len = snprintf(req, sizeof(req),
                       "GET /ctrl-int/1/%s HTTP/1.1\r\nHost: %s:%u\r\nActive-Remote: %s\r\nConnection: close\r\n\r\n",
                       path, ipToString(ip).c_str(), port, activeRemote.c_str());
    bool ok = send(fd, req, len, 0) == len;
    if (ok) {
        // Antwort abwarten (204 No Content), aber nicht ewig
        char resp[256];
        pollfd q = {fd, POLLIN, 0};
        if (poll(&q, 1, 1500) > 0) {
            int n = recv(fd, resp, sizeof(resp) - 1, 0);
            if (n > 0) {
                resp[n] = 0;
                ok = strstr(resp, " 20") != nullptr;
            }
        }
    }
    close(fd);
    logMsg("DACP %s: %s", path, ok ? "ok" : "fehlgeschlagen");
    return ok;
}

bool dacpSend(u32 ip, u16 port, const std::string& activeRemote, int cmd) {
    const char* path = commandPath(cmd);
    return path && dacpRequest(ip, port, activeRemote, path);
}

bool dacpSetVolume(u32 ip, u16 port, const std::string& activeRemote, float db) {
    char path[64];
    snprintf(path, sizeof(path), "setproperty?dmcp.device-volume=%.6f", db);
    return dacpRequest(ip, port, activeRemote, path);
}
