#define TESLA_INIT_IMPL
#include <tesla.hpp>

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

#include "airplay_client.h"

using tsl::Color;
using tsl::gfx::Renderer;

static bool g_running = false;
static bool g_haveStatus = false;
static AirplayStatus g_status;

namespace colors {
constexpr Color Accent = {0x0, 0xF, 0xD, 0xF};     // Tesla-Tuerkis
constexpr Color Text = {0xF, 0xF, 0xF, 0xF};
constexpr Color Dim = {0xA, 0xA, 0xA, 0xF};
constexpr Color Faint = {0x6, 0x6, 0x6, 0xF};
constexpr Color Track = {0x3, 0x3, 0x3, 0xF};
constexpr Color Button = {0x2, 0x2, 0x2, 0xF};
constexpr Color Paused = {0xF, 0xB, 0x3, 0xF};
constexpr Color Black = {0x0, 0x0, 0x0, 0xF};
}  // namespace colors

static std::string formatTime(u32 ms) {
    char buf[16];
    u32 s = ms / 1000;
    snprintf(buf, sizeof(buf), "%u:%02u", s / 60, s % 60);
    return buf;
}

static u32 textWidth(Renderer* r, const std::string& s, float size) {
    return r->drawString(s.c_str(), false, 0, 0, size, tsl::style::color::ColorTransparent).first;
}

// Kuerzt Text mit "..." auf maxWidth (Ergebnis wird zwischengespeichert, Messen kostet Zeit).
static const std::string& fitText(Renderer* r, const std::string& s, float size, u32 maxWidth) {
    static std::map<std::string, std::string> cache;
    std::string key = s + '\x1f' + std::to_string((int)size) + '\x1f' + std::to_string(maxWidth);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 64) cache.clear();

    std::string out = s;
    if (textWidth(r, out, size) > maxWidth) {
        while (!out.empty()) {
            // ein ganzes UTF-8-Zeichen entfernen
            size_t cut = out.size() - 1;
            while (cut > 0 && ((u8)out[cut] & 0xC0) == 0x80) cut--;
            out.erase(cut);
            if (textWidth(r, out + "...", size) <= maxWidth) break;
        }
        out += "...";
    }
    return cache.emplace(key, out).first->second;
}

// ---------------------------------------------------------------- einfache Symbole aus Rechtecken

static void drawTriangleRight(Renderer* r, s32 x, s32 cy, s32 h, Color c) {
    // gleichseitiges Dreieck mit Spitze nach rechts, Hoehe 2h
    for (s32 dy = -h; dy <= h; dy++) {
        s32 w = (s32)((h - (dy < 0 ? -dy : dy)) * 1.7f);
        if (w > 0) r->drawRect(x, cy + dy, w, 1, c);
    }
}

static void drawTriangleLeft(Renderer* r, s32 xRight, s32 cy, s32 h, Color c) {
    for (s32 dy = -h; dy <= h; dy++) {
        s32 w = (s32)((h - (dy < 0 ? -dy : dy)) * 1.7f);
        if (w > 0) r->drawRect(xRight - w, cy + dy, w, 1, c);
    }
}

enum class Icon { Prev, PlayPause, Next, Minus, Plus };

static void drawIcon(Renderer* r, Icon icon, s32 cx, s32 cy, s32 s, bool playing, Color c) {
    switch (icon) {
        case Icon::PlayPause:
            if (playing) {
                r->drawRect(cx - s * 6 / 10, cy - s, s / 2, s * 2, c);
                r->drawRect(cx + s / 10, cy - s, s / 2, s * 2, c);
            } else {
                drawTriangleRight(r, cx - s * 6 / 10, cy, s, c);
            }
            break;
        case Icon::Next:
            drawTriangleRight(r, cx - s, cy, s * 3 / 4, c);
            drawTriangleRight(r, cx - s / 4, cy, s * 3 / 4, c);
            r->drawRect(cx + s * 9 / 10, cy - s * 3 / 4, 3, s * 3 / 2, c);
            break;
        case Icon::Prev:
            drawTriangleLeft(r, cx + s, cy, s * 3 / 4, c);
            drawTriangleLeft(r, cx + s / 4, cy, s * 3 / 4, c);
            r->drawRect(cx - s * 9 / 10 - 3, cy - s * 3 / 4, 3, s * 3 / 2, c);
            break;
        case Icon::Minus:
            r->drawRect(cx - s * 3 / 4, cy - 2, s * 3 / 2, 4, c);
            break;
        case Icon::Plus:
            r->drawRect(cx - s * 3 / 4, cy - 2, s * 3 / 2, 4, c);
            r->drawRect(cx - 2, cy - s * 3 / 4, 4, s * 3 / 2, c);
            break;
    }
}

// ---------------------------------------------------------------- Knopfreihe (links/rechts waehlen, A druecken)

class ButtonRow : public tsl::elm::Element {
public:
    struct Button {
        Icon icon;
        bool big;
        std::function<void()> action;
    };

    ButtonRow(std::vector<Button> buttons, size_t selected, u16 height, const char* left = nullptr, const char* right = nullptr)
        : m_buttons(std::move(buttons)), m_sel(selected), m_height(height), m_left(left), m_right(right) {}

    void draw(Renderer* r) override {
        bool enabled = remoteEnabled();
        for (size_t i = 0; i < m_buttons.size(); i++) {
            s32 cx = centerX(i), cy = this->getY() + m_height / 2;
            s32 rad = radius(i);
            bool active = this->m_focused && i == m_sel;
            Color fill = m_buttons[i].big ? colors::Text : colors::Button;
            if (active) fill = colors::Accent;
            if (!enabled) fill = colors::Track;
            Color fg = (m_buttons[i].big || active) && enabled ? colors::Black : (enabled ? colors::Text : colors::Faint);
            r->drawCircle(cx, cy, rad, true, a(fill));
            bool playing = g_status.state == AirplayState_Playing;
            drawIcon(r, m_buttons[i].icon, cx, cy, m_buttons[i].big ? 14 : 10, playing, a(fg));
        }
        if (m_left) r->drawString(m_left, false, this->getX() + 10, this->getY() + m_height / 2 + 7, 18, a(colors::Dim));
        if (m_right) {
            u32 w = textWidth(r, m_right, 18);
            r->drawString(m_right, false, this->getX() + this->getWidth() - 10 - (s32)w, this->getY() + m_height / 2 + 7, 18, a(colors::Dim));
        }
    }

    void drawHighlight(Renderer* r) override {
        // Ring um den gewaehlten Knopf statt Rahmen um die ganze Zeile
        s32 cx = centerX(m_sel), cy = this->getY() + m_height / 2, rad = radius(m_sel);
        for (int t = 2; t <= 4; t++) r->drawCircle(cx, cy, rad + t, false, a(colors::Accent));
    }

    void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override {
        this->setBoundaries(this->getX(), this->getY(), this->getWidth(), m_height);
    }

    tsl::elm::Element* requestFocus(tsl::elm::Element* oldFocus, tsl::FocusDirection direction) override { return this; }

    bool onClick(u64 keys) override {
        if (keys & HidNpadButton_AnyLeft) {
            if (m_sel > 0) m_sel--;
            return true;
        }
        if (keys & HidNpadButton_AnyRight) {
            if (m_sel + 1 < m_buttons.size()) m_sel++;
            return true;
        }
        if (keys & HidNpadButton_A) {
            if (remoteEnabled() && m_buttons[m_sel].action) m_buttons[m_sel].action();
            return true;
        }
        return false;
    }

    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
        if (event != tsl::elm::TouchEvent::Release) return false;
        for (size_t i = 0; i < m_buttons.size(); i++) {
            s32 dx = currX - centerX(i), dy = currY - (this->getY() + m_height / 2), rad = radius(i) + 8;
            if (dx * dx + dy * dy <= rad * rad) {
                m_sel = i;
                if (remoteEnabled() && m_buttons[i].action) m_buttons[i].action();
                return true;
            }
        }
        return false;
    }

private:
    static bool remoteEnabled() { return g_status.flags & AIRPLAY_FLAG_REMOTE_OK; }

    s32 radius(size_t i) { return m_buttons[i].big ? 32 : 24; }

    s32 centerX(size_t i) {
        // Knoepfe mittig verteilen, Abstand 90 px
        s32 n = (s32)m_buttons.size();
        s32 mid = this->getX() + this->getWidth() / 2;
        return mid + ((s32)i - (n - 1) / 2) * 90 - ((n % 2) == 0 ? 45 : 0);
    }

    std::vector<Button> m_buttons;
    size_t m_sel;
    u16 m_height;
    const char* m_left;
    const char* m_right;
};

// ---------------------------------------------------------------- Kopfbereich

static void drawHeader(Renderer* r, s32 x, s32 y, s32 w, s32 h) {
    const AirplayStatus& s = g_status;
    const s32 left = 24, width = w - 48;

    r->drawString("AirPlay", false, left, 52, 30, r->a(colors::Text));

    // Status-Pille rechts oben
    const char* label = "Bereit";
    Color dot = colors::Faint;
    if (s.state == AirplayState_Playing) { label = "Spielt"; dot = colors::Accent; }
    else if (s.state == AirplayState_Paused) { label = "Pausiert"; dot = colors::Paused; }
    else if (s.state == AirplayState_Connected) { label = "Verbunden"; dot = colors::Accent; }
    u32 lw = textWidth(r, label, 16);
    s32 lx = left + width - (s32)lw;
    r->drawString(label, false, lx, 46, 16, r->a(colors::Dim));
    r->drawCircle(lx - 12, 40, 5, true, r->a(dot));

    char info[96];
    snprintf(info, sizeof(info), "%s  ·  %s", s.device_name[0] ? s.device_name : "Switch", s.switch_ip[0] ? s.switch_ip : "kein WLAN");
    r->drawString(info, false, left, 78, 15, r->a(colors::Faint));

    if (s.state == AirplayState_Idle) {
        r->drawString("Nichts verbunden", false, left, 140, 24, r->a(colors::Text));
        r->drawString("iPhone: Kontrollzentrum > AirPlay", false, left, 172, 17, r->a(colors::Dim));
        std::string pick = std::string("> ") + (s.device_name[0] ? s.device_name : "Switch") + " wählen";
        r->drawString(pick.c_str(), false, left, 196, 17, r->a(colors::Dim));
        r->drawString("Apple Music, Podcasts & alle anderen Apps", false, left, 228, 15, r->a(colors::Faint));
        return;
    }

    const std::string title = s.title[0] ? s.title : "Unbekannter Titel";
    r->drawString(fitText(r, title, 26, width).c_str(), false, left, 132, 26, r->a(colors::Text));
    if (s.artist[0]) r->drawString(fitText(r, s.artist, 19, width).c_str(), false, left, 162, 19, r->a(colors::Accent));
    if (s.album[0]) r->drawString(fitText(r, s.album, 16, width).c_str(), false, left, 186, 16, r->a(colors::Dim));

    // Fortschritt
    const s32 barY = 208;
    r->drawRect(left, barY, width, 4, r->a(colors::Track));
    if (s.flags & AIRPLAY_FLAG_HAS_PROGRESS && s.duration_ms) {
        s32 filled = (s32)((u64)width * s.position_ms / s.duration_ms);
        r->drawRect(left, barY, filled, 4, r->a(colors::Accent));
        r->drawCircle(left + filled, barY + 2, 6, true, r->a(colors::Text));
        std::string pos = formatTime(s.position_ms), dur = "-" + formatTime(s.duration_ms - s.position_ms);
        r->drawString(pos.c_str(), false, left, barY + 26, 14, r->a(colors::Faint));
        u32 dw = textWidth(r, dur, 14);
        r->drawString(dur.c_str(), false, left + width - (s32)dw, barY + 26, 14, r->a(colors::Faint));
    }
    if (!(s.flags & AIRPLAY_FLAG_REMOTE_OK)) {
        const char* hint = "Fernbedienung wird verbunden...";
        u32 hw = textWidth(r, hint, 14);
        r->drawString(hint, false, left + (width - (s32)hw) / 2, barY + 26, 14, r->a(colors::Faint));
    }
}

// ---------------------------------------------------------------- Diagnose, falls das Sysmodule fehlt

static std::string g_diagProcess, g_diagMemory, g_diagLaunch;

static void updateDiagnosis() {
    u64 pid = 0;
    if (R_SUCCEEDED(pmdmntInitialize())) {
        Result rc = pmdmntGetProcessId(&pid, AIRPLAY_TITLE_ID);
        pmdmntExit();
        g_diagProcess = R_SUCCEEDED(rc) && pid ? "Prozess läuft (PID " + std::to_string(pid) + "), Dienst fehlt"
                                               : "Prozess ist nicht gestartet";
    }
    u64 total = 0, used = 0;
    svcGetSystemInfo(&total, SystemInfoType_TotalPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_System);
    svcGetSystemInfo(&used, SystemInfoType_UsedPhysicalMemorySize, INVALID_HANDLE, PhysicalMemorySystemInfo_System);
    char buf[96];
    snprintf(buf, sizeof(buf), "System-Speicher frei: %.2f MB von %.1f MB", (total - used) / 1048576.0, total / 1048576.0);
    g_diagMemory = buf;
}

static void tryLaunch() {
    char buf[96];
    if (R_FAILED(pmshellInitialize())) {
        g_diagLaunch = "pm:shell nicht verfügbar";
        return;
    }
    NcmProgramLocation loc = {};
    loc.program_id = AIRPLAY_TITLE_ID;
    loc.storageID = NcmStorageId_None;
    u64 pid = 0;
    Result rc = pmshellLaunchProgram(0, &loc, &pid);
    pmshellExit();
    if (R_SUCCEEDED(rc)) snprintf(buf, sizeof(buf), "Gestartet - Overlay neu öffnen");
    else snprintf(buf, sizeof(buf), "Start fehlgeschlagen: 0x%X (%04u-%04u)", rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
    g_diagLaunch = buf;
    updateDiagnosis();
}

// ---------------------------------------------------------------- Oberflaeche

class GuiMain : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        if (!g_running) return createDiagnosisUI();

        auto frame = new tsl::elm::HeaderOverlayFrame(250);
        frame->setHeader(new tsl::elm::CustomDrawer(drawHeader));

        auto list = new tsl::elm::List();
        m_transport = new ButtonRow(
                          {
                              {Icon::Prev, false, [] { airplayRemote(AirplayRemote_Prev); }},
                              {Icon::PlayPause, true, [] { airplayRemote(AirplayRemote_PlayPause); }},
                              {Icon::Next, false, [] { airplayRemote(AirplayRemote_Next); }},
                          },
                          1, 96);
        list->addItem(m_transport, 96);

        list->addItem(new tsl::elm::CategoryHeader("Lautstärke Switch"));
        m_volume = new tsl::elm::TrackBar("");
        m_volume->setProgress((u8)(g_status.local_volume * 100.0f + 0.5f));
        m_volume->setValueChangedListener([](u8 v) { airplaySetLocalVolume(v / 100.0f); });
        list->addItem(m_volume);

        list->addItem(new tsl::elm::CategoryHeader("Lautstärke iPhone"));
        m_senderVolume = new tsl::elm::TrackBar("");
        m_senderVolume->setProgress(dbToPercent(g_status.sender_volume_db));
        m_senderVolume->setValueChangedListener([this](u8 v) {
            airplaySetSenderVolume(percentToDb(v));
            m_senderTouchedAt = m_frames;
        });
        list->addItem(m_senderVolume);

        frame->setContent(list);
        return frame;
    }

    void update() override {
        if (!g_running) return;
        // libtesla fragt den Startfokus ab, bevor die Liste befuellt ist: hier nachholen
        if (m_frames > 1 && m_transport && this->getFocusedElement() == nullptr)
            this->requestFocus(m_transport, tsl::FocusDirection::None, false);
        if (m_frames++ % 15 != 0) return;  // ~4x pro Sekunde
        AirplayStatus s;
        if (R_FAILED(airplayGetStatus(&s))) return;
        g_status = s;
        g_haveStatus = true;
        // Aenderungen vom iPhone uebernehmen, aber nicht waehrend hier gerade geregelt wird
        if (m_senderVolume && m_frames - m_senderTouchedAt > 120) m_senderVolume->setProgress(dbToPercent(s.sender_volume_db));
        if (!m_volumeSynced && m_volume) {
            m_volume->setProgress((u8)(s.local_volume * 100.0f + 0.5f));
            m_volumeSynced = true;
        }
    }

private:
    tsl::elm::Element* createDiagnosisUI() {
        auto frame = new tsl::elm::OverlayFrame("AirPlay", "sys-airplay");
        auto list = new tsl::elm::List();
        list->addItem(new tsl::elm::CustomDrawer([](Renderer* r, s32 x, s32 y, s32 w, s32 h) {
            r->drawString("sys-airplay läuft nicht.", false, x + 20, y + 40, 22, r->a(colors::Text));
            r->drawString(g_diagProcess.c_str(), false, x + 20, y + 76, 17, r->a(colors::Dim));
            r->drawString(g_diagMemory.c_str(), false, x + 20, y + 100, 17, r->a(colors::Dim));
            r->drawString(g_diagLaunch.c_str(), false, x + 20, y + 124, 17, r->a(colors::Accent));
        }), 150);
        auto start = new tsl::elm::ListItem("Jetzt starten");
        start->setClickListener([](u64 keys) {
            if (!(keys & HidNpadButton_A)) return false;
            // libtesla haelt ausserhalb von initServices keine sm-Sitzung offen
            tsl::hlp::doWithSmSession([] { tryLaunch(); });
            return true;
        });
        list->addItem(start);
        frame->setContent(list);
        return frame;
    }

    static u8 dbToPercent(float db) {
        if (db <= -30.0f) return 0;
        if (db >= 0.0f) return 100;
        return (u8)((db + 30.0f) * 100.0f / 30.0f + 0.5f);
    }
    static float percentToDb(u8 p) { return p == 0 ? -144.0f : -30.0f + 30.0f * p / 100.0f; }

    u32 m_frames = 0;
    u32 m_senderTouchedAt = 0;
    bool m_volumeSynced = false;
    ButtonRow* m_transport = nullptr;
    tsl::elm::TrackBar* m_volume = nullptr;
    tsl::elm::TrackBar* m_senderVolume = nullptr;
};

class AirplayOverlay : public tsl::Overlay {
public:
    void initServices() override {
        g_running = airplayIsRunning() && R_SUCCEEDED(airplayInit());
        if (g_running && R_SUCCEEDED(airplayGetStatus(&g_status))) g_haveStatus = true;
        if (!g_running) updateDiagnosis();
    }
    void exitServices() override { airplayExit(); }

    std::unique_ptr<tsl::Gui> loadInitialGui() override { return initially<GuiMain>(); }
};

int main(int argc, char** argv) {
    return tsl::loop<AirplayOverlay>(argc, argv);
}
