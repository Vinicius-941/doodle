// Doodle simulator: the virtual Doodle hardware as a Windows process.
// Render = OpenGL, input = keyboard + XInput, audio = XAudio2, plus the SDK natives Doo code calls.
//
// Doodle pad -> keyboard / XInput pad
//   D-pad = arrows (left stick too)   A = Z   B = X   X = A   Y = S   L = Q   R = W
//   Start = Enter   Select = Backspace   HOME = Esc (Back on the pad)
//
// Usage: doodle [root]           boot the firmware (root defaults to the source tree)
//        doodle --check [root]   compile firmware + all games and report errors
#include <windows.h>
#include <Xinput.h>
#include <xaudio2.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <wincodec.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <unordered_map>
#include "compiler.h"
#include "obj.h"
#include "physics.h"
#include "save.h"
#include "wav.h"

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
    std::string id;  // game folder name, or "sistema" for the firmware (names its save file)
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
static double dt = 0;         // real seconds since the last frame
static double timeScale = 1;  // games pause with time.set_scale(0); UI keeps going on time.unscaled_delta
static GLuint fontBase;
static GLYPHMETRICSFLOAT glyphs[256];

static bool justPressed(int b) { return held[b] && !was[b]; }

// ---------- render: 2D screen / 3D camera ----------

struct Camera { Vec3 pos{0, 3, 8}, target{0, 0, 0}; double fov = 60; };
static Camera cam;
static int mode = -1;  // projection in use: 0 = 2D screen, 1 = 3D camera, -1 = must re-apply

// ---------- shaders (GLSL 1.20 on the compatibility profile: the fixed-function state stays readable) ----------
// opengl32.dll only exports GL 1.1; the driver hands out the GL 2.0 shader entry points.
typedef char GLchar;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GLFN(ret, name, args) typedef ret(APIENTRY* PFN_##name) args; static PFN_##name name;
GLFN(GLuint, glCreateShader, (GLenum))
GLFN(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))
GLFN(void, glCompileShader, (GLuint))
GLFN(void, glGetShaderiv, (GLuint, GLenum, GLint*))
GLFN(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLFN(GLuint, glCreateProgram, ())
GLFN(void, glAttachShader, (GLuint, GLuint))
GLFN(void, glLinkProgram, (GLuint))
GLFN(void, glGetProgramiv, (GLuint, GLenum, GLint*))
GLFN(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))
GLFN(void, glUseProgram, (GLuint))
GLFN(GLint, glGetUniformLocation, (GLuint, const GLchar*))
GLFN(void, glUniform1i, (GLint, GLint))
GLFN(void, glUniform1f, (GLint, GLfloat))
GLFN(void, glUniform2f, (GLint, GLfloat, GLfloat))
GLFN(void, glUniform1fv, (GLint, GLsizei, const GLfloat*))

static bool loadShaderApi() {
#define LOAD(name) if (!(name = (PFN_##name)wglGetProcAddress(#name))) return false;
    LOAD(glCreateShader) LOAD(glShaderSource) LOAD(glCompileShader) LOAD(glGetShaderiv) LOAD(glGetShaderInfoLog)
    LOAD(glCreateProgram) LOAD(glAttachShader) LOAD(glLinkProgram) LOAD(glGetProgramiv) LOAD(glGetProgramInfoLog)
    LOAD(glUseProgram) LOAD(glGetUniformLocation) LOAD(glUniform1i) LOAD(glUniform1f) LOAD(glUniform2f) LOAD(glUniform1fv)
#undef LOAD
    return true;
}

// Prepended to every shader, built-in or a game's own (which must not declare #version).
static const char* shaderHeader = R"(#version 120
uniform int lightCount;       // lights of this frame, in gl_LightSource[0..7] (eye space)
uniform float lightRange[8];  // point/spot: fades to zero at this distance
uniform sampler2D tex;
uniform int useTexture;
uniform float time;           // seconds since the console started
uniform vec2 snapGrid;        // PS1 shader: the resolution vertices snap to
varying vec3 vPos;            // eye space
varying vec3 vNormal;         // eye space
varying vec4 vColor;
varying vec2 vUV;

vec3 lighting(vec3 p, vec3 n) {  // ambient + diffuse of every light, at eye-space point p with normal n
    vec3 sum = gl_LightModel.ambient.rgb;
    for (int i = 0; i < 8; i++) {
        if (i >= lightCount) break;
        vec4 lp = gl_LightSource[i].position;
        vec3 l = lp.xyz - p * lp.w;  // directional lights have w = 0
        float d = length(l);
        l /= max(d, 0.0001);
        float att = 1.0;
        if (lp.w != 0.0) {
            float x = d / lightRange[i];
            att = clamp(1.0 - x * x, 0.0, 1.0);
            att *= att;
            float cutoff = gl_LightSource[i].spotCosCutoff;  // -1 for non-spot lights
            if (cutoff >= 0.0) att *= smoothstep(cutoff, mix(cutoff, 1.0, 0.25), dot(-l, normalize(gl_LightSource[i].spotDirection)));
        }
        sum += gl_LightSource[i].diffuse.rgb * max(dot(n, l), 0.0) * att;
    }
    return sum;
}
)";

static const char* defaultVert = R"(
void main() {
    vec4 eye = gl_ModelViewMatrix * gl_Vertex;
    vPos = eye.xyz;
    vNormal = gl_NormalMatrix * gl_Normal;
    vColor = gl_Color;
    vUV = gl_MultiTexCoord0.xy;
    gl_Position = gl_ProjectionMatrix * eye;
}
)";

static const char* defaultFrag = R"(
void main() {  // per-pixel lighting
    vec4 c = vColor;
    if (useTexture != 0) c *= texture2D(tex, vUV);
    if (c.a < 0.01) discard;  // transparent texels (magenta keyed)
    gl_FragColor = vec4(c.rgb * lighting(vPos, normalize(vNormal)), c.a);
}
)";

// The PS1 look as a choice: vertices snap to a low-res grid (wobble), textures are mapped affinely (swim),
// and lighting is per vertex (Gouraud).
static const char* ps1Vert = R"(
varying vec3 vUVw;  // uv * w and w: dividing per pixel undoes perspective correction
void main() {
    vec4 eye = gl_ModelViewMatrix * gl_Vertex;
    vec4 clip = gl_ProjectionMatrix * eye;
    if (clip.w > 0.0) {
        vec2 px = floor((clip.xy / clip.w * 0.5 + 0.5) * snapGrid + 0.5);
        clip.xy = (px / snapGrid * 2.0 - 1.0) * clip.w;
    }
    gl_Position = clip;
    vColor = vec4(gl_Color.rgb * lighting(eye.xyz, normalize(gl_NormalMatrix * gl_Normal)), gl_Color.a);
    vUVw = vec3(gl_MultiTexCoord0.xy * clip.w, clip.w);
}
)";

static const char* ps1Frag = R"(
varying vec3 vUVw;
void main() {
    vec4 c = vColor;
    if (useTexture != 0) c *= texture2D(tex, vUVw.xy / vUVw.z);
    if (c.a < 0.01) discard;
    gl_FragColor = c;
}
)";

struct Shader {
    GLuint prog = 0;
    GLint lightCount = -1, lightRange = -1, tex = -1, useTexture = -1, time = -1, snapGrid = -1;
};
static bool shadersOk;                    // false: fixed-function fallback (lights per vertex, set_shader ignored)
static std::map<std::string, Shader> shaders;
static const Shader* shader;              // for this frame's 3D; reset to "padrao" every frame
static double elapsed;                    // for the `time` uniform

static Shader buildShader(const std::string& name, const std::string& vert, const std::string& frag) {
    auto stage = [&](GLenum type, const std::string& body) {
        std::string src = shaderHeader + body;
        const GLchar* p = src.c_str();
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &p, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[2048] = "";
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            throw std::runtime_error("shader " + name + (type == GL_VERTEX_SHADER ? " (.vert): " : " (.frag): ") + log);
        }
        return s;
    };
    Shader sh;
    sh.prog = glCreateProgram();
    glAttachShader(sh.prog, stage(GL_VERTEX_SHADER, vert));
    glAttachShader(sh.prog, stage(GL_FRAGMENT_SHADER, frag));
    glLinkProgram(sh.prog);
    GLint ok = 0;
    glGetProgramiv(sh.prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = "";
        glGetProgramInfoLog(sh.prog, sizeof log, nullptr, log);
        throw std::runtime_error("shader " + name + ": " + log);
    }
    sh.lightCount = glGetUniformLocation(sh.prog, "lightCount");
    sh.lightRange = glGetUniformLocation(sh.prog, "lightRange");
    sh.tex = glGetUniformLocation(sh.prog, "tex");
    sh.useTexture = glGetUniformLocation(sh.prog, "useTexture");
    sh.time = glGetUniformLocation(sh.prog, "time");
    sh.snapGrid = glGetUniformLocation(sh.prog, "snapGrid");
    return sh;
}

// ---------- lights: declared every frame (update phase), applied when 3D drawing starts ----------

enum LightType { LIGHT_DIRECTIONAL, LIGHT_POINT, LIGHT_SPOT };
struct LightDef {
    int type;
    Vec3 pos, dir, color;  // color already multiplied by intensity
    double range = 0, angle = 0;
};
static const Vec3 defaultAmbient{0.35, 0.35, 0.4};
static std::vector<LightDef> lights;  // max 8 (GL_MAX_LIGHTS); none = a default sun
static Vec3 ambient = defaultAmbient;

static void beginFrameLighting() {
    lights.clear();
    ambient = defaultAmbient;
    shader = shadersOk ? &shaders["padrao"] : nullptr;
    mode = -1;
}

static void applyLights() {  // needs the view matrix on the modelview stack: GL stores light positions in eye space
    std::vector<LightDef> ls = lights;
    if (ls.empty()) ls.push_back({LIGHT_DIRECTIONAL, {}, {0.5, -1, 0.7}, {1, 1, 1}});  // default sun
    GLfloat range[8] = {};
    for (int i = 0; i < 8; i++) {
        GLenum id = GL_LIGHT0 + i;
        if (i >= (int)ls.size()) {
            glDisable(id);
            continue;
        }
        const LightDef& l = ls[i];
        glEnable(id);
        GLfloat pos[4] = {(GLfloat)l.pos.x, (GLfloat)l.pos.y, (GLfloat)l.pos.z, 1};
        if (l.type == LIGHT_DIRECTIONAL) {  // GL wants the direction *to* the light
            pos[0] = (GLfloat)-l.dir.x; pos[1] = (GLfloat)-l.dir.y; pos[2] = (GLfloat)-l.dir.z; pos[3] = 0;
        }
        GLfloat diffuse[4] = {(GLfloat)l.color.x, (GLfloat)l.color.y, (GLfloat)l.color.z, 1}, black[4] = {0, 0, 0, 1};
        GLfloat dir[3] = {(GLfloat)l.dir.x, (GLfloat)l.dir.y, (GLfloat)l.dir.z};
        glLightfv(id, GL_POSITION, pos);
        glLightfv(id, GL_DIFFUSE, diffuse);
        glLightfv(id, GL_AMBIENT, black);
        glLightfv(id, GL_SPECULAR, black);
        glLightfv(id, GL_SPOT_DIRECTION, dir);
        glLightf(id, GL_SPOT_CUTOFF, l.type == LIGHT_SPOT ? (GLfloat)std::clamp(l.angle / 2, 1.0, 90.0) : 180.0f);
        glLightf(id, GL_SPOT_EXPONENT, l.type == LIGHT_SPOT ? 8.0f : 0.0f);
        glLightf(id, GL_CONSTANT_ATTENUATION, 1);  // fixed-function fallback only: roughly fades out by `range`
        glLightf(id, GL_QUADRATIC_ATTENUATION, l.type == LIGHT_DIRECTIONAL ? 0.0f : GLfloat(25 / std::max(0.01, l.range * l.range)));
        range[i] = (GLfloat)std::max(0.01, l.range);
    }
    GLfloat amb[4] = {(GLfloat)ambient.x, (GLfloat)ambient.y, (GLfloat)ambient.z, 1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    if (!shader) return;
    glUseProgram(shader->prog);
    glUniform1i(shader->lightCount, (GLint)std::min<size_t>(ls.size(), 8));
    glUniform1fv(shader->lightRange, 8, range);
    glUniform1i(shader->tex, 0);
    glUniform1f(shader->time, (GLfloat)elapsed);
    glUniform2f(shader->snapGrid, 320, 240);
}

// Every draw call picks its projection, so games can mix 3D scenes and a 2D HUD freely.
// 2D always draws and writes the nearest depth, so it stays on top of 3D drawn later in the frame (HUD).
static void mode2D() {
    if (mode == 0) return;
    mode = 0;
    if (shadersOk) glUseProgram(0);
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
    applyLights();
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

static Vec3 rgb01(double c) {  // 0xRRGGBB -> components in 0..1
    int v = (int)c;
    return {(v >> 16 & 255) / 255.0, (v >> 8 & 255) / 255.0, (v & 255) / 255.0};
}

static void setColor(double c, double alpha = 1) {
    int v = (int)c;
    glColor4ub(GLubyte(v >> 16), GLubyte(v >> 8), GLubyte(v), GLubyte(std::clamp(alpha, 0.0, 1.0) * 255));
}

static void rect(double x, double y, double w, double h, double c, double alpha = 1) {
    mode2D();
    setColor(c, alpha);
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

static void textW(double x, double y, const std::wstring& s, double size, double c, double alpha = 1) {
    mode2D();
    setColor(c, alpha);
    glPushMatrix();
    glTranslated(x, y + size * 0.8, 0);  // y = top of the text; glyph outlines sit on the baseline
    glScaled(size, -size, 1);            // glyphs are y-up, the screen is y-down
    glListBase(fontBase);
    glCallLists((GLsizei)s.size(), GL_UNSIGNED_SHORT, s.data());
    glPopMatrix();
}

static void text(double x, double y, const std::string& s, double size, double c, double alpha = 1) {
    textW(x, y, widen(s), size, c, alpha);
}

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
// XAudio2 (built into Windows) mixes one voice per playing sound. Sample buffers must outlive their voice:
// files stay cached for good, generated tones are owned by the voice.

struct Voice {
    int id;
    IXAudio2SourceVoice* v;
    std::shared_ptr<Wav> tone;      // samples of a generated tone
    std::weak_ptr<Instance> source; // AudioSource: follows this instance
    double volume = 1, range = 0;   // range > 0 = positional (fades out with distance to the camera)
};
static IXAudio2* xaudio;  // null when there is no audio device: games run muted
static IXAudio2MasteringVoice* master;  // its volume = system volume (firmware settings)
static std::vector<Voice> voices;
static int nextVoiceId = 1;

static void initAudio() {
    if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xaudio->CreateMasteringVoice(&master))) {
        xaudio = nullptr;
        fprintf(stderr, "aviso: nenhum dispositivo de áudio, os jogos vão rodar mudos\n");
    }
}

static Voice* playWav(const Wav& w, double volume, bool loop) {  // nullptr when muted
    if (!xaudio) return nullptr;
    IXAudio2SourceVoice* v = nullptr;
    if (FAILED(xaudio->CreateSourceVoice(&v, reinterpret_cast<const WAVEFORMATEX*>(w.format.data()))))
        throw std::runtime_error("formato de WAV não suportado (use PCM de 8 ou 16 bits)");
    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = (UINT32)w.data.size();
    buf.pAudioData = w.data.data();
    buf.Flags = XAUDIO2_END_OF_STREAM;
    if (loop) buf.LoopCount = XAUDIO2_LOOP_INFINITE;
    v->SubmitSourceBuffer(&buf);
    v->SetVolume((float)volume);
    v->Start();
    voices.push_back({nextVoiceId++, v});
    voices.back().volume = volume;
    return &voices.back();
}

static std::shared_ptr<Wav> makeTone(double freq, double ms) {  // square wave with a linear fade-out (no clicks)
    const int rate = 22050, n = std::max(1, int(rate * ms / 1000));
    WAVEFORMATEX fmt = {WAVE_FORMAT_PCM, 1, rate, rate * 2, 2, 16, 0};
    auto w = std::make_shared<Wav>();
    w->format.assign(reinterpret_cast<uint8_t*>(&fmt), reinterpret_cast<uint8_t*>(&fmt) + sizeof fmt);
    w->data.resize(size_t(n) * 2);
    auto s = reinterpret_cast<int16_t*>(w->data.data());
    for (int i = 0; i < n; i++) s[i] = int16_t((std::fmod(i * freq / rate, 1.0) < 0.5 ? 9000 : -9000) * (1.0 - double(i) / n));
    return w;
}

template <class Match>
static void stopVoices(Match match) {
    for (size_t i = 0; i < voices.size();) {
        if (!match(voices[i])) { i++; continue; }
        voices[i].v->DestroyVoice();
        voices.erase(voices.begin() + i);
    }
}

// Once per frame: drop finished voices; positional ones follow their instance and fade with distance.
// ponytail: volume only, no stereo panning; use X3DAudio when direction matters
static void updateAudio() {
    stopVoices([](Voice& vc) {
        XAUDIO2_VOICE_STATE st;
        vc.v->GetState(&st, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (st.BuffersQueued == 0) return true;
        if (vc.range <= 0) return false;
        auto inst = vc.source.lock();
        Value* p = inst && inst->alive ? inst->field("position") : nullptr;
        if (!p || !std::holds_alternative<Vec3>(*p)) return true;  // the source is gone
        Vec3 d = std::get<Vec3>(*p) - cam.pos;
        Value* vol = inst->field("volume");  // read live, so the game can change it while playing
        if (vol && std::holds_alternative<double>(*vol)) vc.volume = std::get<double>(*vol);
        vc.v->SetVolume(float(vc.volume * std::max(0.0, 1 - std::sqrt(d.dot(d)) / vc.range)));
        return false;
    });
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

static std::vector<SourceFile> dooFiles(const std::string& dir) {
    std::vector<SourceFile> files;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root / fs::u8path(dir), ec))
        if (e.path().extension() == ".doo") files.push_back({dir + "/" + e.path().filename().u8string(), readFile(e.path())});
    return files;
}

// SDK prefabs (BasicCharacterController, ParticleSystem...): objects written in Doo that every program gets.
static std::vector<SourceFile> prefabs() { return dooFiles("sdk/prefabs"); }

// Every .doo of a program folder (one object each); main.doo first, since its object is the root.
static std::vector<SourceFile> sources(const std::string& dir) {
    std::vector<SourceFile> files = dooFiles(dir);
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

static void load(Program& prog, const std::string& id, const std::string& dir, const fs::path& base, bool privileged) {
    prog = Program{};
    prog.id = id;
    prog.base = base;
    active = &prog;
    cam = {};  // each program starts with the default camera, and unpaused
    timeScale = 1;
    mode = -1;
    auto defs = compileAll(sources(dir), vm, privileged, prefabs());
    for (auto& d : defs) prog.objects[d->name] = d;
    spawnIn(prog, defs[0], nullptr);
}

static void unload(Program& prog) {  // destroy() on everything still alive, then drop the program
    auto scene = std::move(prog.scene);
    prog = Program{};
    for (auto& inst : scene) if (inst->alive) vm.call(*inst, "destroy");
}

static void stopAllSounds() {
    stopVoices([](const Voice&) { return true; });
}

static void bootFirmware() {
    stopAllSounds();
    game = Program{};
    load(firmware, "sistema", "firmware", root, true);
}

static void backToFirmware() {  // a game's sounds (music loops included) and pause end with it
    stopAllSounds();
    timeScale = 1;
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

static fs::path savePath(const std::string& id) { return root / "saves" / fs::u8path(id + ".sav"); }

static std::map<std::string, Value> readSave(const std::string& id) {
    std::error_code ec;
    return fs::exists(savePath(id), ec) ? decodeSave(readFile(savePath(id))) : std::map<std::string, Value>{};
}

static std::map<fs::path, Wav> sounds;  // never evicted: playing voices point into these buffers

static const Wav& sound(const std::string& file) {
    fs::path p = active->base / fs::u8path(file);
    if (auto it = sounds.find(p); it != sounds.end()) return it->second;
    try {
        return sounds[p] = parseWav(readFile(p));
    } catch (const std::exception& e) {
        throw std::runtime_error(file + ": " + e.what());
    }
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

static void drawPart(GLuint list, GLuint tex, Vec3 rgb) {  // lit color x texture (GL_MODULATE / the shader)
    glColor3d(rgb.x, rgb.y, rgb.z);
    if (shader) glUniform1i(shader->useTexture, tex ? 1 : 0);
    if (tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);  // crisp PS1-style texels in 3D
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    glCallList(list);
    if (tex) glDisable(GL_TEXTURE_2D);
}

static double opt(Args& a, size_t i, double fallback) { return i < a.size() ? argNum(a, i) : fallback; }

static void registerSdk() {
    for (int i = 0; i < NBUTTONS; i++) vm.constants["Button." + std::string(buttonNames[i])] = Value(double(i));
    for (int i = 0; i < NMESHES; i++) vm.constants["Mesh." + std::string(meshNames[i])] = Value(double(i));
    vm.constants["Light.Directional"] = Value(double(LIGHT_DIRECTIONAL));
    vm.constants["Light.Point"] = Value(double(LIGHT_POINT));
    vm.constants["Light.Spot"] = Value(double(LIGHT_SPOT));
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
    // 2D calls take an optional last `alpha` (0..1, default 1) for fades and translucent panels
    vm.addNative("render.rect", [](Instance&, Args& a) {  // (x, y, w, h, color, alpha)
        rect(argNum(a, 0), argNum(a, 1), argNum(a, 2), argNum(a, 3), argNum(a, 4), opt(a, 5, 1));
        return Value();
    });
    vm.addNative("render.gradient", [](Instance&, Args& a) {  // (x, y, w, h, top color, bottom color, top alpha, bottom alpha)
        double x = argNum(a, 0), y = argNum(a, 1), w = argNum(a, 2), h = argNum(a, 3);
        mode2D();
        glBegin(GL_QUADS);
        setColor(argNum(a, 4), opt(a, 6, 1));
        glVertex2d(x, y); glVertex2d(x + w, y);
        setColor(argNum(a, 5), opt(a, 7, 1));
        glVertex2d(x + w, y + h); glVertex2d(x, y + h);
        glEnd();
        return Value();
    });
    vm.addNative("render.text", [](Instance&, Args& a) {  // (x, y, text, size, color, alpha)
        text(argNum(a, 0), argNum(a, 1), str(a, 2), argNum(a, 3), argNum(a, 4), opt(a, 5, 1));
        return Value();
    });
    vm.addNative("render.text_width", [](Instance&, Args& a) { return Value(textWidth(str(a, 0), argNum(a, 1))); });
    vm.addNative("render.image", [](Instance&, Args& a) {  // (path, x, y, w, h, alpha) -> false if the file is missing
        const Image& img = image(active->base / fs::u8path(str(a, 0)));
        if (!img.tex) return Value(false);
        double x = argNum(a, 1), y = argNum(a, 2), w = argNum(a, 3), h = argNum(a, 4);
        mode2D();
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, img.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // smooth when UI icons are scaled
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        setColor(0xFFFFFF, opt(a, 5, 1));
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
    // Lights of this frame (declare them in update, every frame): max 8; with none, a default sun lights the scene.
    auto light = [](LightDef l, double intensity) {
        if (lights.size() >= 8) return Value(false);  // ponytail: 8 per frame, like GL's fixed lights; cull by distance if games need more
        l.color = l.color * intensity;
        lights.push_back(l);
        mode = -1;
        return Value(true);
    };
    vm.addNative("render.light_directional", [light](Instance&, Args& a) {  // (direction, color, intensity = 1)
        return light({LIGHT_DIRECTIONAL, {}, argVec(a, 0), rgb01(argNum(a, 1))}, opt(a, 2, 1));
    });
    vm.addNative("render.light_point", [light](Instance&, Args& a) {  // (position, color, range, intensity = 1)
        return light({LIGHT_POINT, argVec(a, 0), {}, rgb01(argNum(a, 1)), argNum(a, 2)}, opt(a, 3, 1));
    });
    vm.addNative("render.light_spot", [light](Instance&, Args& a) {  // (position, direction, color, range, angle = 45, intensity = 1)
        return light({LIGHT_SPOT, argVec(a, 0), argVec(a, 1), rgb01(argNum(a, 2)), argNum(a, 3), opt(a, 4, 45)}, opt(a, 5, 1));
    });
    vm.addNative("render.ambient", [](Instance&, Args& a) {  // (color) light everywhere, even in shadow
        ambient = rgb01(argNum(a, 0));
        mode = -1;
        return Value();
    });
    // ("padrao" | "ps1" | "name") for this frame's 3D; "name" = name.vert and/or name.frag in the game folder
    vm.addNative("render.set_shader", [](Instance&, Args& a) {
        if (!shadersOk) return Value(false);
        std::string name = str(a, 0);
        std::string key = name == "padrao" || name == "ps1" ? name : (active->base / fs::u8path(name)).u8string();
        auto it = shaders.find(key);
        if (it == shaders.end()) {
            auto stage = [&](const char* ext, const char* fallback) {
                std::error_code ec;
                fs::path p = active->base / fs::u8path(name + ext);
                return fs::exists(p, ec) ? readFile(p) : std::string(fallback);
            };
            std::string vert = stage(".vert", defaultVert), frag = stage(".frag", defaultFrag);
            if (vert == defaultVert && frag == defaultFrag) throw std::runtime_error("shader não encontrado: " + name + ".vert/.frag");
            it = shaders.emplace(key, buildShader(name, vert, frag)).first;
        }
        shader = &it->second;
        mode = -1;
        return Value(true);
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
        Vec3 tint = rgb01(argNum(a, 4));
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

    // audio.play(Hz, ms) = tone; audio.play("file.wav", volume = 1) = sample. Both return an id for audio.stop(id).
    vm.addNative("audio.play", [](Instance&, Args& a) {
        Voice* v;
        if (!a.empty() && std::holds_alternative<double>(a[0])) {
            auto t = makeTone(argNum(a, 0), argNum(a, 1));
            if ((v = playWav(*t, 1, false))) v->tone = t;
        } else {
            v = playWav(sound(str(a, 0)), a.size() > 1 ? argNum(a, 1) : 1, false);
        }
        return Value(double(v ? v->id : 0));
    });
    vm.addNative("audio.loop", [](Instance&, Args& a) {  // ("music.wav", volume = 1) -> id; plays until stopped
        Voice* v = playWav(sound(str(a, 0)), a.size() > 1 ? argNum(a, 1) : 1, true);
        return Value(double(v ? v->id : 0));
    });
    vm.addNative("audio.stop", [](Instance&, Args& a) {  // (id) stops that sound; () stops everything
        int id = a.empty() ? 0 : (int)argNum(a, 0);
        stopVoices([&](const Voice& v) { return !id || v.id == id; });
        return Value();
    });

    // use AudioSource: the object's `sound` plays at its `position` and fades out at `range` from the camera
    vm.components["AudioSource"] = {{"position", Value(Vec3{})}, {"sound", Value(std::string())}, {"volume", Value(1.0)},
                                    {"loop", Value(false)}, {"range", Value(20.0)}};
    vm.addNative("audio.source_play", [](Instance& self, Args&) {
        auto& uses = self.def->uses;
        if (std::find(uses.begin(), uses.end(), "AudioSource") == uses.end())
            throw std::runtime_error("audio.source_play() precisa de `use AudioSource` no objeto");
        auto num = [&](const char* f) {
            if (auto d = std::get_if<double>(self.field(f))) return *d;
            throw std::runtime_error(std::string("'") + f + "' precisa ser um número");
        };
        Voice* v = playWav(sound(toString(*self.field("sound"))), 0, truthy(*self.field("loop")));  // volume set by updateAudio
        if (!v) return Value(0.0);
        v->source = self.weak_from_this();
        v->volume = num("volume");
        v->range = std::max(0.001, num("range"));
        return Value(double(v->id));
    });
    vm.addNative("audio.source_stop", [](Instance& self, Args&) {
        stopVoices([&](const Voice& v) { return v.source.lock().get() == &self; });
        return Value();
    });

    vm.addNative("time.delta", [](Instance&, Args&) { return Value(dt * timeScale); });
    vm.addNative("time.unscaled_delta", [](Instance&, Args&) { return Value(dt); });  // ignores pause (for UI)
    vm.addNative("time.scale", [](Instance&, Args&) { return Value(timeScale); });
    vm.addNative("time.set_scale", [](Instance&, Args& a) {  // 0 = paused, 1 = normal, 0.5 = slow motion
        timeScale = std::max(0.0, argNum(a, 0));
        return Value();
    });
    auto now = [](const char* format) {  // local wall clock through strftime
        time_t t = time(nullptr);
        tm local;
        localtime_s(&local, &t);
        char buf[32];
        strftime(buf, sizeof buf, format, &local);
        return Value(std::string(buf));
    };
    vm.addNative("time.clock", [now](Instance&, Args&) { return now("%H:%M"); });
    vm.addNative("time.date", [now](Instance&, Args&) { return now("%d/%m"); });

    // store: installed games come from games/; saves live in saves/<id>.sav (the online store doesn't exist yet)
    vm.addNative("store.installed", [](Instance&, Args&) {
        auto list = std::make_shared<Array>();
        for (auto& id : installedGames()) list->push_back(Value(id));
        return Value(list);
    });
    vm.addNative("store.is_installed", [](Instance&, Args& a) { return Value(fs::exists(gameDir(str(a, 0)) / "main.doo")); });
    vm.addNative("store.title", [](Instance&, Args& a) {  // "titulo:" line of games/<id>/info.txt, else the id
        std::string id = str(a, 0);
        std::ifstream f(gameDir(id) / "info.txt");
        for (std::string line; std::getline(f, line);) {
            if (line.rfind("titulo:", 0) != 0) continue;
            size_t b = line.find_first_not_of(" \t", 7), e = line.find_last_not_of(" \t\r");
            if (b != std::string::npos) return Value(line.substr(b, e - b + 1));
        }
        return Value(id);
    });
    vm.addNative("store.save", [](Instance&, Args& a) {  // (key, value): the running program's own save data
        if (a.size() < 2) throw std::runtime_error("store.save espera (chave, valor)");
        auto kv = readSave(active->id);
        kv[str(a, 0)] = a[1];
        std::string text = encodeSave(kv);  // validates before touching the file
        fs::create_directories(root / "saves");
        std::ofstream(savePath(active->id), std::ios::binary) << text;
        return Value();
    });
    vm.addNative("store.load", [](Instance&, Args& a) {  // (key, default)
        auto kv = readSave(active->id);
        auto it = kv.find(str(a, 0));
        return it != kv.end() ? it->second : a.size() > 1 ? a[1] : Value();
    });
    vm.addNative("store.get_save_data", [](Instance&, Args& a) {  // a game's raw save ("" = none)
        std::string id = str(a, 0);
        if (active != &firmware && id != active->id) throw std::runtime_error("um jogo só pode ler o próprio save");
        std::error_code ec;
        return Value(fs::exists(savePath(id), ec) ? readFile(savePath(id)) : std::string());
    });

    // system: firmware only (enforced by the compiler)
    vm.addNative("system.launch", [](Instance&, Args& a) { pendingLaunch = str(a, 0); return Value(); });
    vm.addNative("system.set_volume", [](Instance&, Args& a) {  // 0..1, the whole console
        if (master) master->SetVolume((float)std::clamp(argNum(a, 0), 0.0, 1.0));
        return Value();
    });
    vm.addNative("system.delete_save", [](Instance&, Args& a) {
        std::error_code ec;
        fs::remove(savePath(str(a, 0)), ec);
        return Value();
    });
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
        physicsStep(vm, scene, dt * timeScale);
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
            load(game, id, "games/" + id, gameDir(id), false);
        }
    });
}

static int checkAll() {
    auto check = [](const std::string& dir, bool privileged) {
        try {
            compileAll(sources(dir), vm, privileged, prefabs());
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

    glEnable(GL_COLOR_MATERIAL);  // render.mesh color drives the lit material
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glEnable(GL_NORMALIZE);       // keep lighting right on scaled meshes (fixed-function fallback)
    buildMeshes();
    shadersOk = loadShaderApi();
    try {
        if (shadersOk) {
            shaders["padrao"] = buildShader("padrao", defaultVert, defaultFrag);
            shaders["ps1"] = buildShader("ps1", ps1Vert, ps1Frag);
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "%s\n", e.what());
        shadersOk = false;
    }
    if (!shadersOk) fprintf(stderr, "aviso: sem shaders no driver; iluminação por vértice e render.set_shader desligado\n");
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SelectObject(dc, CreateFontW(-64, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI"));
    fontBase = glGenLists(256);
    if (!wglUseFontOutlinesW(dc, 32, 224, fontBase + 32, 0, 0, WGL_FONT_POLYGONS, glyphs + 32))
        fprintf(stderr, "aviso: fonte não carregou, textos não vão aparecer\n");

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC (images) is COM
    initAudio();
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
        elapsed += dt;
        beginFrameLighting();
        frame();
        updateAudio();
        SwapBuffers(dc);
    }
}
