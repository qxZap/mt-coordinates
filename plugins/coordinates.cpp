// Coordinates -- saved spots, return spots and notes, as plugins\mt_plugin_Coordinates.dll.
//
// The user, 2026-11-11: "store coordinates on a keybind, we will need that soon" (ports, launch points, delivery
// spots). 2026-11-12: "a named return spot", and notes from chat, "#note, saved but never sent".
//   Ctrl+F<Hotkey>   (F11) saves a spot named spot<N>; Ctrl+Shift / Ctrl+Alt + F<Hotkey>: go to the next / previous one
//   tp_x_y_z, #tp x y z  teleport there (world cm)
//   #save <name>     saves a spot with that name                                    (typed in chat)
//   #return [name]   puts you, or the vehicle you drive, back on it; no name = the last spot saved
//   #note <text>     saves the text with where you are
//   #seat            rewrites the seat files of the vehicle you sit in. They are also written by themselves: any seat
//                    occupied for 3 s gets plugins\seat_probe\<Vehicle>__<Seat>.txt once -- every seat of the vehicle and
//                    every bone of whoever sits there, in the vehicle's own cm ([Coordinates] AutoSeat=0 stops it)
// Any chat line starting with '#' is handled here and never reaches the server. Everything goes to
// Binaries\Win64\plugins\coordinates.csv:
//   time, x, y, z (cm, the game's world), yaw (degrees), what you are in (vehicle or character class), name, note
// No beeps (the user, 2026-11-17): the log says what happened. Written whether or not logging
// is on: the file is the point.
//
// Settings: plugins\coordinates.ini, [General] Disabled; [Coordinates] Hotkey (11 = Ctrl+F11).

#include <cstdio>
#include <string>
#include <vector>
#include <set>
#include <map>
#include "../include/mt_plugin.hpp"

using namespace mt;

static const wchar_t* INI = L"coordinates.ini";
struct Vec { double x, y, z; };
static uintptr_t cPC, fPawn, gameplayCDO, fLoc, fRot, fTeleport, fRoot, fSetVel, cPrim;
static int pcIdx = -1;
static uintptr_t pcObj;
static volatile LONG pending;

// No beeps (the user, 2026-11-17: "remove the beeps ... whenever you are adding in a new POI"): the log says what happened.

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
    if (!pw) { MT_LOG("no player pawn: not in a world"); return; }
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

static int fPawnOf = -1;  // Controller.Pawn
// Puts a pawn at `at` facing `yaw`, and stops it (a vehicle keeps its speed through a teleport).
static void teleport(uintptr_t pw, Vec at, double yaw) {
    unsigned char p[0x40] = {0};
    Vec rot{0, yaw, 0};
    memcpy(p, &at, 24);
    memcpy(p + 24, &rot, 24);
    api->process_event(pw, fTeleport, p);
    uintptr_t root = 0;
    if (fRoot) api->process_event(pw, fRoot, &root);
    if (root && fSetVel && cPrim && api->is_a(classOf(root), cPrim)) { unsigned char q[0x30] = {0}; api->process_event(root, fSetVel, q); }
}

static void goBack(const std::string& name) {
    uintptr_t pw = pawn();
    Vec at;
    double yaw;
    if (!pw) { MT_LOG("return: not in a world"); return; }
    if (!fTeleport) { MT_LOG("return: K2_TeleportTo not found"); return; }
    if (!findSpot(name, at, yaw)) { MT_LOG("return: no spot named '%s'", name.c_str()); return; }
    at.z += 50;  // a little above, so a vehicle does not land in the ground
    teleport(pw, at, yaw);
    MT_LOG("returned to '%s' at %.0f %.0f %.0f", name.empty() ? "(last)" : name.c_str(), at.x, at.y, at.z - 50);
   
}

// tp_x_y_z / #tp x y z (the user, 2026-11-13: "whenever I type in chat tp_x_y_z it teleports me there, so I can check
// what I placed"): world cm, as in coordinates.csv; lands 1 m above. `pc` is whose pawn: on a server, the sender's.
static bool goTo(uintptr_t pc, const std::string& args) {
    double x, y, z;
    std::string a = args;
    for (char& c : a) if (c == '_' || c == ',') c = ' ';
    if (sscanf_s(a.c_str(), "%lf %lf %lf", &x, &y, &z) != 3) { MT_LOG("tp: needs x y z (world cm), got '%s'", args.c_str()); return false; }
    uintptr_t pw = 0;
    if (pc && fPawnOf >= 0) pw = get<uintptr_t>(pc + fPawnOf);
    if (!pw) pw = pawn();
    if (!pw || !fTeleport) { MT_LOG("tp: no pawn to move"); return false; }
    Vec rot{}; api->process_event(pw, fRot, &rot);
    teleport(pw, Vec{x, y, z + 100}, rot.y);
    MT_LOG("tp: %s to %.0f %.0f %.0f", objName(classOf(pw)).c_str(), x, y, z);
   
    return true;
}

// ---- #seat: seats and seated bodies, in vehicle-local cm (2026-11-12, Arini's seat pass) ----
static uintptr_t fCompsByClass, fCompLoc, fCompRot, fCompScale, fFwd, fRight, fUp, fParentActor, fNumBones, fBoneName, fSocketLoc,
    fActorScale, cSeat, cVehicle;
static int oNetChar = -1, oSeatPos = -1, oSeatType = -1, oSeatIdx = -1, oCharMesh = -1, pBoneIdx = -1, pBoneRet = -1, pSockName = -1,
    pSockRet = -1;
static const char* kSeatPos[] = {"None", "Sedan", "ScooterDriver", "ScooterPassenger", "BikeDriverUpright", "BikeDriverSport", "Bed", "Chair",
                                 "Standing_FrontGrip"};

static Vec call3(uintptr_t o, uintptr_t fn) { Vec v{}; if (fn) api->process_event(o, fn, &v); return v; }

// One file per vehicle and seat, plugins\seat_probe\<Vehicle>__<Seat>.txt: the vehicle's seat list, then the seat probed
// and every bone of whoever sits in it. Automatic (the user, 2026-11-12: "when I'm inside a vehicle, it's always going
// to grab that information"): a seat occupied for 3 s that has no file yet is written once; #seat rewrites them all.
static std::set<std::string> seatDone;
static std::map<uintptr_t, std::pair<uintptr_t, ULONGLONG>> seatSince;  // seat -> occupant, since when
static void seatProbe(bool manual) {
    uintptr_t pw = pawn();
    if (!pw) { if (manual) { MT_LOG("seat: not in a world"); } return; }
    uintptr_t veh = pw;  // a passenger may be the pawn themself: then the vehicle is what they sit on
    if (cVehicle && !api->is_a(classOf(veh), cVehicle) && fParentActor) { uintptr_t a = 0; api->process_event(pw, fParentActor, &a); if (a) veh = a; }
    if (!cVehicle || !api->is_a(classOf(veh), cVehicle)) { if (manual) { MT_LOG("seat: sit in a vehicle first"); } return; }
    Vec loc = call3(veh, fLoc), f = call3(veh, fFwd), r = call3(veh, fRight), u = call3(veh, fUp);
    auto dot = [](Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    auto local = [&](Vec w) { Vec d{w.x - loc.x, w.y - loc.y, w.z - loc.z}; return Vec{dot(d, f), dot(d, r), dot(d, u)}; };
    Vec vrot = call3(veh, fRot);
    std::string cls = objName(classOf(veh));
    struct { uintptr_t cls; uintptr_t data; int num, max; } q = {cSeat, 0, 0, 0};
    api->process_event(veh, fCompsByClass, &q);
    std::vector<uintptr_t> seats;
    for (int i = 0; q.data && i < q.num && i < 64; i++) if (uintptr_t c = get<uintptr_t>(q.data + 8ull * i)) seats.push_back(c);
    std::string list;
    std::vector<Vec> at(seats.size());
    for (size_t i = 0; i < seats.size(); i++) {
        uintptr_t c = seats[i];
        Vec w{}, rot{}, sc{};
        api->process_event(c, fCompLoc, &w);
        api->process_event(c, fCompRot, &rot);
        api->process_event(c, fCompScale, &sc);
        at[i] = local(w);
        int pos = oSeatPos >= 0 ? get<uint8_t>(c + oSeatPos) : -1;
        uintptr_t who = oNetChar >= 0 ? get<uintptr_t>(c + oNetChar) : 0;
        char b[512];
        snprintf(b, sizeof b, "seat %s index %d type %d position %s at %.1f %.1f %.1f, world rot (p %.1f y %.1f r %.1f) = yaw %.1f off the vehicle, scale %.3f %.3f %.3f%s\n",
                 objName(c).c_str(), oSeatIdx >= 0 ? get<int>(c + oSeatIdx) : -1, oSeatType >= 0 ? get<uint8_t>(c + oSeatType) : -1,
                 pos >= 0 && pos < 9 ? kSeatPos[pos] : "?", at[i].x, at[i].y, at[i].z, rot.x, rot.y, rot.z, rot.y - vrot.y, sc.x, sc.y, sc.z,
                 who ? (" -- occupied by " + objName(who)).c_str() : "");
        list += b;
    }
    int wrote = 0;
    ULONGLONG nowMs = GetTickCount64();
    for (size_t i = 0; i < seats.size(); i++) {
        uintptr_t c = seats[i], who = oNetChar >= 0 ? get<uintptr_t>(seats[i] + oNetChar) : 0;
        if (!who || oCharMesh < 0) { seatSince.erase(c); continue; }
        std::string key = cls + "__" + objName(c);
        std::wstring dir = gameDir() + L"plugins\\seat_probe", path = dir + L"\\" + std::wstring(key.begin(), key.end()) + L".txt";
        if (!manual) {
            if (seatDone.count(key)) continue;
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) { seatDone.insert(key); continue; }
            auto& since = seatSince[c];
            if (since.first != who) { since = {who, nowMs}; continue; }
            if (nowMs - since.second < 3000) continue;  // let the sit-down animation finish
        }
        CreateDirectoryW(dir.c_str(), nullptr);
        FILE* out = _wfopen(path.c_str(), L"w");
        if (!out) continue;
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(out, "== %04d-%02d-%02d %02d:%02d:%02d %s (vehicle yaw %.1f pitch %.1f roll %.1f), vehicle-local cm: bow +X, right +Y, up +Z\n",
                t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, cls.c_str(), vrot.y, vrot.x, vrot.z);
        fputs(list.c_str(), out);
        fprintf(out, "probed seat %s\n", objName(c).c_str());
        uintptr_t mesh = get<uintptr_t>(who + oCharMesh);
        Vec as = call3(who, fActorScale), ms{};
        if (mesh) api->process_event(mesh, fCompScale, &ms);
        fprintf(out, "  body %s: actor scale %.3f %.3f %.3f, mesh world scale %.3f %.3f %.3f\n", objName(classOf(who)).c_str(), as.x, as.y, as.z, ms.x, ms.y, ms.z);
        if (mesh && fNumBones && fBoneName && fSocketLoc && pBoneRet >= 0 && pSockName >= 0 && pSockRet >= 0) {
            int nb = 0; api->process_event(mesh, fNumBones, &nb);
            double top = -1e9; std::string topName;
            for (int b = 0; b < nb && b < 400; b++) {
                unsigned char bp[0x20] = {0};
                memcpy(bp + (pBoneIdx < 0 ? 0 : pBoneIdx), &b, 4);
                api->process_event(mesh, fBoneName, bp);
                unsigned char sp[0x40] = {0};
                memcpy(sp + pSockName, bp + pBoneRet, 8);  // the FName, passed straight on
                api->process_event(mesh, fSocketLoc, sp);
                Vec bw; memcpy(&bw, sp + pSockRet, 24);
                Vec bl = local(bw);
                char nm[128] = {0};
                api->fname((uintptr_t)(bp + pBoneRet), nm, sizeof nm);
                fprintf(out, "  bone %3d %-28s %.1f %.1f %.1f  (%.1f above the seat)\n", b, nm, bl.x, bl.y, bl.z, bl.z - at[i].z);
                if (bl.z > top) { top = bl.z; topName = nm; }
            }
            fprintf(out, "  highest bone %s at z %.1f, %.1f above the seat\n", topName.c_str(), top, top - at[i].z);
        }
        fclose(out);
        seatDone.insert(key);
        wrote++;
        MT_LOG("seat: %s recorded to plugins\\seat_probe\\%s.txt", key.c_str(), key.c_str());
    }
    if (manual) { MT_LOG("seat: %zu seat(s), %d written", seats.size(), wrote); }
}

static volatile LONG seatPending;
static bool kAutoSeat = true;
static void autoSeat(void*) {
    InterlockedExchange(&seatPending, 0);
    static ULONGLONG lastSearch;
    // no player controller yet (the main menu): the search walks every object, so not more than once in 30 s
    if (!pcObj || api->object(pcIdx) != pcObj) {
        if (GetTickCount64() - lastSearch < 30000) return;
        lastSearch = GetTickCount64();
    }
    seatProbe(false);
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

// Chat commands. 2026-11-13: ServerSendChat never fired once (the user's notes were lost), so a '#' line is also taken
// from ClientChat -- the server's echo of every message -- when its sender is this player; whichever comes first runs
// it, the other is skipped as a repeat. Through the echo the line has reached the server already (others on a server
// see it); through ServerSendChat it is kept off. And every "*Chat*" function is watched for its first fire, logged,
// so the log names the path the chat box really takes.
struct FStr { wchar_t* d; int n, m; };
static int msgOffFor(uintptr_t func) {
    for (const char* n : {"Message", "Msg", "Text", "ChatMessage", "InMessage", "Str"}) {
        int o = api->field_offset(func, n);
        if (o >= 0) return o;
    }
    return -1;
}
static std::string lastLine;
static double lastLineAt;
static double nowSec() { return GetTickCount64() / 1000.0; }
// Runs a '#' line once; false if it is not one.
static bool runLine(const wchar_t* d, int n, const char* via, uintptr_t pc);
static bool command(uintptr_t func, void* parms, const char* via, uintptr_t pc) {
    int off = msgOffFor(func);
    if (off < 0) return false;
    FStr str = *(FStr*)((char*)parms + off);
    if (!str.d || str.n < 2 || str.n > 1000) return false;
    return runLine(str.d, str.n - 1, via, pc);
}
// A typed line (n characters, no terminator): runs it if it is a command, once.
static bool runLine(const wchar_t* d, int n, const char* via, uintptr_t pc) {
    while (n > 0 && d[0] == L' ') { d++; n--; }
    if (n < 2) return false;
    bool tpLine = n > 3 && (d[0] == L't' || d[0] == L'T') && (d[1] == L'p' || d[1] == L'P') && d[2] == L'_';
    if (d[0] != L'#' && !tpLine) return false;
    char u[1024];
    int k = WideCharToMultiByte(CP_UTF8, 0, d, n, u, sizeof u - 1, nullptr, nullptr);
    u[k > 0 ? k : 0] = 0;
    if (lastLine == u && nowSec() - lastLineAt < 3) return true;  // the same line through the other path
    lastLine = u; lastLineAt = nowSec();
    MT_LOG("chat command via %s: %s", via, u);
    if (tpLine) return goTo(pc, std::string(u + 3)), true;
    std::string cmd, rest;
    split(u, cmd, rest);
    if (cmd == "tp") { goTo(pc, rest); return true; }
    // The POI Teleport plugin has its own commands; they are not this plugin's, and they are
    // not an error here either (2026-11-17: the plugin was beeping "unknown command" at them).
    if (cmd == "poi" || cmd == "pois" || cmd == "poireload") return true;
    if (cmd == "note") save("", rest.empty() ? "(empty note)" : rest);
    else if (cmd == "save") save(rest, "");
    else if (cmd == "return" || cmd == "back") goBack(rest);
    else if (cmd == "seat") seatProbe(true);
    else { MT_LOG("chat: unknown command #%s (note, save, return, seat)", cmd.c_str()); }
    return true;
}
// ServerSendChat: run it and keep it off the server
// ServerSendChat runs on the sending player's controller (on a server: the sender's, which is whose pawn tp moves).
// A client with no authority over its pawn (someone else's server) still teleports locally, but lets a tp line through,
// so a server running this plugin can do it for real; '#' notes are kept off as before.
static uintptr_t fHasAuthC;
static int onChat(uintptr_t self, uintptr_t func, void* parms, void*) {
    if (!command(func, parms, "ServerSendChat", self)) return 0;
    if (lastLine.rfind("tp_", 0) == 0 || lastLine.rfind("TP_", 0) == 0 || lastLine.rfind("#tp", 0) == 0) {
        uintptr_t pw = fPawnOf >= 0 ? get<uintptr_t>(self + fPawnOf) : 0;
        bool auth = true;
        if (pw && fHasAuthC) { auth = false; api->process_event(pw, fHasAuthC, &auth); }
        if (!auth) return 0;  // on to the server
    }
    return 1;
}
// ClientChat(sender PlayerState, Message, Category): only this player's own lines
static int oCtrlPS = -1;
static int onClientChat(uintptr_t, uintptr_t func, void* parms, void*) {
    if (!pcObj || oCtrlPS < 0) return 0;
    uintptr_t mine = get<uintptr_t>(pcObj + oCtrlPS);
    uintptr_t sender = 0;
    for (const char* n : {"PlayerState", "SenderPlayerState", "Sender", "InPlayerState", "PS"}) {
        int o = api->field_offset(func, n);
        if (o >= 0) { sender = *(uintptr_t*)((char*)parms + o); break; }
    }
    if (!sender) sender = *(uintptr_t*)parms;  // the first parameter, by the header
    if (!mine || sender != mine) return 0;
    command(func, parms, "ClientChat", pcObj);
    return 0;  // the echo is shown as usual
}
// The chat box (2026-11-14: no "*Chat*" function fired at all -- the game handles chat inside its own code). A text box
// tells its owner "text committed" through a dynamic delegate, which goes through ProcessEvent: every function with
// "Commit" or "Submit" in its name is watched, its FText / FString parameters read, and a '#' / tp_ line run from there.
static uintptr_t fTextToStr, textCDO;
static int pTxtIn = -1, pTxtRet = -1;
static std::wstring textOf(const unsigned char* ftext) {
    if (!fTextToStr || !textCDO || pTxtIn < 0 || pTxtRet <= pTxtIn) return L"";
    alignas(16) unsigned char p[0x40] = {0};
    memcpy(p + pTxtIn, ftext, pTxtRet - pTxtIn);
    api->process_event(textCDO, fTextToStr, p);
    FStr r; memcpy(&r, p + pTxtRet, sizeof r);
    if (!r.d || r.n < 1 || r.n > 1000) return L"";
    return std::wstring(r.d, r.n - 1);  // shortcut: the returned FString's buffer is not freed (a few per typed line)
}
static int onCommit(uintptr_t self, uintptr_t func, void* parms, void*) {
    static std::set<uintptr_t> told;
    for (uintptr_t pr = get<uintptr_t>(func + L().struct_child_props); pr; pr = get<uintptr_t>(pr + L().field_next)) {
        std::string type = name(get<uint32_t>(get<uintptr_t>(pr + 8)));
        int at = get<int32_t>(pr + L().field_offset);
        const unsigned char* v = (const unsigned char*)parms + at;
        std::wstring t;
        if (type == "TextProperty") t = textOf(v);
        else if (type == "StrProperty") { FStr fs; memcpy(&fs, v, sizeof fs); if (fs.d && fs.n > 1 && fs.n < 1000) t.assign(fs.d, fs.n - 1); }
        if (t.empty()) continue;
        if (told.insert(func).second) MT_LOG("text commit path: %s on %s", objName(func).c_str(), objName(classOf(self)).c_str());
        if (runLine(t.c_str(), (int)t.size(), "text commit", pcObj)) return 0;
    }
    return 0;
}

// discovery: the first fire of each "*Chat*" function, once
static int onAnyChat(uintptr_t, uintptr_t func, void*, void*) {
    static std::set<uintptr_t> told;
    if (told.insert(func).second) MT_LOG("chat path: %s fired", objName(func).c_str());
    return 0;
}

// Walks the saved spots (rows with a name) in coordinates.csv, one per press, and goes there.
static volatile LONG hopDir;
static bool hopDown;
static int hopAt = -1;
static void hopSpot(void*) {
    int dir = (int)InterlockedExchange(&hopDir, 0);
    if (!dir) return;
    std::vector<std::pair<std::string, std::vector<std::string>>> spots;
    for (auto& l : lines()) { auto c = cols(l); if (c.size() >= 7 && !c[6].empty() && c[6] != "name") spots.push_back({c[6], c}); }
    if (spots.empty()) { MT_LOG("hop: no saved spots"); return; }
    int n = (int)spots.size();
    hopAt = hopAt < 0 ? (dir > 0 ? 0 : n - 1) : (hopAt + dir + n) % n;
    auto& c = spots[hopAt].second;
    uintptr_t pw = pawn();
    if (!pw || !fTeleport) { MT_LOG("hop: no pawn"); return; }
    teleport(pw, Vec{atof(c[1].c_str()), atof(c[2].c_str()), atof(c[3].c_str()) + 100}, atof(c[4].c_str()));
    MT_LOG("hop %d/%d: %s at %s %s %s", hopAt + 1, n, spots[hopAt].first.c_str(), c[1].c_str(), c[2].c_str(), c[3].c_str());
   
}

static DWORD WINAPI watchChat(LPVOID) {
    // the chat RPC exists only once the game has loaded its player classes: retry until it registers
    static const char* const names[] = {"ServerSendChat"};
    while (!api->watch_functions(names, 1, onChat, nullptr)) Sleep(2000);
    static const char* const echo[] = {"ClientChat"};
    bool e = api->watch_functions(echo, 1, onClientChat, nullptr) != 0;
    if (uintptr_t cC = api->find_object("Class", "Controller", "/Script/Engine")) { oCtrlPS = off(cC, "PlayerState"); fPawnOf = off(cC, "Pawn"); }
    fHasAuthC = api->find_object("Function", "HasAuthority", "Actor");
    // every function with "Chat" in its name, for the log
    static std::vector<std::string> found;
    static std::vector<const char*> ptrs;
    if (uintptr_t cFn = api->find_object("Class", "Function", "/Script/CoreUObject")) {
        std::set<std::string> uniq;
        for (int i = 0, n = api->num_objects(); i < n && uniq.size() < 64; i++) {
            uintptr_t o = api->object(i);
            if (!o || classOf(o) != cFn) continue;
            std::string nm = objName(o);
            if (nm.find("Chat") != std::string::npos) uniq.insert(nm);
        }
        found.assign(uniq.begin(), uniq.end());
        for (auto& f : found) ptrs.push_back(f.c_str());
        if (!ptrs.empty()) api->watch_functions(ptrs.data(), (int)ptrs.size(), onAnyChat, nullptr);
        // text commits: every "*Commit*" / "*Submit*" function
        fTextToStr = api->find_object("Function", "Conv_TextToString", "KismetTextLibrary");
        textCDO = api->find_object("KismetTextLibrary", "Default__KismetTextLibrary", "/Script/Engine");
        if (fTextToStr) { pTxtIn = off(fTextToStr, "InText"); pTxtRet = off(fTextToStr, "ReturnValue"); }
        static std::vector<std::string> cfound;
        static std::vector<const char*> cptrs;
        std::set<std::string> cu;
        for (int i = 0, n = api->num_objects(); i < n && cu.size() < 128; i++) {
            uintptr_t o = api->object(i);
            if (!o || classOf(o) != cFn) continue;
            std::string nm = objName(o);
            if (nm.find("Commit") != std::string::npos || nm.find("Submit") != std::string::npos) cu.insert(nm);
        }
        cfound.assign(cu.begin(), cu.end());
        for (auto& f : cfound) cptrs.push_back(f.c_str());
        if (!cptrs.empty()) api->watch_functions(cptrs.data(), (int)cptrs.size(), onCommit, nullptr);
        MT_LOG("text commits watched: %zu functions", cptrs.size());
    }
    MT_LOG("chat commands on: #note <text>, #save <name>, #return [name], tp_x_y_z / #tp x y z (ServerSendChat%s; %zu chat functions watched for the log)",
           e ? " + ClientChat echo" : "", ptrs.size());
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
    fCompsByClass = api->find_object("Function", "K2_GetComponentsByClass", "Actor");
    fCompLoc = api->find_object("Function", "K2_GetComponentLocation", "SceneComponent");
    fCompRot = api->find_object("Function", "K2_GetComponentRotation", "SceneComponent");
    fCompScale = api->find_object("Function", "K2_GetComponentScale", "SceneComponent");
    fSocketLoc = api->find_object("Function", "GetSocketLocation", "SceneComponent");
    fFwd = api->find_object("Function", "GetActorForwardVector", "Actor");
    fRight = api->find_object("Function", "GetActorRightVector", "Actor");
    fUp = api->find_object("Function", "GetActorUpVector", "Actor");
    fActorScale = api->find_object("Function", "GetActorScale3D", "Actor");
    fParentActor = api->find_object("Function", "GetAttachParentActor", "Actor");
    fNumBones = api->find_object("Function", "GetNumBones", "SkinnedMeshComponent");
    fBoneName = api->find_object("Function", "GetBoneName", "SkinnedMeshComponent");
    cSeat = api->find_object("Class", "MTSeatComponent", "/Script/MotorTown");
    cVehicle = api->find_object("Class", "MTVehicle", "/Script/MotorTown");
    if (cSeat) { oNetChar = off(cSeat, "Net_Character"); oSeatPos = off(cSeat, "SeatPositionType"); oSeatType = off(cSeat, "SeatType"); oSeatIdx = off(cSeat, "SeatIndex"); }
    if (uintptr_t cChar = api->find_object("Class", "Character", "/Script/Engine")) oCharMesh = off(cChar, "Mesh");
    if (fBoneName) { pBoneIdx = off(fBoneName, "BoneIndex"); pBoneRet = off(fBoneName, "ReturnValue"); }
    if (fSocketLoc) { pSockName = off(fSocketLoc, "InSocketName"); pSockRet = off(fSocketLoc, "ReturnValue"); }
    CloseHandle(CreateThread(nullptr, 0, watchChat, nullptr, 0, nullptr));
    int key = iniInt(INI, L"Coordinates", L"Hotkey", 11);
    if (key < 1 || key > 12) key = 11;
    MT_LOG("Coordinates ready: Ctrl+F%d saves a spot to plugins\\coordinates.csv", key);
    kAutoSeat = iniInt(INI, L"Coordinates", L"AutoSeat", 1) != 0;
    bool down = false;
    for (int tick = 0;; tick++) {
        if (kAutoSeat && tick % 125 == 0 && !seatPending && api->game_thread_ready()) {  // every 2 s
            InterlockedExchange(&seatPending, 1);
            api->run_on_game_thread(autoSeat, nullptr);
        }
        bool ctl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, fk = (GetAsyncKeyState(VK_F1 + key - 1) & 0x8000) != 0;
        bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0, alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        // Ctrl+Shift+F<key> / Ctrl+Alt+F<key>: the next / previous saved spot (2026-11-14: a teleport while chat is not caught)
        bool hop = ctl && fk && (shift || alt);
        if (hop && !hopDown && api->game_thread_ready()) {
            InterlockedExchange(&hopDir, shift ? 1 : -1);
            api->run_on_game_thread(hopSpot, nullptr);
        }
        hopDown = hop;
        bool k = ctl && fk && !shift && !alt;
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
