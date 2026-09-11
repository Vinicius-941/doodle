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
#include <GL/glu.h>
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include "compiler.h"
#include "obj.h"
#include "physics.h"

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

// A running program (firmware or game): its objects and the instances alive in its scene.
struct Program {
    std::unordered_map<std::string, std::shared_ptr<ObjectDef>> objects;
    std::vector<std::shared_ptr<Instance>> scene;  // scene[0] = the object from main.doo (the program's root)
    fs::path base;                                 // render.image paths are relative to this
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

// ---------- render: 2D screen / 3D camera ----------

struct Camera { Vec3 pos{0, 3, 8}, target{0, 0, 0}; double fov = 60; };
static Camera cam;
static int mode = -1;  // projection in use: 0 = 2D screen, 1 = 3D camera, -1 = must re-apply

// Every draw call picks its projection, so games can mix 3D scenes and a 2D HUD freely.
// 2D always draws and writes the nearest depth, so it stays on top of 3D drawn later in the frame (HUD).
static void mode2D() {
    if (mode == 0) return;
    mode = 0;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_ALWAYS);
    glDisable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, H, 0, 0, 1);  // z = 0 -> depth 0 (nearest)
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

static void mode3D() {
    if (mode == 1) return;
    mode = 1;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_LIGHTING);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(cam.fov, double(W) / H, 0.1, 500);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(cam.pos.x, cam.pos.y, cam.pos.z, cam.target.x, cam.target.y, cam.target.z, 0, 1, 0);
    // ponytail: one fixed directional "sun"; Light (Point/Directional/Spot) from doc §5 needs a render.light API
    static const GLfloat sun[] = {-0.5f, 1.0f, -0.7f, 0.0f};  // w = 0: directional, fixed in world space
    glLightfv(GL_LIGHT0, GL_POSITION, sun);
}

// Unity-style primitives at unit size: Cube 1, Sphere Ø1, Cylinder and Capsule Ø1 x 2 tall, Plane 1x1 facing up.
// All carry texture coords (GL convention: t = 0 at the bottom of the image).
enum Mesh { MESH_CUBE, MESH_SPHERE, MESH_CYLINDER, MESH_CAPSULE, MESH_PLANE, NMESHES };
static const char* meshNames[NMESHES] = {"Cube", "Sphere", "Cylinder", "Capsule", "Plane"};
static GLuint meshBase;

static void buildMeshes() {
    GLUquadric* q = gluNewQuadric();
    gluQuadricTexture(q, GL_TRUE);
    meshBase = glGenLists(NMESHES);

    glNewList(meshBase + MESH_CUBE, GL_COMPILE);
    glBegin(GL_QUADS);
    for (int axis = 0; axis < 3; axis++) {
        static const int uvAxes[3][2] = {{2, 1}, {0, 2}, {0, 1}};  // texture s/t axes per face; side faces keep t = +y
        for (int side = -1; side <= 1; side += 2) {                // one face per axis direction
            double n[3] = {};
            n[axis] = side;
            glNormal3dv(n);
            static const int corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (auto& c : corners) {
                double p[3];
                p[axis] = 0.5 * side;
                p[uvAxes[axis][0]] = 0.5 * c[0];
                p[uvAxes[axis][1]] = 0.5 * c[1];
                glTexCoord2d((c[0] + 1) / 2, (c[1] + 1) / 2);
                glVertex3dv(p);
            }
        }
    }
    glEnd();
    glEndList();

    glNewList(meshBase + MESH_SPHERE, GL_COMPILE);  // poles on Y, so textures wrap around the vertical axis
    glPushMatrix();
    glRotated(-90, 1, 0, 0);
    gluSphere(q, 0.5, 24, 16);
    glPopMatrix();
    glEndList();

    glNewList(meshBase + MESH_CYLINDER, GL_COMPILE);  // GLU builds along +Z; rotate it to stand on Y
    glPushMatrix();
    glRotated(-90, 1, 0, 0);
    glTranslated(0, 0, -1);
    gluCylinder(q, 0.5, 0.5, 2, 24, 1);
    gluQuadricOrientation(q, GLU_INSIDE);  // bottom cap faces down
    gluDisk(q, 0, 0.5, 24, 1);
    gluQuadricOrientation(q, GLU_OUTSIDE);
    glTranslated(0, 0, 2);
    gluDisk(q, 0, 0.5, 24, 1);
    glPopMatrix();
    glEndList();

    glNewList(meshBase + MESH_CAPSULE, GL_COMPILE);  // cylinder 1 tall + a sphere on each end
    glPushMatrix();
    glRotated(-90, 1, 0, 0);
    glTranslated(0, 0, -0.5);
    gluCylinder(q, 0.5, 0.5, 1, 24, 1);
    gluSphere(q, 0.5, 24, 16);
    glTranslated(0, 0, 1);
    gluSphere(q, 0.5, 24, 16);
    glPopMatrix();
    glEndList();

    glNewList(meshBase + MESH_PLANE, GL_COMPILE);
    glBegin(GL_QUADS);
    glNormal3d(0, 1, 0);
    glTexCoord2d(0, 1); glVertex3d(-0.5, 0, -0.5);
    glTexCoord2d(0, 0); glVertex3d(-0.5, 0, 0.5);
    glTexCoord2d(1, 0); glVertex3d(0.5, 0, 0.5);
    glTexCoord2d(1, 1); glVertex3d(0.5, 0, -0.5);
    glEnd();
    glEndList();

    gluDeleteQuadric(q);
}

static void setColor(double c) {
    int v = (int)c;
    glColor3ub(GLubyte(v >> 16), GLubyte(v >> 8), GLubyte(v));
}

static void rect(double x, double y, double w, double h, double c) {
    mode2D();
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
    mode2D();
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

// Decodes PNG/JPG/BMP/GIF with WIC (the codecs built into Windows) into 32-bit BGRA, top row first.
static bool decodeImage(const fs::path& p, int& w, int& h, std::vector<uint32_t>& px) {
    static IWICImagingFactory* wic = [] {
        IWICImagingFactory* f = nullptr;
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        return f;
    }();
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT uw = 0, uh = 0;
    bool ok = wic && SUCCEEDED(wic->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
              SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
              SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(conv->GetSize(&uw, &uh));
    if (ok) {
        w = (int)uw;
        h = (int)uh;
        px.resize(size_t(w) * h);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, UINT(px.size() * 4), reinterpret_cast<BYTE*>(px.data())));
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

static const Image& image(const fs::path& p) {
    auto it = images.find(p);
    if (it != images.end()) return it->second;
    Image& img = images[p];  // a failed load stays cached as tex 0
    std::vector<uint32_t> px;
    if (!decodeImage(p, img.w, img.h, px)) return img;
    for (int y = 0; y < img.h / 2; y++)  // bottom row first: GL textures (and .obj UVs) start at the bottom
        std::swap_ranges(px.begin() + size_t(y) * img.w, px.begin() + size_t(y + 1) * img.w, px.end() - size_t(y + 1) * img.w);
    for (auto& c : px) if ((c & 0xFFFFFF) == 0xFF00FF) c = 0;  // magenta = transparent, for images without alpha
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

// Every .doo of a program folder (one object each); main.doo first, since its object is the root.
static std::vector<SourceFile> sources(const std::string& dir) {
    std::vector<SourceFile> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root / fs::u8path(dir), ec))
        if (e.path().extension() == ".doo") files.push_back({dir + "/" + e.path().filename().u8string(), readFile(e.path())});
    auto main = std::find_if(files.begin(), files.end(), [&](const SourceFile& f) { return f.file == dir + "/main.doo"; });
    if (main == files.end()) throw std::runtime_error(dir + "/main.doo não encontrado");
    std::iter_swap(files.begin(), main);
    return files;
}

// Instances join the scene before create(), so whatever they spawn in create() comes after them.
static std::shared_ptr<Instance> spawnIn(Program& prog, const std::shared_ptr<ObjectDef>& def, const Vec3* position) {
    auto inst = vm.instantiate(def);
    if (Value* p = inst->field("position"); p && position) *p = Value(*position);
    prog.scene.push_back(inst);
    vm.call(*inst, "create");
    return inst;
}

static void load(Program& prog, const std::string& dir, const fs::path& base, bool privileged) {
    prog = Program{};
    prog.base = base;
    active = &prog;
    cam = {};  // each program starts with the default camera
    mode = -1;
    auto defs = compileAll(sources(dir), vm, privileged);
    for (auto& d : defs) prog.objects[d->name] = d;
    spawnIn(prog, defs[0], nullptr);
}

static void unload(Program& prog) {  // destroy() on everything still alive, then drop the program
    auto scene = std::move(prog.scene);
    prog = Program{};
    for (auto& inst : scene) if (inst->alive) vm.call(*inst, "destroy");
}

static void bootFirmware() {
    game = Program{};
    load(firmware, "firmware", root, true);
}

static void backToFirmware() {
    game = Program{};
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

static std::string str(Args& a, size_t i) {
    if (i >= a.size()) throw std::runtime_error("faltou o argumento " + std::to_string(i + 1));
    return toString(a[i]);
}

static int button(Args& a) {
    int b = (int)argNum(a, 0);
    if (b < 0 || b >= NBUTTONS) throw std::runtime_error("botão inválido");
    return b;
}

static GLuint texture(const fs::path& p) {  // like image(), but a missing texture is an error
    GLuint t = image(p).tex;
    if (!t) throw std::runtime_error("imagem não encontrada ou inválida: " + p.filename().u8string());
    return t;
}

// .obj models: one display list per material, built on first use and cached.
struct Model {
    struct Part { GLuint list, tex; Vec3 color; };
    std::vector<Part> parts;
};
static std::map<fs::path, Model> models;

static const Model& model(const fs::path& p) {
    if (auto it = models.find(p); it != models.end()) return it->second;
    fs::path dir = p.parent_path();
    std::vector<ObjPart> parts;
    try {
        parts = parseObj(readFile(p), [&](const std::string& f) { return readFile(dir / fs::u8path(f)); });
    } catch (const std::exception& e) {
        throw std::runtime_error(p.filename().u8string() + ": " + e.what());
    }
    Model m;
    for (auto& part : parts) {
        GLuint list = glGenLists(1);
        glNewList(list, GL_COMPILE);
        glBegin(GL_TRIANGLES);
        for (auto& v : part.tris) {
            glNormal3d(v.normal.x, v.normal.y, v.normal.z);
            glTexCoord2d(v.u, v.v);
            glVertex3d(v.pos.x, v.pos.y, v.pos.z);
        }
        glEnd();
        glEndList();
        m.parts.push_back({list, part.texture.empty() ? 0 : texture(dir / fs::u8path(part.texture)), part.color});
    }
    return models[p] = std::move(m);
}

static void drawPart(GLuint list, GLuint tex, Vec3 rgb) {  // lit color x texture (GL_MODULATE)
    glColor3d(rgb.x, rgb.y, rgb.z);
    if (tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    glCallList(list);
    if (tex) glDisable(GL_TEXTURE_2D);
}

static void registerSdk() {
    for (int i = 0; i < NBUTTONS; i++) vm.constants["Button." + std::string(buttonNames[i])] = Value(double(i));
    for (int i = 0; i < NMESHES; i++) vm.constants["Mesh." + std::string(meshNames[i])] = Value(double(i));
    registerPhysics(vm);

    vm.addNative("spawn", [](Instance&, Args& a) {  // (Object, position?) -> ref to the new instance
        std::string name = str(a, 0);
        auto it = active->objects.find(name);
        if (it == active->objects.end()) throw std::runtime_error("o objeto '" + name + "' não existe neste programa");
        Vec3 pos = a.size() > 1 ? argVec(a, 1) : Vec3{};
        return Value(Ref{spawnIn(*active, it->second, a.size() > 1 ? &pos : nullptr)});
    });

    vm.addNative("rgb", [](Instance&, Args& a) {
        auto c = [&](size_t i) { return std::clamp((int)argNum(a, i), 0, 255); };
        return Value(double(c(0) << 16 | c(1) << 8 | c(2)));
    });
    vm.addNative("render.clear", [](Instance&, Args& a) {  // fills the screen (scissored to it) and resets depth
        int c = (int)argNum(a, 0);
        glClearColor((c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return Value();
    });
    vm.addNative("render.rect", [](Instance&, Args& a) {
        rect(argNum(a, 0), argNum(a, 1), argNum(a, 2), argNum(a, 3), argNum(a, 4));
        return Value();
    });
    vm.addNative("render.text", [](Instance&, Args& a) {
        text(argNum(a, 0), argNum(a, 1), str(a, 2), argNum(a, 3), argNum(a, 4));
        return Value();
    });
    vm.addNative("render.text_width", [](Instance&, Args& a) { return Value(textWidth(str(a, 0), argNum(a, 1))); });
    vm.addNative("render.image", [](Instance&, Args& a) {  // (path, x, y, w, h) -> false if the file is missing
        const Image& img = image(active->base / fs::u8path(str(a, 0)));
        if (!img.tex) return Value(false);
        double x = argNum(a, 1), y = argNum(a, 2), w = argNum(a, 3), h = argNum(a, 4);
        mode2D();
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, img.tex);
        glColor3ub(255, 255, 255);
        glBegin(GL_QUADS);
        glTexCoord2d(0, 1); glVertex2d(x, y);  // screen y grows down, texture t grows up
        glTexCoord2d(1, 1); glVertex2d(x + w, y);
        glTexCoord2d(1, 0); glVertex2d(x + w, y + h);
        glTexCoord2d(0, 0); glVertex2d(x, y + h);
        glEnd();
        glDisable(GL_TEXTURE_2D);
        return Value(true);
    });

    // ponytail: perspective only; add an orthographic variant when a game needs it
    vm.addNative("render.camera", [](Instance&, Args& a) {  // (position, target, fov = 60)
        cam = {argVec(a, 0), argVec(a, 1), a.size() > 2 ? argNum(a, 2) : 60};
        mode = -1;
        return Value();
    });
    // (Mesh.X or "model.obj", position, rotation in degrees, scale, color, texture = "")
    // color tints: 0xFFFFFF keeps a model's own colors/textures. texture (optional) replaces the material's.
    vm.addNative("render.mesh", [](Instance&, Args& a) {
        const Model* mdl = nullptr;
        int m = -1;
        if (auto path = a.empty() ? nullptr : std::get_if<std::string>(&a[0])) {
            mdl = &model(active->base / fs::u8path(*path));
        } else {
            m = (int)argNum(a, 0);
            if (m < 0 || m >= NMESHES) throw std::runtime_error("mesh inválida");
        }
        Vec3 p = argVec(a, 1), r = argVec(a, 2), s;
        if (a.size() > 3 && std::holds_alternative<double>(a[3])) {  // a number = uniform scale
            double k = argNum(a, 3);
            s = {k, k, k};
        } else {
            s = argVec(a, 3);
        }
        int c = (int)argNum(a, 4);
        Vec3 tint{(c >> 16 & 255) / 255.0, (c >> 8 & 255) / 255.0, (c & 255) / 255.0};
        GLuint tex = a.size() > 5 && !str(a, 5).empty() ? texture(active->base / fs::u8path(str(a, 5))) : 0;
        mode3D();
        glPushMatrix();
        glTranslated(p.x, p.y, p.z);
        glRotated(r.y, 0, 1, 0);  // Unity order: Z, then X, then Y
        glRotated(r.x, 1, 0, 0);
        glRotated(r.z, 0, 0, 1);
        glScaled(s.x, s.y, s.z);
        if (mdl) {
            for (auto& part : mdl->parts) drawPart(part.list, tex ? tex : part.tex, {tint.x * part.color.x, tint.y * part.color.y, tint.z * part.color.z});
        } else {
            drawPart(meshBase + m, tex, tint);
        }
        glPopMatrix();
        return Value();
    });

    vm.addNative("input.pressed", [](Instance&, Args& a) { return Value(held[button(a)]); });  // held down
    vm.addNative("input.just_pressed", [](Instance&, Args& a) { return Value(justPressed(button(a))); });

    vm.addNative("audio.play", [](Instance&, Args& a) { tone(argNum(a, 0), argNum(a, 1)); return Value(); });  // (Hz, ms)
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
        guarded([] { unload(game); });
        backToFirmware();
        return;
    }
    guarded([] {
        // Frame lifecycle: update() on everyone -> physics (+ on_collision) -> draw() -> destroy() on the dead.
        // Indexed loops: instances spawned during the frame join in right away.
        auto& scene = active->scene;
        if (scene.empty()) return;
        for (size_t i = 0; i < scene.size(); i++) if (scene[i]->alive) vm.call(*scene[i], "update");
        physicsStep(vm, scene, dt);
        for (size_t i = 0; i < scene.size(); i++) if (scene[i]->alive) vm.call(*scene[i], "draw");
        for (size_t i = 0; i < scene.size(); i++) if (!scene[i]->alive) vm.call(*scene[i], "destroy");
        if (!scene[0]->alive) {  // the root destroyed itself: a game exits to the menu, the firmware powers off
            if (active == &game) {
                unload(game);
                backToFirmware();
            } else {
                PostQuitMessage(0);
            }
            return;
        }
        scene.erase(std::remove_if(scene.begin(), scene.end(), [](auto& i) { return !i->alive; }), scene.end());
        if (!pendingLaunch.empty()) {
            std::string id;
            id.swap(pendingLaunch);
            load(game, "games/" + id, gameDir(id), false);
        }
    });
}

static int checkAll() {
    auto check = [](const std::string& dir, bool privileged) {
        try {
            compileAll(sources(dir), vm, privileged);
            printf("ok    %s\n", dir.c_str());
            return true;
        } catch (const std::exception& e) {
            printf("ERRO  %s\n", e.what());
            return false;
        }
    };
    bool ok = check("firmware", true);
    for (auto& id : installedGames()) ok &= check("games/" + id, false);
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
    pfd.cDepthBits = 24;
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    wglMakeCurrent(dc, wglCreateContext(dc));
    // ponytail: frame pacing relies on vsync; add a sleep-based limiter if some driver ignores it
    if (auto swapInterval = (BOOL(WINAPI*)(int))wglGetProcAddress("wglSwapIntervalEXT")) swapInterval(1);

    glEnable(GL_LIGHT0);
    glEnable(GL_COLOR_MATERIAL);  // render.mesh color drives the lit material
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_NORMALIZE);       // keep lighting right on scaled meshes
    const GLfloat ambient[] = {0.35f, 0.35f, 0.4f, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
    buildMeshes();
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
        int vx = (rc.right - vw) / 2, vy = (rc.bottom - vh) / 2;
        glViewport(vx, vy, vw, vh);
        glDisable(GL_SCISSOR_TEST);  // black bars, then keep render.clear inside the virtual screen
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glScissor(vx, vy, vw, vh);
        glEnable(GL_SCISSOR_TEST);
        pollInput();
        frame();
        SwapBuffers(dc);
    }
}
