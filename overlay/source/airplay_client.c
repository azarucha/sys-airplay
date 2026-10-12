#include "airplay_client.h"

static Service g_srv;
static bool g_open = false;

bool airplayIsRunning(void) {
    // Laesst sich der Name registrieren, laeuft der Dienst nicht.
    Handle h;
    SmServiceName name = smEncodeName(AIRPLAY_SERVICE_NAME);
    if (R_FAILED(smRegisterService(&h, name, false, 1))) return true;
    svcCloseHandle(h);
    smUnregisterService(name);
    return false;
}

Result airplayInit(void) {
    if (g_open) return 0;
    Result rc = smGetService(&g_srv, AIRPLAY_SERVICE_NAME);
    if (R_SUCCEEDED(rc)) g_open = true;
    return rc;
}

void airplayExit(void) {
    if (g_open) serviceClose(&g_srv);
    g_open = false;
}

Result airplayGetStatus(AirplayStatus* out) {
    if (!g_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatch(&g_srv, AirplayCmd_GetStatus,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { out, sizeof(*out) } },
    );
}

Result airplayRemote(u32 cmd) {
    if (!g_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatchIn(&g_srv, AirplayCmd_Remote, cmd);
}

Result airplaySetLocalVolume(float v) {
    if (!g_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatchIn(&g_srv, AirplayCmd_SetLocalVolume, v);
}

Result airplaySetSenderVolume(float db) {
    if (!g_open) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatchIn(&g_srv, AirplayCmd_SetSenderVolume, db);
}
