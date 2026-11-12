// Coordinates -- saved spots, return spots and notes, as plugins\mt_plugin_Coordinates.dll.
//
// The user, 2026-11-11: "store coordinates on a keybind, we will need that soon" (ports, launch points, delivery
// spots). 2026-11-12: "a named return spot", and notes from chat, "#note, saved but never sent".
//   Ctrl+F<Hotkey>   (F11) saves a spot named spot<N>
//   #save <name>     saves a spot with that name                                    (typed in chat)
//   #return [name]   puts you, or the vehicle you drive, back on it; no name = the last spot saved
//   #note <text>     saves the text with where you are
// Any chat line starting with '#' is handled here and never reaches the server. Everything goes to
// Binaries\Win64\plugins\coordinates.csv:
//   time, x, y, z (cm, the game's world), yaw (degrees), what you are in (vehicle or character class), name, note
// One beep high = saved, two high = returned, low = failed (no world, no such spot). Written whether or not logging
// is on: the file is the point.
//
// Settings: plugins\coordinates.ini, [General] Disabled; [Coordinates] Hotkey (11 = Ctrl+F11).

#include <cstdio>
#include <string>
#include <vector>
#include "../include/mt_plugin.hpp"

using namespace mt;

static const wchar_t* INI = L"coordinates.ini";
struct Vec { double x, y, z; };
static uintptr_t cPC, fPawn, gameplayCDO, fLoc, fRot, fTeleport, fRoot, fSetVel, cPrim;
static int pcIdx = -1;
static uintptr_t pcObj;
static volatile LONG pending;

static void beep(int hz, int n) {
    struct B { int hz, n; };
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID p) -> DWORD { auto b = (B*)p; for (int i = 0; i < b->n; i++) { Beep(b->hz, 120); Sleep(60); } delete b; return 0; }, new B{hz, n}, 0, nullptr));
}

static std::wstring csvPath() { return gameDir() + L"plugins\\coordinates.csv"; }

// one CSV line; the header goes in when the file is new
static bool append(const char* line) {
    std::wstring path = csvPath();
    bool fresh = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
    FILE* f = _wfopen(path.c_str(), L"a");
    if (!f) return false;
    if (fresh) fputs("time,x,y,z,yaw,in,name,note\n", f);
    fputs(line, f);
    fclose(f);
    return true;
}

static std::vector<std::string> lines() {
    std::vector<std::string> out;
    FILE* f = _wfopen(csvPath().c_str(), L"r");
    if (!f) return out;
    char b[1024];
    while (fgets(b, sizeof b, f)) out.push_back(b);
    fclose(f);
    return out;
}

static std::vector<std::string> cols(const std::string& l) {
    std::vector<std::string> c(1);
    for (char ch : l)
        if (ch == ',') c.emplace_back();
        else if (ch != '\n' && ch != '\r') c.back() += ch;
    return c;
}

// commas and line breaks would break the CSV
static std::string clean(std::string t) {
    for (char& c : t)
        if (c == ',') c = ';';
        else if (c == '\n' || c == '\r') c = ' ';
    return t;
}

// the pawn you control: the vehicle while driving, else the character. Game thread.
static uintptr_t pawn() {
    if (!pcObj || api->object(pcIdx) != pcObj) {  // found by a walk over every object, kept while it lives
        pcObj = 0;
        for (int i = 0, n = api->num_objects(); i < n; i++) {
            uintptr_t o = api->object(i);
            if (o && !isCDO(o) && api->is_a(classOf(o), cPC)) { pcObj = o; pcIdx = i; break; }
        }
    }
    if (!pcObj) return 0;
    struct { uintptr_t ctx; int i; uintptr_t ret; } p = {pcObj, 0, 0};
    api->process_event(gameplayCDO, fPawn, &p);
    return p.ret;
}

static int spotCount() {
    int n = 0;
    for (auto& l : lines()) { auto c = cols(l); if (c.size() > 6 && !c[6].empty() && c[6] != "name") n++; }
    return n;
}

static void save(std::string name, const std::string& note) {
    uintptr_t pw = pawn();
    if (!pw) { MT_LOG("no player pawn: not in a world"); beep(300, 2); return; }
    if (name.empty() && note.empty()) name = "spot" + std::to_string(spotCount() + 1);
    Vec at{}, rot{};
    api->process_event(pw, fLoc, &at);
    api->process_event(pw, fRot, &rot);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1024];
    snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d,%.0f,%.0f,%.0f,%.0f,%s,%s,%s\n", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond, at.x, at.y, at.z, rot.y, objName(classOf(pw)).c_str(), clean(name).c_str(), clean(note).c_str());
    bool ok = append(line);
    MT_LOG("%s %s", ok ? "saved" : "FAILED to write", line);
    beep(ok ? 1300 : 300, ok ? 1 : 3);
}

// the last line saved under that name (any named spot when name is empty; notes are not spots)
static bool findSpot(const std::string& name, Vec& at, double& yaw) {
    auto ls = lines();
    for (auto it = ls.rbegin(); it != ls.rend(); ++it) {
        auto c = cols(*it);
        if (c.size() < 7 || c[6].empty() || c[6] == "name" || (!name.empty() && _stricmp(c[6].c_str(), name.c_str()))) continue;
        at = {atof(c[1].c_str()), atof(c[2].c_str()), atof(c[3].c_str())};
        yaw = atof(c[4].c_str());
        return true;
    }
    return false;
}

static void goBack(const std::string& name) {
    uintptr_t pw = pawn();
    Vec at;
    double yaw;
    if (!pw) { MT_LOG("return: not in a world"); beep(300, 2); return; }
    if (!fTeleport) { MT_LOG("return: K2_TeleportTo not found"); beep(300, 3); return; }
    if (!findSpot(name, at, yaw)) { MT_LOG("return: no spot named '%s'", name.c_str()); beep(300, 3); return; }
    at.z += 50;  // a little above, so a vehicle does not land in the ground
    unsigned char p[0x40] = {0};
    Vec rot{0, yaw, 0};
    memcpy(p, &at, 24);
    memcpy(p + 24, &rot, 24);
    api->process_event(pw, fTeleport, p);
    uintptr_t root = 0;  // a vehicle keeps its speed through a teleport: stop it
    if (fRoot) api->process_event(pw, fRoot, &root);
    if (root && fSetVel && cPrim && api->is_a(classOf(root), cPrim)) { unsigned char q[0x30] = {0}; api->process_event(root, fSetVel, q); }
    MT_LOG("returned to '%s' at %.0f %.0f %.0f", name.empty() ? "(last)" : name.c_str(), at.x, at.y, at.z - 50);
    beep(1700, 2);
}

static void capture(void*) {
    InterlockedExchange(&pending, 0);
    save("", "");
}

// "#cmd rest" -> cmd (lowercased), rest (trailing spaces trimmed)
static void split(const std::string& line, std::string& cmd, std::string& rest) {
    size_t sp = line.find(' ');
    cmd = line.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
    for (char& c : cmd) c = (char)tolower((unsigned char)c);
    rest = sp == std::string::npos ? "" : line.substr(sp + 1);
    while (!rest.empty() && rest.back() == ' ') rest.pop_back();
}

// ServerSendChat(<FString message>, ...): read it, and keep any '#' line off the server. Game thread.
static int msgOff = -2;
static int onChat(uintptr_t, uintptr_t func, void* parms, void*) {
    if (msgOff == -2) {
        msgOff = -1;
        for (const char* n : {"Message", "Msg", "Text", "ChatMessage", "InMessage", "Str"})
            if ((msgOff = api->field_offset(func, n)) >= 0) { MT_LOG("chat message parameter: %s at %d", n, msgOff); break; }
        if (msgOff < 0) MT_LOG("chat: message parameter not found, chat commands off");
    }
    if (msgOff < 0) return 0;
    struct FStr { wchar_t* d; int n, m; };
    FStr str = *(FStr*)((char*)parms + msgOff);
    if (!str.d || str.n < 2 || str.n > 1000 || str.d[0] != L'#') return 0;
    char u[1024];
    int k = WideCharToMultiByte(CP_UTF8, 0, str.d, str.n - 1, u, sizeof u - 1, nullptr, nullptr);
    u[k > 0 ? k : 0] = 0;
    std::string cmd, rest;
    split(u, cmd, rest);
    if (cmd == "note") save("", rest.empty() ? "(empty note)" : rest);
    else if (cmd == "save") save(rest, "");
    else if (cmd == "return" || cmd == "back") goBack(rest);
    else { MT_LOG("chat: unknown command #%s (note, save, return)", cmd.c_str()); beep(300, 2); }
    return 1;  // never sent
}

static DWORD WINAPI watchChat(LPVOID) {
    // the chat RPC exists only once the game has loaded its player classes: retry until it registers
    static const char* const names[] = {"ServerSendChat"};
    while (!api->watch_functions(names, 1, onChat, nullptr)) Sleep(2000);
    MT_LOG("chat commands on: #note <text>, #save <name>, #return [name]");
    return 0;
}

static DWORD WINAPI run(LPVOID) {
    if (!api->wait_engine(INFINITE)) return 0;
    cPC = api->find_object("Class", "PlayerController", "/Script/Engine");
    fPawn = api->find_object("Function", "GetPlayerPawn", "GameplayStatics");
    gameplayCDO = api->find_object("GameplayStatics", "Default__GameplayStatics", "/Script/Engine");
    fLoc = api->find_object("Function", "K2_GetActorLocation", "Actor");
    fRot = api->find_object("Function", "K2_GetActorRotation", "Actor");
    if (!cPC || !fPawn || !gameplayCDO || !fLoc || !fRot) { MT_LOG("FAILED: functions not found"); return 0; }
    fTeleport = api->find_object("Function", "K2_TeleportTo", "Actor");
    fRoot = api->find_object("Function", "K2_GetRootComponent", "Actor");
    fSetVel = api->find_object("Function", "SetAllPhysicsLinearVelocity", "PrimitiveComponent");
    cPrim = api->find_object("Class", "PrimitiveComponent", "/Script/Engine");
    CloseHandle(CreateThread(nullptr, 0, watchChat, nullptr, 0, nullptr));
    int key = iniInt(INI, L"Coordinates", L"Hotkey", 11);
    if (key < 1 || key > 12) key = 11;
    MT_LOG("Coordinates ready: Ctrl+F%d saves a spot to plugins\\coordinates.csv", key);
    bool down = false;
    for (;;) {
        bool k = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_F1 + key - 1) & 0x8000);
        if (k && !down && !pending) {
            InterlockedExchange(&pending, 1);
            if (api->game_thread_ready()) api->run_on_game_thread(capture, nullptr);
            else InterlockedExchange(&pending, 0);
        }
        down = k;
        Sleep(16);
    }
}

static void demo() {  // the CSV and command shapes, without a game
    char line[128];
    snprintf(line, sizeof line, "%.0f,%.0f", -36727.4, 1404486.6);
    if (std::string(line) != "-36727,1404487") MT_LOG("SELF-CHECK FAILED: number format");
    std::string c, r;
    split("#Save Dock 2 ", c, r);
    if (c != "save" || r != "Dock 2") MT_LOG("SELF-CHECK FAILED: command split");
    auto cs = cols("t,1,2,3,90,Car,dock,a note\n");
    if (cs.size() != 8 || cs[6] != "dock" || cs[7] != "a note" || clean("a,b\n") != "a;b ") MT_LOG("SELF-CHECK FAILED: csv");
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    if (iniDisabled(INI)) return 0;
    demo();
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
