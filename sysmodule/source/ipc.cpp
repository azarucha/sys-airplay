// IPC-Dienst "airplay", ueber den das Tesla-Overlay Status abfragt und Befehle schickt.
#include "ipc.hpp"
#include "audio.hpp"
#include "state.hpp"
#include "util.hpp"

#include <cstring>

#include "airplay_ipc.h"

namespace ipc {

static const int kMaxSessions = 4;
static Handle g_handles[kMaxSessions + 1];
static int g_count = 0;
static Thread g_thread;

static const Result kErrBadRequest = MAKERESULT(420, 1);
static const Result kErrUnknownCmd = MAKERESULT(420, 2);

static void reply(Handle h, Result rc, const void* data, size_t size) {
    void* base = armGetTls();
    HipcMetadata meta = {};
    meta.num_data_words = (u32)((16 + sizeof(CmifOutHeader) + size + 3) / 4);
    HipcRequest msg = hipcMakeRequest(base, meta);
    CmifOutHeader* out = (CmifOutHeader*)cmifGetAlignedDataStart(msg.data_words, base);
    out->magic = CMIF_OUT_HEADER_MAGIC;
    out->version = 0;
    out->result = rc;
    out->token = 0;
    if (size) memcpy(out + 1, data, size);
    s32 idx;
    svcReplyAndReceive(&idx, nullptr, 0, h, 0);  // liefert erwartungsgemaess "Timeout"
}

// false = Sitzung schliessen
static bool handle(Handle h) {
    void* base = armGetTls();
    HipcParsedRequest r = hipcParseRequest(base);
    if (r.meta.type == CmifCommandType_Close) return false;
    if (r.meta.type != CmifCommandType_Request && r.meta.type != CmifCommandType_RequestWithContext) {
        reply(h, kErrBadRequest, nullptr, 0);
        return true;
    }

    const CmifInHeader* in = (const CmifInHeader*)cmifGetAlignedDataStart(r.data.data_words, base);
    if (in->magic != CMIF_IN_HEADER_MAGIC) {
        reply(h, kErrBadRequest, nullptr, 0);
        return true;
    }
    const u8* args = (const u8*)(in + 1);

    switch (in->command_id) {
        case AirplayCmd_GetStatus: {
            if (r.meta.num_recv_buffers < 1 || hipcGetBufferSize(&r.data.recv_buffers[0]) < sizeof(AirplayStatus)) {
                reply(h, kErrBadRequest, nullptr, 0);
                break;
            }
            void* dst = hipcGetBufferAddress(&r.data.recv_buffers[0]);
            AirplayStatus st;
            state::fill(st);
            memcpy(dst, &st, sizeof(st));
            reply(h, 0, nullptr, 0);
            break;
        }
        case AirplayCmd_Remote: {
            u32 cmd;
            memcpy(&cmd, args, sizeof(cmd));
            state::requestRemote((int)cmd);
            reply(h, 0, nullptr, 0);
            break;
        }
        case AirplayCmd_SetLocalVolume: {
            float v;
            memcpy(&v, args, sizeof(v));
            audio::setLocalVolume(v);
            config().localVolume = audio::localVolume();
            state::markConfigDirty();
            reply(h, 0, nullptr, 0);
            break;
        }
        case AirplayCmd_SetSenderVolume: {
            float db;
            memcpy(&db, args, sizeof(db));
            state::requestSenderVolume(db);
            reply(h, 0, nullptr, 0);
            break;
        }
        case AirplayCmd_GetVersion: {
            u32 v = AIRPLAY_IPC_VERSION;
            reply(h, 0, &v, sizeof(v));
            break;
        }
        default:
            reply(h, kErrUnknownCmd, nullptr, 0);
            break;
    }
    return true;
}

static void closeSession(int idx) {
    svcCloseHandle(g_handles[idx]);
    g_handles[idx] = g_handles[g_count - 1];
    g_count--;
}

static void ipcThread(void*) {
    while (true) {
        s32 idx = -1;
        Result rc = svcReplyAndReceive(&idx, g_handles, g_count, INVALID_HANDLE, UINT64_MAX);
        if (R_FAILED(rc)) {
            // typischerweise 0xF601: das Overlay wurde geschlossen
            if (idx > 0 && idx < g_count) closeSession(idx);
            else svcSleepThread(10'000'000ULL);
            continue;
        }
        if (idx == 0) {
            Handle s;
            if (R_SUCCEEDED(svcAcceptSession(&s, g_handles[0]))) {
                if (g_count < kMaxSessions + 1) g_handles[g_count++] = s;
                else svcCloseHandle(s);
            }
            continue;
        }
        if (!handle(g_handles[idx])) closeSession(idx);
    }
}

bool start() {
    Handle port;
    Result rc = smRegisterService(&port, smEncodeName(AIRPLAY_SERVICE_NAME), false, kMaxSessions);
    if (R_FAILED(rc)) {
        logMsg("Dienst konnte nicht registriert werden: 0x%x", rc);
        return false;
    }
    g_handles[0] = port;
    g_count = 1;
    rc = threadCreate(&g_thread, ipcThread, nullptr, nullptr, 0x4000, 0x2C, -2);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
    if (R_FAILED(rc)) {
        logMsg("IPC-Thread konnte nicht starten: 0x%x", rc);
        return false;
    }
    return true;
}

}  // namespace ipc
