// Doodle simulator: the virtual Doodle hardware as a Windows process.
// Render = OpenGL, input = keyboard + XInput, audio = PlaySound, plus the SDK natives Doo code calls.
//
// Doodle pad -> keyboard / XInput pad
//   D-pad = arrows (left stick too)   A = Z   B = X   X = A   Y = S   L = Q   R = W
//   Start = Enter   Select = Backspace   HOME = Esc (Back on the pad)
//
// Usage: doodle [root]           boot the firmware (root defaults to the source tree)
//        doodle --check [root]   compile firmware + all games and report errors
#include <windows.h>
#include <mmsystem.h>
#include <Xinput.h>
#include <GL/gl.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include "compiler.h"

namespace fs = std::filesystem;
using Args = std::vector<Value>;

static const int W = 640, H = 480;  // virtual screen

enum Button { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_START, BTN_SELECT, BTN_HOME, NBUTTONS };
static const char* buttonNames[NBUTTONS] = {"Up", "Down", "Left", "Right", "A", "B", "X", "Y", "L", "R", "Start", "Select", "Home"};
static const int keyMap[NBUTTONS] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT, 'Z', 'X', 'A', 'S', 'Q', 'W', VK_RETURN, VK_BACK, VK_ESCAPE};
static const WORD padMap[NBUTTONS] = {
    XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
    XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
    XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_START, 0, XINPUT_GAMEPAD_BACK};

struct Program {
    std::unique_ptr<Instance> inst;
    fs::path base;  // render.image paths are relative to this
};

static fs::path root;
static VM vm;
static Program firmware, game;
static Program* active = &firmware;
static std::string crash, pendingLaunch;
static bool keyDown[256], held[NBUTTONS], was[NBUTTONS];
static double dt = 0;
static GLuint fontBase;
static GLYPHMETRICSFLOAT glyphs[256];

static bool justPressed(int b) { return held[b] && !was[b]; }

// ---------- render ----------

static void setColor(double c) {
    int v = (int)c;
    glColor3ub(GLubyte(v >> 16), GLubyte(v >> 8), GLubyte(v));
}

static void rect(double x, double y, double w, double h, double c) {
    setColor(c);
    glBegin(GL_QUADS);
    glVertex2d(x, y); glVertex2d(x + w, y); glVertex2d(x + w, y + h); glVertex2d(x, y + h);
    glEnd();
}

static std::wstring widen(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size()));
    for (auto& ch : w) if (ch > 255) ch = L'?';  // font covers Latin-1 (accents included)
    return w;
}

static void textW(double x, double y, const std::wstring& s, double size, double c) {
    setColor(c);
    glPushMatrix();
    glTranslated(x, y + size * 0.8, 0);  // y = top of the text; glyph outlines sit on the baseline
    glScaled(size, -size, 1);            // glyphs are y-up, the screen is y-down
    glListBase(fontBase);
    glCallLists((GLsizei)s.size(), GL_UNSIGNED_SHORT, s.data());
    glPopMatrix();
}

static void text(double x, double y, const std::string& s, double size, double c) { textW(x, y, widen(s), size, c); }

static double textWidth(const std::string& s, double size) {
    double w = 0;
    for (wchar_t ch : widen(s)) w += glyphs[ch].gmfCellIncX;
    return w * size;
}

struct Image { GLuint tex = 0; int w = 0, h = 0; };
static std::map<fs::path, Image> images;

// ponytail: BMP only (native LoadImage), magenta = transparent; add stb_image/WIC when PNG with alpha is needed
static const Image& image(const fs::path& p) {
    auto it = images.find(p);
    if (it != images.end()) return it->second;
    Image& img = images[p];  // a failed load stays cached as tex 0
    HBITMAP bmp = (HBITMAP)LoadImageW(nullptr, p.c_str(), IMAGE_BITMAP, 0, 0, LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!bmp) return img;
    BITMAP bm;
    GetObject(bmp, sizeof bm, &bm);
    img.w = bm.bmWidth;
    img.h = bm.bmHeight;
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), img.w, -img.h, 1, 32, BI_RGB};  // 32-bit top-down BGRA
    std::vector<uint32_t> px(size_t(img.w) * img.h);
    HDC dc = GetDC(nullptr);
    GetDIBits(dc, bmp, 0, img.h, px.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    DeleteObject(bmp);
    for (auto& c : px) c = (c & 0xFFFFFF) == 0xFF00FF ? 0 : c | 0xFF000000;
    glGenTextures(1, &img.tex);
    glBindTexture(GL_TEXTURE_2D, img.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.w, img.h, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, px.data());
    return img;
}

// ---------- audio ----------

static std::vector<unsigned char> wav;

// ponytail: PlaySound = one sound at a time (a new one cuts the last); move to a waveOut/XAudio2 mixer for overlapping sounds
static void tone(double freq, double ms) {
    PlaySoundW(nullptr, nullptr, 0);  // stop before reusing the buffer
    const int rate = 22050, n = std::max(1, int(rate * ms / 1000));
    wav.assign(44 + n, 0);
    auto put = [](unsigned char* p, uint32_t v, int bytes) { memcpy(p, &v, bytes); };
    memcpy(&wav[0], "RIFF", 4); put(&wav[4], 36 + n, 4); memcpy(&wav[8], "WAVEfmt ", 8);
    put(&wav[16], 16, 4); put(&wav[20], 1, 2); put(&wav[22], 1, 2);           // PCM, mono
    put(&wav[24], rate, 4); put(&wav[28], rate, 4); put(&wav[32], 1, 2); put(&wav[34], 8, 2);  // 8-bit
    memcpy(&wav[36], "data", 4); put(&wav[40], n, 4);
    for (int i = 0; i < n; i++) {  // square wave with a linear fade-out so notes don't click
        double env = 1.0 - double(i) / n;
        wav[44 + i] = (unsigned char)(128 + (std::fmod(i * freq / rate, 1.0) < 0.5 ? 40 : -40) * env);
    }
    PlaySoundW((LPCWSTR)wav.data(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

// ---------- input ----------

static void pollInput() {
    // ponytail: an empty XInput slot is slow to query, so a missing pad is re-checked every ~2s
    static bool padConnected = true;
    static int padRetry = 0;
    XINPUT_STATE xs = {};
    bool pad = false;
    if (padConnected || --padRetry <= 0) {
        pad = padConnected = XInputGetState(0, &xs) == ERROR_SUCCESS;
        padRetry = 120;
    }
    const XINPUT_GAMEPAD& g = xs.Gamepad;
    for (int i = 0; i < NBUTTONS; i++) {
        was[i] = held[i];
        held[i] = keyDown[keyMap[i]] || (pad && (g.wButtons & padMap[i]));
    }
    if (pad) {  // left stick doubles as the d-pad
        const int dz = 16000;
        held[BTN_UP] |= g.sThumbLY > dz;
        held[BTN_DOWN] |= g.sThumbLY < -dz;
        held[BTN_LEFT] |= g.sThumbLX < -dz;
        held[BTN_RIGHT] |= g.sThumbLX > dz;
    }
}

// ---------- programs: firmware and games ----------

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("não foi possível abrir " + p.u8string());
    return {std::istreambuf_iterator<char>(f), {}};
}

static fs::path gameDir(const std::string& id) { return root / "games" / fs::u8path(id); }

static std::vector<std::string> installedGames() {
    std::vector<std::string> ids;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root / "games", ec))
        if (fs::exists(e.path() / "main.doo")) ids.push_back(e.path().filename().u8string());
    return ids;
}

static void load(Program& prog, const fs::path& base, const std::string& file, bool privileged) {
    prog.inst.reset();
    prog.base = base;
    active = &prog;
    prog.inst = vm.instantiate(compile(readFile(root / fs::u8path(file)), file, vm, privileged));
}

static void bootFirmware() {
    game.inst.reset();
    load(firmware, root, "firmware/main.doo", true);
}

static void backToFirmware() {
    game.inst.reset();
    active = &firmware;
}

static void guarded(void (*fn)()) {
    try {
        fn();
    } catch (const std::exception& e) {
        crash = e.what();
        fprintf(stderr, "%s\n", crash.c_str());
    }
}

// ---------- SDK ----------

static double num(Args& a, size_t i) {
    if (i >= a.size() || !std::holds_alternative<double>(a[i]))
        throw std::runtime_error("argumento " + std::to_string(i + 1) + " deve ser um número");
    return std::get<double>(a[i]);
}

static std::string str(Args& a, size_t i) {
    if (i >= a.size()) throw std::runtime_error("faltou o argumento " + std::to_string(i + 1));
    return toString(a[i]);
}

static int button(Args& a) {
    int b = (int)num(a, 0);
    if (b < 0 || b >= NBUTTONS) throw std::runtime_error("botão inválido");
    return b;
}

static void registerSdk() {
    for (int i = 0; i < NBUTTONS; i++) vm.constants["Button." + std::string(buttonNames[i])] = Value(double(i));

    vm.addNative("rgb", [](Instance&, Args& a) {
        auto c = [&](size_t i) { return std::clamp((int)num(a, i), 0, 255); };
        return Value(double(c(0) << 16 | c(1) << 8 | c(2)));
    });
    vm.addNative("render.clear", [](Instance&, Args& a) { rect(0, 0, W, H, num(a, 0)); return Value(); });
    vm.addNative("render.rect", [](Instance&, Args& a) {
        rect(num(a, 0), num(a, 1), num(a, 2), num(a, 3), num(a, 4));
        return Value();
    });
    vm.addNative("render.text", [](Instance&, Args& a) {
        text(num(a, 0), num(a, 1), str(a, 2), num(a, 3), num(a, 4));
        return Value();
    });
    vm.addNative("render.text_width", [](Instance&, Args& a) { return Value(textWidth(str(a, 0), num(a, 1))); });
    vm.addNative("render.image", [](Instance&, Args& a) {  // (path, x, y, w, h) -> false if the file is missing
        const Image& img = image(active->base / fs::u8path(str(a, 0)));
        if (!img.tex) return Value(false);
        double x = num(a, 1), y = num(a, 2), w = num(a, 3), h = num(a, 4);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, img.tex);
        glColor3ub(255, 255, 255);
        glBegin(GL_QUADS);
        glTexCoord2d(0, 0); glVertex2d(x, y);
        glTexCoord2d(1, 0); glVertex2d(x + w, y);
        glTexCoord2d(1, 1); glVertex2d(x + w, y + h);
        glTexCoord2d(0, 1); glVertex2d(x, y + h);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        return Value(true);
    });

    vm.addNative("input.pressed", [](Instance&, Args& a) { return Value(held[button(a)]); });  // held down
    vm.addNative("input.just_pressed", [](Instance&, Args& a) { return Value(justPressed(button(a))); });

    vm.addNative("audio.play", [](Instance&, Args& a) { tone(num(a, 0), num(a, 1)); return Value(); });  // (Hz, ms)
    vm.addNative("audio.stop", [](Instance&, Args&) { PlaySoundW(nullptr, nullptr, 0); return Value(); });

    vm.addNative("time.delta", [](Instance&, Args&) { return Value(dt); });

    // store: local stubs until the real store exists
    vm.addNative("store.installed", [](Instance&, Args&) {
        auto list = std::make_shared<Array>();
        for (auto& id : installedGames()) list->push_back(Value(id));
        return Value(list);
    });
    vm.addNative("store.is_installed", [](Instance&, Args& a) { return Value(fs::exists(gameDir(str(a, 0)) / "main.doo")); });
    vm.addNative("store.get_save_data", [](Instance&, Args&) { return Value(std::string()); });  // no saves yet

    // system: firmware only (enforced by the compiler)
    vm.addNative("system.launch", [](Instance&, Args& a) { pendingLaunch = str(a, 0); return Value(); });
}

// ---------- main loop ----------

static void frame() {
    if (!crash.empty()) {
        rect(0, 0, W, H, 0x301010);
        text(30, 30, "Erro", 32, 0xFF6060);
        std::wstring msg = widen(crash);
        for (size_t i = 0; i * 60 < msg.size(); i++) textW(30, 90 + i * 22.0, msg.substr(i * 60, 60), 16, 0xFFFFFF);
        text(30, H - 50, "HOME (Esc) para voltar", 18, 0xAAAAAA);
        if (justPressed(BTN_HOME)) {
            crash.clear();
            if (active == &game) backToFirmware(); else guarded(bootFirmware);  // a crashed firmware reboots from disk
        }
        return;
    }
    if (active == &game && justPressed(BTN_HOME)) {  // HOME always belongs to the system
        guarded([] { vm.call(*game.inst, "destroy"); });
        backToFirmware();
        return;
    }
    guarded([] {
        Instance& inst = *active->inst;
        vm.call(inst, "update");
        if (!inst.alive) {  // destroy_self(): a game exits to the menu, the firmware powers off
            vm.call(inst, "destroy");
            if (active == &game) backToFirmware(); else PostQuitMessage(0);
        }
        if (!pendingLaunch.empty()) {
            std::string id;
            id.swap(pendingLaunch);
            load(game, gameDir(id), "games/" + id + "/main.doo", false);
        }
    });
}

static int checkAll() {
    auto check = [](const std::string& file, bool privileged) {
        try {
            compile(readFile(root / fs::u8path(file)), file, vm, privileged);
            printf("ok    %s\n", file.c_str());
            return true;
        } catch (const std::exception& e) {
            printf("ERRO  %s\n", e.what());
            return false;
        }
    };
    bool ok = check("firmware/main.doo", true);
    for (auto& id : installedGames()) ok &= check("games/" + id + "/main.doo", false);
    return ok ? 0 : 1;
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: keyDown[w & 0xFF] = msg == WM_KEYDOWN; return 0;
    case WM_KILLFOCUS: memset(keyDown, 0, sizeof keyDown); return 0;
    case WM_CLOSE: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

int main(int argc, char** argv) {
    bool check = false;
    root = fs::u8path(DOODLE_ROOT);
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--check") check = true;
        else root = argv[i];
    }
    registerSdk();
    if (check) return checkAll();

    SetProcessDPIAware();
    WNDCLASSW wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Doodle";
    RegisterClassW(&wc);
    RECT r = {0, 0, W * 3 / 2, H * 3 / 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"Doodle", L"Doodle Simulator", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    wglMakeCurrent(dc, wglCreateContext(dc));
    // ponytail: frame pacing relies on vsync; add a sleep-based limiter if some driver ignores it
    if (auto swapInterval = (BOOL(WINAPI*)(int))wglGetProcAddress("wglSwapIntervalEXT")) swapInterval(1);

    glMatrixMode(GL_PROJECTION);
    glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SelectObject(dc, CreateFontW(-64, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI"));
    fontBase = glGenLists(256);
    if (!wglUseFontOutlinesW(dc, 32, 224, fontBase + 32, 0, 0, WGL_FONT_POLYGONS, glyphs + 32))
        fprintf(stderr, "aviso: fonte não carregou, textos não vão aparecer\n");

    guarded(bootFirmware);
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (IsIconic(hwnd)) { Sleep(50); continue; }
        auto now = std::chrono::steady_clock::now();
        dt = std::min(std::chrono::duration<double>(now - last).count(), 0.1);
        last = now;

        RECT rc;
        GetClientRect(hwnd, &rc);
        int vw = std::min<int>(rc.right, rc.bottom * W / H), vh = vw * H / W;  // 4:3 letterbox
        glViewport((rc.right - vw) / 2, (rc.bottom - vh) / 2, vw, vh);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        pollInput();
        frame();
        SwapBuffers(dc);
    }
}
