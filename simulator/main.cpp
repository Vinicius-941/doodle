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
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include "assinatura.h"
#include "bytecode.h"
#include "compiler.h"
#include "net.h"
#include "obj.h"
#include "physics.h"
#include "save.h"
#include "wav.h"

namespace fs = std::filesystem;
using Args = std::vector<Value>;

// A tela do console: 320 x 180 (16:9), fixa. O jogo desenha nesses pixels e o simulador amplia sem
// suavizar, então o pixel do console é um pixel quadrado na tela do PC.
static const int W = 320, H = 180;

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
static const int JOGADORES = 2;
static bool keyDown[256], held[JOGADORES][NBUTTONS], was[JOGADORES][NBUTTONS];
static double stickX[JOGADORES], stickY[JOGADORES], lookX[JOGADORES], lookY[JOGADORES];  // -1..1 (y+ = cima/frente)
static double gatilhoL[JOGADORES], gatilhoR[JOGADORES];  // gatilhos analógicos, 0..1
static double vibraAte[JOGADORES];                       // quando a vibração de cada controle desliga (0 = parado)
static bool showFps = false, fpsLog = false;  // F3 (ou --fps): contador de quadros

// Orçamento do console: o simulador roda num PC que aguenta muito mais que o alvo da Fase 3, então ele
// mede o custo de cada quadro e avisa quando o jogo passa do que o hardware de referência entrega.
struct Orcamento {
    long long tris = 0, chamadas = 0;
};
static Orcamento quadro, pico;
static double avisou = -9;  // último aviso de estouro, para não repetir todo quadro
static long long texBytes = 0, audioBytes = 0;  // memória de mídia carregada: textura e som
static const long long MAX_TRIS = 30000;       // por quadro (1,8 M/s a 60 fps)
static const long long MAX_CHAMADAS = 600;     // desenhos por quadro
static const long long MAX_MIDIA = 8 << 20;    // 8 MB de textura + som juntos
static std::map<GLuint, long long> listaTris;  // triângulos de cada display list
static double fpsValue = 0, fpsWorst = 0;
static double dt = 0;         // real seconds since the last frame
static double timeScale = 1;  // games pause with time.set_scale(0); UI keeps going on time.unscaled_delta

static bool justPressed(int p, int b) { return held[p][b] && !was[p][b]; }

// ---------- render: 2D screen / 3D camera ----------

struct Camera { Vec3 pos{0, 3, 8}, target{0, 0, 0}; double fov = 60, largura = 0; };  // largura > 0: ortográfica
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
    if (cam.largura > 0) {  // ortográfica: sem perspectiva, `largura` unidades do mundo cabem na tela
        double w = cam.largura / 2, h = w * H / W;
        glOrtho(-w, w, -h, h, -500, 500);
    } else {
        gluPerspective(cam.fov, double(W) / H, 0.1, 500);
    }
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
    gluSphere(q, 0.5, 16, 10);
    glPopMatrix();
    glEndList();

    glNewList(meshBase + MESH_CYLINDER, GL_COMPILE);  // GLU builds along +Z; rotate it to stand on Y
    glPushMatrix();
    glRotated(-90, 1, 0, 0);
    glTranslated(0, 0, -1);
    gluCylinder(q, 0.5, 0.5, 2, 16, 1);
    gluQuadricOrientation(q, GLU_INSIDE);  // bottom cap faces down
    gluDisk(q, 0, 0.5, 16, 1);
    gluQuadricOrientation(q, GLU_OUTSIDE);
    glTranslated(0, 0, 2);
    gluDisk(q, 0, 0.5, 16, 1);
    glPopMatrix();
    glEndList();

    glNewList(meshBase + MESH_CAPSULE, GL_COMPILE);  // cylinder 1 tall + a sphere on each end
    glPushMatrix();
    glRotated(-90, 1, 0, 0);
    glTranslated(0, 0, -0.5);
    gluCylinder(q, 0.5, 0.5, 1, 16, 1);
    gluSphere(q, 0.5, 16, 10);
    glTranslated(0, 0, 1);
    gluSphere(q, 0.5, 16, 10);
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
    listaTris[meshBase + MESH_CUBE] = 12;
    listaTris[meshBase + MESH_SPHERE] = 16 * 10 * 2;
    listaTris[meshBase + MESH_CYLINDER] = 16 * 2 + 16 * 2;
    listaTris[meshBase + MESH_CAPSULE] = 16 * 2 + 16 * 10 * 4;
    listaTris[meshBase + MESH_PLANE] = 2;
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
    quadro.chamadas++;
    quadro.tris += 2;
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

// Fonte de pixel: numa tela de 320 x 180, letra vetorial escalada vira borrão. Cada tamanho vira um atlas
// desenhado pelo GDI **sem suavização**, então o traço cai no pixel inteiro e o texto fica legível.
struct Font {
    GLuint tex = 0;
    int texW = 0, texH = 0;
    struct Glyph { int x, y, w, h, advance; } g[256] = {};
};
static std::map<int, Font> fonts;

static const Font& font(int size) {
    size = std::clamp(size, 5, 96);
    if (auto it = fonts.find(size); it != fonts.end()) return it->second;
    Font f;
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT hf = CreateFontW(-size, 0, 0, 0, size <= 12 ? FW_NORMAL : FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY, DEFAULT_PITCH, L"Tahoma");
    SelectObject(dc, hf);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    int cell = tm.tmHeight + 1, cols = 16, wide = tm.tmMaxCharWidth + 1;
    for (f.texW = 1; f.texW < cols * wide; f.texW *= 2) {}
    for (f.texH = 1; f.texH < ((224 + cols - 1) / cols) * cell; f.texH *= 2) {}

    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof bi.bmiHeader, f.texW, -f.texH, 1, 32, BI_RGB};
    uint32_t* px = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&px, nullptr, 0);
    SelectObject(dc, bmp);
    RECT all = {0, 0, f.texW, f.texH};
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int ch = 32; ch < 256; ch++) {
        wchar_t w = (wchar_t)ch;
        SIZE sz = {};
        GetTextExtentPoint32W(dc, &w, 1, &sz);
        int i = ch - 32, x = (i % cols) * wide, y = (i / cols) * cell;
        TextOutW(dc, x, y, &w, 1);
        f.g[ch] = {x, y, std::min<int>(sz.cx, wide), tm.tmHeight, sz.cx};
    }
    GdiFlush();
    std::vector<uint8_t> alpha(size_t(f.texW) * f.texH);
    for (size_t i = 0; i < alpha.size(); i++) alpha[i] = uint8_t(px[i] & 0xFF);  // branco sobre preto = cobertura
    DeleteObject(bmp);
    DeleteObject(hf);
    DeleteDC(dc);

    glGenTextures(1, &f.tex);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, f.texW, f.texH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, alpha.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    return fonts.emplace(size, f).first->second;
}

static void textW(double x, double y, const std::wstring& s, double size, double c, double alpha = 1) {
    quadro.chamadas++;
    quadro.tris += (long long)s.size() * 2;
    mode2D();
    setColor(c, alpha);
    const Font& f = font((int)std::lround(size));
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, f.tex);
    double cx = std::floor(x), cy = std::floor(y);  // texto sempre em pixel inteiro
    glBegin(GL_QUADS);
    for (wchar_t ch : s) {
        const Font::Glyph& g = f.g[ch & 0xFF];
        if (g.w > 0) {
            double u0 = double(g.x) / f.texW, v0 = double(g.y) / f.texH;
            double u1 = double(g.x + g.w) / f.texW, v1 = double(g.y + g.h) / f.texH;
            glTexCoord2d(u0, v0); glVertex2d(cx, cy);
            glTexCoord2d(u1, v0); glVertex2d(cx + g.w, cy);
            glTexCoord2d(u1, v1); glVertex2d(cx + g.w, cy + g.h);
            glTexCoord2d(u0, v1); glVertex2d(cx, cy + g.h);
        }
        cx += g.advance;
    }
    glEnd();
    glDisable(GL_TEXTURE_2D);
}

static void text(double x, double y, const std::string& s, double size, double c, double alpha = 1) {
    textW(x, y, widen(s), size, c, alpha);
}

static double textWidth(const std::string& s, double size) {
    const Font& f = font((int)std::lround(size));
    double w = 0;
    for (wchar_t ch : widen(s)) w += f.g[ch & 0xFF].advance;
    return w;
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
    const GLenum GL_GENERATE_MIPMAP = 0x8191;  // GL 1.4: the driver builds the mip levels (no shimmer far away)
    glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
    texBytes += (long long)img.w * img.h * 4;
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
    bool loop = false;              // música: nunca é a voz sacrificada quando o console está cheio
};
static IXAudio2* xaudio;  // null when there is no audio device: games run muted
static IXAudio2MasteringVoice* master;  // its volume = system volume (firmware settings)
static std::vector<Voice> voices;
static int nextVoiceId = 1;

static void initAudio() {
    MFStartup(MF_VERSION, MFSTARTUP_LITE);  // decodificador do Windows para MP3 e companhia
    if (FAILED(XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR)) || FAILED(xaudio->CreateMasteringVoice(&master))) {
        xaudio = nullptr;
        fprintf(stderr, "aviso: nenhum dispositivo de áudio, os jogos vão rodar mudos\n");
    }
}

static const size_t MAX_VOZES = 24;  // o console mistura 24 sons ao mesmo tempo, como o PS1

static Voice* playWav(const Wav& w, double volume, bool loop) {  // nullptr when muted
    if (!xaudio) return nullptr;
    if (voices.size() >= MAX_VOZES) {  // cheio: a voz mais antiga que não está em loop cede o lugar
        auto velha = std::find_if(voices.begin(), voices.end(), [](const Voice& v) { return !v.loop && v.range == 0 && !v.source.lock(); });
        if (velha == voices.end()) velha = voices.begin();
        velha->v->DestroyVoice();
        voices.erase(velha);
    }
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
    voices.back().loop = loop;
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
// Lado da fonte em relação à câmera -> ganho em cada caixa, com potência constante (no meio, os dois lados
// tocam como antes). ponytail: só saída estéreo; 5.1 fica no mapa padrão do XAudio2.
static void pan(Voice& vc, Vec3 d) {
    XAUDIO2_VOICE_DETAILS fonte = {}, saida = {};
    master->GetVoiceDetails(&saida);
    vc.v->GetVoiceDetails(&fonte);
    if (saida.InputChannels != 2 || fonte.InputChannels > 2) return;
    Vec3 frente = cam.target - cam.pos, direita{-frente.z, 0, frente.x};
    double lado = 0, dl = std::sqrt(d.x * d.x + d.z * d.z), rl = std::sqrt(direita.dot(direita));
    if (dl > 1e-6 && rl > 1e-6) lado = std::clamp((d.x * direita.x + d.z * direita.z) / (dl * rl), -1.0, 1.0);
    const double quarto = 3.14159265358979323846 / 4;
    float l = float(std::cos((lado + 1) * quarto) * std::sqrt(2.0)), r = float(std::sin((lado + 1) * quarto) * std::sqrt(2.0));
    float mono[2] = {l, r}, estereo[4] = {l, 0, 0, r};  // linha = caixa de saída, coluna = canal da fonte
    vc.v->SetOutputMatrix(nullptr, fonte.InputChannels, 2, fonte.InputChannels == 1 ? mono : estereo);
}

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
        pan(vc, d);
        return false;
    });
}

// ---------- input ----------

// Analógico do XInput em -1..1, com zona morta radial: dentro dela o valor é 0, e fora dela recomeça do 0
// (sem isso o eixo pula de 0 para ~0.25 quando o dedo passa da zona morta).
static void axes(short rx, short ry, int deadzone, double& x, double& y) {
    double fx = rx / 32767.0, fy = ry / 32767.0, m = std::sqrt(fx * fx + fy * fy), d = deadzone / 32767.0;
    if (m <= d) {
        x = y = 0;
        return;
    }
    double k = std::min(1.0, (m - d) / (1 - d)) / m;
    x = fx * k;
    y = fy * k;
}

// Teclado no lugar do analógico: digital, então -1 ou 1.
static void keys(int left, int right, int down, int up, double& x, double& y) {
    if (keyDown[left] != keyDown[right]) x = keyDown[right] ? 1 : -1;
    if (keyDown[down] != keyDown[up]) y = keyDown[up] ? 1 : -1;
}

static void pollInput() {
    // ponytail: an empty XInput slot is slow to query, so a missing pad is re-checked every ~2s
    static bool padConnected[JOGADORES] = {true, true};
    static int padRetry[JOGADORES] = {};
    for (int p = 0; p < JOGADORES; p++) {
        XINPUT_STATE xs = {};
        bool pad = false;
        if (padConnected[p] || --padRetry[p] <= 0) {
            pad = padConnected[p] = XInputGetState(p, &xs) == ERROR_SUCCESS;
            padRetry[p] = 120;
        }
        const XINPUT_GAMEPAD& g = xs.Gamepad;
        for (int i = 0; i < NBUTTONS; i++) {
            was[p][i] = held[p][i];
            held[p][i] = (p == 0 && keyDown[keyMap[i]]) || (pad && (g.wButtons & padMap[i]));  // teclado = jogador 1
        }
        if (pad) {  // left stick doubles as the d-pad
            const int dz = 16000;
            held[p][BTN_UP] |= g.sThumbLY > dz;
            held[p][BTN_DOWN] |= g.sThumbLY < -dz;
            held[p][BTN_LEFT] |= g.sThumbLX < -dz;
            held[p][BTN_RIGHT] |= g.sThumbLX > dz;
        }
        auto gatilho = [](BYTE v) {  // abaixo do limiar do XInput vale 0, e depois recomeça do 0
            const double t = XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
            return v > t ? (v - t) / (255 - t) : 0.0;
        };
        gatilhoL[p] = pad ? gatilho(g.bLeftTrigger) : 0;
        gatilhoR[p] = pad ? gatilho(g.bRightTrigger) : 0;
        axes(pad ? g.sThumbLX : 0, pad ? g.sThumbLY : 0, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, stickX[p], stickY[p]);
        axes(pad ? g.sThumbRX : 0, pad ? g.sThumbRY : 0, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, lookX[p], lookY[p]);
    }
    keys(VK_LEFT, VK_RIGHT, VK_DOWN, VK_UP, stickX[0], stickY[0]);  // sem controle, as setas fazem o analógico esquerdo
    keys('J', 'L', 'K', 'I', lookX[0], lookY[0]);                   // ...e IJKL, o direito
    if (keyDown['E']) gatilhoL[0] = 1;                              // E e R, os gatilhos
    if (keyDown['R']) gatilhoR[0] = 1;
    for (int p = 0; p < JOGADORES; p++) {                           // vibração com hora para acabar
        if (vibraAte[p] > 0 && elapsed >= vibraAte[p]) {
            XINPUT_VIBRATION zero = {};
            XInputSetState(p, &zero);
            vibraAte[p] = 0;
        }
    }
}

// ---------- programs: firmware and games ----------

static std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("não foi possível abrir " + p.u8string());
    return {std::istreambuf_iterator<char>(f), {}};
}

static fs::path gameDir(const std::string& id) { return root / "games" / fs::u8path(id); }

// Um jogo é uma pasta com o código-fonte (main.doo) ou com o jogo compilado (jogo.doobc, o que a loja entrega).
static bool isGame(const fs::path& dir) {
    std::error_code ec;
    return fs::exists(dir / "main.doo", ec) || fs::exists(dir / "jogo.doobc", ec);
}

static std::vector<std::string> installedGames() {
    std::vector<std::string> ids;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(root / "games", ec))
        if (isGame(e.path()) && e.path().extension() != ".parcial")  // pula download pela metade
            ids.push_back(e.path().filename().u8string());
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

// Os objetos de um programa: com main.doo compila o fonte (quem está desenvolvendo); sem ele, lê jogo.doobc.
static std::vector<std::shared_ptr<ObjectDef>> program(const std::string& dir, bool privileged) {
    fs::path pasta = root / fs::u8path(dir);
    std::error_code ec;
    if (!fs::exists(pasta / "main.doo", ec) && fs::exists(pasta / "jogo.doobc", ec))
        return loadBytecode(readFile(pasta / "jogo.doobc"), vm, privileged, dir + "/jogo.doobc");
    return compileAll(sources(dir), vm, privileged, prefabs());
}

static void load(Program& prog, const std::string& id, const std::string& dir, const fs::path& base, bool privileged) {
    prog = Program{};
    prog.id = id;
    prog.base = base;
    active = &prog;
    vm.scene = &prog.scene;  // with e instance_* olham a cena de quem está rodando
    cam = {};  // each program starts with the default camera, and unpaused
    timeScale = 1;
    mode = -1;
    auto defs = program(dir, privileged);
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

static void pararVibracao() {
    for (int p = 0; p < JOGADORES; p++) {
        XINPUT_VIBRATION zero = {};
        XInputSetState(p, &zero);
        vibraAte[p] = 0;
    }
}

static void backToFirmware() {  // a game's sounds (music loops included), vibration and pause end with it
    pararVibracao();
    stopAllSounds();
    timeScale = 1;
    game = Program{};
    active = &firmware;
    vm.scene = &firmware.scene;
}

static void guarded(void (*fn)()) {
    try {
        fn();
    } catch (const std::exception& e) {
        crash = e.what();
        fprintf(stderr, "%s\n", crash.c_str());
    }
}

// ---------- loja: catálogo e download em segundo plano ----------
//
// A loja é um servidor estático: GET <url>/catalogo.txt lista os jogos, GET <url>/<id>/<arquivo> baixa cada
// arquivo. O catálogo é o mesmo formato dos saves (chave TAB valor), um bloco por jogo:
//     jogo<TAB>quadrado
//     titulo<TAB>Quadrado Andante
//     info<TAB>O primeiro teste do console
//     arquivo<TAB>main.doo<TAB>412
// Baixar bloqueia, então roda numa thread; a thread do quadro só olha o progresso.

struct StoreItem {
    std::string id, title, info;
    long long size = 0;
    std::vector<std::pair<std::string, long long>> files;
};

static std::string storeUrl = "http://localhost:8080";
static std::vector<StoreItem> catalog;
static std::string storeError;
static bool catalogReady = false;
static std::mutex storeMutex;  // protege catalog/storeError/catalogReady entre a thread do quadro e a da loja
static std::thread storeThread;
static std::atomic<bool> storeBusy{false};
static std::atomic<double> storeProgress{0};

// Nomes que viram caminho em disco ou pedaço de URL: só o que não escapa da pasta do jogo.
static void checkName(const std::string& s, bool slash) {
    bool ok = !s.empty() && s.size() <= 120 && s.find("..") == std::string::npos && s[0] != '/';
    for (char c : s)
        ok = ok && (isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || (slash && c == '/'));
    if (!ok) throw std::runtime_error("nome recusado pela loja: " + s);
}

static void storeRun(std::function<void()> job) {
    if (storeBusy.exchange(true)) return;  // um download por vez
    if (storeThread.joinable()) storeThread.join();
    {
        std::lock_guard<std::mutex> g(storeMutex);
        storeError.clear();
    }
    storeProgress = 0;
    storeThread = std::thread([job] {
        try {
            job();
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> g(storeMutex);
            storeError = e.what();
        }
        storeBusy = false;
    });
}

static void fetchCatalog() {
    std::string txt = net::get(storeUrl + "/catalogo.txt", 1 << 20);
    std::vector<StoreItem> items;
    std::istringstream in(txt);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        std::string key = line.substr(0, tab), rest = line.substr(tab + 1);
        if (key == "jogo") {
            checkName(rest, false);
            items.push_back({rest});
        } else if (items.empty()) {
            continue;
        } else if (key == "titulo") {
            items.back().title = rest;
        } else if (key == "info") {
            items.back().info = rest;
        } else if (key == "arquivo") {
            size_t t2 = rest.find('\t');
            std::string name = rest.substr(0, t2);
            checkName(name, true);
            long long bytes = t2 == std::string::npos ? 0 : atoll(rest.c_str() + t2 + 1);
            items.back().files.push_back({name, bytes});
            items.back().size += bytes;
        }
    }
    std::lock_guard<std::mutex> g(storeMutex);
    catalog = std::move(items);
    catalogReady = true;
}

static StoreItem catalogItem(const std::string& id) {
    std::lock_guard<std::mutex> g(storeMutex);
    for (auto& it : catalog)
        if (it.id == id) return it;
    throw std::runtime_error("jogo fora do catálogo: " + id);
}

// Baixa para <id>.parcial e só então troca pela pasta final: um download interrompido não vira jogo quebrado.
static void installGame(const std::string& id) {
    StoreItem item = catalogItem(id);
    fs::path tmp = root / "games" / fs::u8path(id + ".parcial");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    long long done = 0;
    for (auto& [name, bytes] : item.files) {
        std::string data = net::get(storeUrl + "/" + id + "/" + name, 64u << 20);
        fs::path out = tmp / fs::u8path(name);
        fs::create_directories(out.parent_path());
        std::ofstream(out, std::ios::binary).write(data.data(), data.size());
        done += (long long)data.size();
        storeProgress = item.size > 0 ? std::min(1.0, double(done) / item.size) : 0.5;
    }
    std::error_code chaveEc;
    fs::path chavePublica = root / "loja.pub";
    if (fs::exists(chavePublica, chaveEc)) {  // console com chave: pacote precisa vir assinado pela loja
        fs::path jogo = tmp / "jogo.doobc", sig = tmp / "jogo.sig";
        if (!fs::exists(jogo, chaveEc) || !fs::exists(sig, chaveEc)) {
            fs::remove_all(tmp, ec);
            throw std::runtime_error("este console só instala jogo compilado e assinado pela loja");
        }
        if (!assinatura::confere(readFile(jogo), readFile(sig), readFile(chavePublica))) {
            fs::remove_all(tmp, ec);
            throw std::runtime_error("a assinatura do jogo não confere: pacote adulterado ou de outra loja");
        }
    }
    if (!isGame(tmp)) {
        fs::remove_all(tmp, ec);
        throw std::runtime_error("pacote sem main.doo nem jogo.doobc");
    }
    std::ofstream(tmp / ".loja", std::ios::binary) << storeUrl << "\n";  // marca de origem: só isto pode desinstalar
    fs::remove_all(gameDir(id), ec);
    fs::rename(tmp, gameDir(id));
    storeProgress = 1;
}

// ---------- SDK ----------

static std::string str(Args& a, size_t i) {
    if (i >= a.size()) throw std::runtime_error("faltou o argumento " + std::to_string(i + 1));
    return toString(a[i]);
}

static int player(Args& a, size_t i) {  // argumento opcional: 1 ou 2 (padrão 1)
    int p = i < a.size() ? (int)argNum(a, i) : 1;
    if (p < 1 || p > JOGADORES) throw std::runtime_error("jogador inválido (use 1 ou 2)");
    return p - 1;
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

// Áudio que não é WAV (MP3, WMA, AAC...) passa pelo Media Foundation, que já vem no Windows, e vira PCM
// aqui na memória. ponytail: decodifica o arquivo inteiro na hora de tocar; música longa pede streaming.
static Wav decodeMedia(const fs::path& p) {
    struct Solta {  // COM: solta o que abriu, saindo por onde sair
        IUnknown* o = nullptr;
        ~Solta() { if (o) o->Release(); }
    };
    IMFSourceReader* leitor = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(p.wstring().c_str(), nullptr, &leitor)))
        throw std::runtime_error("o Windows não sabe ler este áudio");
    Solta fechaLeitor{leitor};
    IMFMediaType* pedido = nullptr;
    if (FAILED(MFCreateMediaType(&pedido))) throw std::runtime_error("sem memória para o áudio");
    Solta fechaPedido{pedido};
    pedido->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    pedido->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (FAILED(leitor->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, pedido)))
        throw std::runtime_error("formato de áudio não suportado");

    IMFMediaType* saida = nullptr;
    if (FAILED(leitor->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &saida)))
        throw std::runtime_error("não descobri o formato do áudio");
    Solta fechaSaida{saida};
    WAVEFORMATEX* fmt = nullptr;
    UINT32 tam = 0;
    if (FAILED(MFCreateWaveFormatExFromMFMediaType(saida, &fmt, &tam)))
        throw std::runtime_error("não descobri o formato do áudio");
    Wav w;
    w.format.assign((uint8_t*)fmt, (uint8_t*)fmt + tam);
    CoTaskMemFree(fmt);

    for (;;) {
        DWORD flags = 0;
        IMFSample* amostra = nullptr;
        if (FAILED(leitor->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &amostra)))
            throw std::runtime_error("erro lendo o áudio");
        if (!amostra) {
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            continue;  // lacuna no fluxo
        }
        Solta fechaAmostra{amostra};
        IMFMediaBuffer* buffer = nullptr;
        if (FAILED(amostra->ConvertToContiguousBuffer(&buffer))) throw std::runtime_error("erro lendo o áudio");
        Solta fechaBuffer{buffer};
        BYTE* dados = nullptr;
        DWORD bytes = 0;
        if (FAILED(buffer->Lock(&dados, nullptr, &bytes))) throw std::runtime_error("erro lendo o áudio");
        w.data.insert(w.data.end(), dados, dados + bytes);
        buffer->Unlock();
    }
    if (w.data.empty()) throw std::runtime_error("áudio vazio");
    return w;
}

static const Wav& sound(const std::string& file) {
    fs::path p = active->base / fs::u8path(file);
    if (auto it = sounds.find(p); it != sounds.end()) return it->second;
    try {
        std::string ext = p.extension().u8string();
        for (char& c : ext) c = (char)tolower((unsigned char)c);
        const Wav& w = sounds[p] = ext == ".wav" ? parseWav(readFile(p)) : decodeMedia(p);
        audioBytes += (long long)w.data.size();
        return w;
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
    std::vector<ObjPart> fonte;  // os triângulos crus: usados para misturar dois quadros de animação
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
        listaTris[list] = (long long)part.tris.size() / 3;
        m.parts.push_back({list, part.texture.empty() ? 0 : texture(dir / fs::u8path(part.texture)), part.color});
    }
    m.fonte = std::move(parts);
    return models[p] = std::move(m);
}

// Terrain meshes, built once per (heights, size, tiling). The cache holds the heights array so its
// address can't be reused by another one while cached.
struct TerrainKey {
    const Array* rows;
    const Array* mask;  // com máscara, cada vértice leva a opacidade da segunda textura
    double sx, sy, sz, tiling;
    bool operator<(const TerrainKey& o) const {
        return std::tie(rows, mask, sx, sy, sz, tiling) < std::tie(o.rows, o.mask, o.sx, o.sy, o.sz, o.tiling);
    }
};
struct TerrainMesh { std::shared_ptr<Array> rows, mask; GLuint list; };  // os arrays ficam vivos enquanto no cache
static std::map<TerrainKey, TerrainMesh> terrainMeshes;

// Linhas de números (heights, máscara) -> grade de doubles; vazia se não houver pelo menos 2 x 2.
static std::vector<std::vector<double>> grid(const std::shared_ptr<Array>& rows, const char* nome) {
    std::vector<std::vector<double>> g;
    if (!rows || rows->size() < 2) return g;
    for (auto& r : *rows) {
        auto cols = std::get_if<std::shared_ptr<Array>>(&r);
        if (!cols || (*cols)->size() < 2) throw std::runtime_error(std::string(nome) + ": cada linha precisa de 2+ números");
        g.emplace_back();
        for (auto& v : **cols) g.back().push_back(argNum({v}, 0));
        if (g.back().size() != g[0].size()) throw std::runtime_error(std::string(nome) + ": todas as linhas precisam do mesmo tamanho");
    }
    return g;
}

static GLuint terrainMesh(const std::shared_ptr<Array>& rows, Vec3 size, double tiling, const std::shared_ptr<Array>& mask = nullptr) {
    TerrainKey key{rows.get(), mask.get(), size.x, size.y, size.z, tiling};
    if (auto it = terrainMeshes.find(key); it != terrainMeshes.end()) return it->second.list;
    auto m = grid(mask, "máscara");
    auto opacidade = [&](double u, double v) {  // a máscara pode ter outra resolução: amostra com interpolação
        double fx = u * (m[0].size() - 1), fz = v * (m.size() - 1);
        size_t j = std::min(size_t(fx), m[0].size() - 2), i = std::min(size_t(fz), m.size() - 2);
        double tx = fx - j, tz = fz - i;
        return (m[i][j] * (1 - tx) + m[i][j + 1] * tx) * (1 - tz) + (m[i + 1][j] * (1 - tx) + m[i + 1][j + 1] * tx) * tz;
    };
    auto h = grid(rows, "heights");
    if (h.empty()) h.assign(33, std::vector<double>(33, 0.0));  // flat plane: still a 33 x 33 grid, so per-vertex (PS1) lighting shows on it
    size_t H = h.size(), W = h[0].size();
    auto P = [&](size_t i, size_t j) {  // local position of grid point (row i, column j)
        return Vec3{(double(j) / (W - 1) - 0.5) * size.x, h[i][j] * size.y, (double(i) / (H - 1) - 0.5) * size.z};
    };
    auto vertex = [&](size_t i, size_t j) {
        Vec3 dx = P(i, std::min(j + 1, W - 1)) - P(i, j ? j - 1 : 0), dz = P(std::min(i + 1, H - 1), j) - P(i ? i - 1 : 0, j);
        Vec3 n{dz.y * dx.z - dz.z * dx.y, dz.z * dx.x - dz.x * dx.z, dz.x * dx.y - dz.y * dx.x};  // dz x dx: up
        Vec3 p = P(i, j);
        glNormal3d(n.x, n.y, n.z);
        if (!m.empty()) glColor4d(1, 1, 1, opacidade(double(j) / (W - 1), double(i) / (H - 1)));
        glTexCoord2d(double(j) / (W - 1) * tiling, double(i) / (H - 1) * tiling);
        glVertex3d(p.x, p.y, p.z);
    };
    GLuint list = glGenLists(1);
    glNewList(list, GL_COMPILE);
    glBegin(GL_TRIANGLES);
    for (size_t i = 0; i + 1 < H; i++) {
        for (size_t j = 0; j + 1 < W; j++) {  // same split as the physics: (00, 10, 01) and (10, 11, 01)
            vertex(i, j); vertex(i + 1, j); vertex(i, j + 1);
            vertex(i, j + 1); vertex(i + 1, j); vertex(i + 1, j + 1);
        }
    }
    glEnd();
    glEndList();
    listaTris[list] = (long long)(H - 1) * (W - 1) * 2;
    terrainMeshes[key] = {rows, mask, list};
    return list;
}

static void useTexture(GLuint tex) {
    if (shader) glUniform1i(shader->useTexture, tex ? 1 : 0);
    if (!tex) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);  // crisp texels up close, no shimmer far
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

static void drawPart(GLuint list, GLuint tex, Vec3 rgb, double alpha = 1) {  // lit color x texture (GL_MODULATE / the shader)
    quadro.chamadas++;
    quadro.tris += listaTris[list];
    glColor4d(rgb.x, rgb.y, rgb.z, alpha);
    useTexture(tex);
    glCallList(list);
    if (tex) glDisable(GL_TEXTURE_2D);
}

// Dois quadros-chave da mesma malha, misturados vértice a vértice (0 = o primeiro, 1 = o segundo).
// É como o PS1 animava personagem: sem osso, os quadros já vêm deformados e o console interpola.
static void drawMix(const Model& A, const Model& B, double t, Vec3 tint, GLuint tex) {
    if (A.fonte.size() != B.fonte.size()) throw std::runtime_error("os quadros precisam ter a mesma malha");
    for (size_t i = 0; i < A.fonte.size(); i++) {
        const std::vector<ObjVertex>&va = A.fonte[i].tris, &vb = B.fonte[i].tris;
        if (va.size() != vb.size()) throw std::runtime_error("os quadros precisam ter a mesma malha");
        quadro.chamadas++;
        quadro.tris += (long long)va.size() / 3;
        Vec3 c = A.fonte[i].color;
        glColor3d(c.x * tint.x, c.y * tint.y, c.z * tint.z);
        GLuint t2 = tex ? tex : A.parts[i].tex;
        useTexture(t2);
        glBegin(GL_TRIANGLES);
        for (size_t k = 0; k < va.size(); k++) {
            Vec3 p = va[k].pos + (vb[k].pos - va[k].pos) * t, n = va[k].normal + (vb[k].normal - va[k].normal) * t;
            glNormal3d(n.x, n.y, n.z);  // GL_NORMALIZE cuida do tamanho depois da mistura
            glTexCoord2d(va[k].u, va[k].v);
            glVertex3d(p.x, p.y, p.z);
        }
        glEnd();
        if (t2) glDisable(GL_TEXTURE_2D);
    }
}

static double opt(Args& a, size_t i, double fallback) { return i < a.size() ? argNum(a, i) : fallback; }

static void registerSdk() {
    auto lower = [](std::string s) {  // btn_a, mesh_cube: constantes em minúsculo, como as da GML
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    for (int i = 0; i < NBUTTONS; i++) vm.constants["btn_" + lower(buttonNames[i])] = Value(double(i));
    for (int i = 0; i < NMESHES; i++) vm.constants["mesh_" + lower(meshNames[i])] = Value(double(i));
    vm.constants["lt_directional"] = Value(double(LIGHT_DIRECTIONAL));
    vm.constants["lt_point"] = Value(double(LIGHT_POINT));
    vm.constants["lt_spot"] = Value(double(LIGHT_SPOT));
    registerPhysics(vm);

    vm.addNative("instance_create", [](Instance&, Args& a) {  // (Object, position?) -> ref to the new instance
        std::string name = str(a, 0);
        auto it = active->objects.find(name);
        if (it == active->objects.end()) throw std::runtime_error("o objeto '" + name + "' não existe neste programa");
        Vec3 pos = a.size() > 1 ? argVec(a, 1) : Vec3{};
        return Value(Ref{spawnIn(*active, it->second, a.size() > 1 ? &pos : nullptr)});
    });

    vm.addNative("make_color_rgb", [](Instance&, Args& a) {
        auto c = [&](size_t i) { return std::clamp((int)argNum(a, i), 0, 255); };
        return Value(double(c(0) << 16 | c(1) << 8 | c(2)));
    });
    vm.addNative("draw_clear", [](Instance&, Args& a) {  // fills the screen (scissored to it) and resets depth
        int c = (int)argNum(a, 0);
        glClearColor((c >> 16 & 255) / 255.f, (c >> 8 & 255) / 255.f, (c & 255) / 255.f, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        return Value();
    });
    // 2D calls take an optional last `alpha` (0..1, default 1) for fades and translucent panels
    vm.addNative("draw_rectangle", [](Instance&, Args& a) {  // (x, y, w, h, color, alpha)
        rect(argNum(a, 0), argNum(a, 1), argNum(a, 2), argNum(a, 3), argNum(a, 4), opt(a, 5, 1));
        return Value();
    });
    vm.addNative("draw_gradient", [](Instance&, Args& a) {  // (x, y, w, h, top color, bottom color, top alpha, bottom alpha)
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
    vm.addNative("draw_text", [](Instance&, Args& a) {  // (x, y, text, size, color, alpha)
        text(argNum(a, 0), argNum(a, 1), str(a, 2), argNum(a, 3), argNum(a, 4), opt(a, 5, 1));
        return Value();
    });
    vm.addNative("string_width", [](Instance&, Args& a) { return Value(textWidth(str(a, 0), argNum(a, 1))); });
    vm.addNative("draw_sprite", [](Instance&, Args& a) {  // (path, x, y, w, h, alpha) -> false if the file is missing
        const Image& img = image(active->base / fs::u8path(str(a, 0)));
        if (!img.tex) return Value(false);
        double x = argNum(a, 1), y = argNum(a, 2), w = argNum(a, 3), h = argNum(a, 4);
        mode2D();
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, img.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);  // smooth when UI icons are scaled
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
    vm.addNative("camera_set", [](Instance&, Args& a) {  // (position, target, fov = 60)
        cam = {argVec(a, 0), argVec(a, 1), a.size() > 2 ? argNum(a, 2) : 60};
        mode = -1;
        return Value();
    });
    vm.addNative("camera_set_ortho", [](Instance&, Args& a) {  // (position, target, largura): sem perspectiva
        double largura = argNum(a, 2);
        if (largura <= 0) throw std::runtime_error("camera_set_ortho: a largura precisa ser maior que 0");
        cam = {argVec(a, 0), argVec(a, 1), 60, largura};
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
    vm.addNative("light_directional", [light](Instance&, Args& a) {  // (direction, color, intensity = 1)
        return light({LIGHT_DIRECTIONAL, {}, argVec(a, 0), rgb01(argNum(a, 1))}, opt(a, 2, 1));
    });
    vm.addNative("light_point", [light](Instance&, Args& a) {  // (position, color, range, intensity = 1)
        return light({LIGHT_POINT, argVec(a, 0), {}, rgb01(argNum(a, 1)), argNum(a, 2)}, opt(a, 3, 1));
    });
    vm.addNative("light_spot", [light](Instance&, Args& a) {  // (position, direction, color, range, angle = 45, intensity = 1)
        return light({LIGHT_SPOT, argVec(a, 0), argVec(a, 1), rgb01(argNum(a, 2)), argNum(a, 3), opt(a, 4, 45)}, opt(a, 5, 1));
    });
    vm.addNative("light_ambient", [](Instance&, Args& a) {  // (color) light everywhere, even in shadow
        ambient = rgb01(argNum(a, 0));
        mode = -1;
        return Value();
    });
    // ("padrao" | "ps1" | "name") for this frame's 3D; "name" = name.vert and/or name.frag in the game folder
    vm.addNative("shader_set", [](Instance&, Args& a) {
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

    // Terrain (see the Terrain prefab): heights = rows of 0..1, or nil for a flat plane
    // (position, size, heights, color, texture = "", tiling = 1, texture2 = "", mascara = nil, tiling2 = tiling)
    vm.addNative("draw_terrain", [](Instance&, Args& a) {
        Vec3 p = argVec(a, 0), s = argVec(a, 1);
        auto rows = a.size() > 2 ? std::get_if<std::shared_ptr<Array>>(&a[2]) : nullptr;
        double tiling = opt(a, 5, 1);
        GLuint list = terrainMesh(rows ? *rows : nullptr, s, tiling);
        GLuint tex = a.size() > 4 && !str(a, 4).empty() ? texture(active->base / fs::u8path(str(a, 4))) : 0;
        mode3D();
        glPushMatrix();
        glTranslated(p.x, p.y, p.z);
        drawPart(list, tex, rgb01(argNum(a, 3)));
        auto mask = a.size() > 7 ? std::get_if<std::shared_ptr<Array>>(&a[7]) : nullptr;
        if (a.size() > 6 && !str(a, 6).empty() && mask && (*mask)->size() >= 2) {
            // segunda passada: a mesma malha com a segunda textura, opaca onde a máscara é clara
            GLuint tex2 = texture(active->base / fs::u8path(str(a, 6)));
            GLuint list2 = terrainMesh(rows ? *rows : nullptr, s, opt(a, 8, tiling), *mask);
            glDepthFunc(GL_LEQUAL);  // mesmos vértices, mesma profundidade: tem que passar no empate
            drawPart(list2, tex2, {1, 1, 1});
            glDepthFunc(GL_LESS);
        }
        glPopMatrix();
        return Value();
    });
    vm.addNative("heightmap_read", [](Instance&, Args& a) {  // ("relevo.png") -> linhas de 0..1 (brilho), no máximo 65 x 65
        int w = 0, h = 0;
        std::vector<uint32_t> px;
        if (!decodeImage(active->base / fs::u8path(str(a, 0)), w, h, px) || w < 2 || h < 2)
            throw std::runtime_error("heightmap não encontrado ou inválido: " + str(a, 0));
        int step = std::max(1, (std::max(w, h) - 1 + 63) / 64);  // grade de no máximo 65 x 65: 8 mil triângulos
        auto rows = std::make_shared<Array>();
        for (int y = 0; y < h; y += step) {
            auto row = std::make_shared<Array>();
            for (int x = 0; x < w; x += step) {
                uint32_t c = px[size_t(y) * w + x];
                row->push_back(Value(((c >> 16 & 255) + (c >> 8 & 255) + (c & 255)) / 765.0));
            }
            rows->push_back(Value(row));
        }
        return Value(rows);
    });

    // (Mesh.X or "model.obj", position, rotation in degrees, scale, color, texture = "")
    // color tints: 0xFFFFFF keeps a model's own colors/textures. texture (optional) replaces the material's.
    // (quadroA.obj, quadroB.obj, mistura 0..1, posição, rotação, escala, cor, textura = "")
    vm.addNative("draw_mesh_mix", [](Instance&, Args& a) {
        const Model& A = model(active->base / fs::u8path(str(a, 0)));
        const Model& B = model(active->base / fs::u8path(str(a, 1)));
        double t = std::clamp(argNum(a, 2), 0.0, 1.0);
        Vec3 p = argVec(a, 3), r = argVec(a, 4), s;
        if (a.size() > 5 && std::holds_alternative<double>(a[5])) {
            double k = argNum(a, 5);
            s = {k, k, k};
        } else {
            s = argVec(a, 5);
        }
        GLuint tex = a.size() > 7 && !str(a, 7).empty() ? texture(active->base / fs::u8path(str(a, 7))) : 0;
        mode3D();
        glPushMatrix();
        glTranslated(p.x, p.y, p.z);
        glRotated(r.y, 0, 1, 0);  // Unity order: Z, then X, then Y
        glRotated(r.x, 1, 0, 0);
        glRotated(r.z, 0, 0, 1);
        glScaled(s.x, s.y, s.z);
        drawMix(A, B, t, rgb01(argNum(a, 6)), tex);
        glPopMatrix();
        return Value();
    });
    // (malha, posição, rotação, escala, cor, textura = "", alpha = 1)
    vm.addNative("draw_mesh", [](Instance&, Args& a) {
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
        double alpha = std::clamp(opt(a, 6, 1), 0.0, 1.0);
        mode3D();
        glPushMatrix();
        glTranslated(p.x, p.y, p.z);
        glRotated(r.y, 0, 1, 0);  // Unity order: Z, then X, then Y
        glRotated(r.x, 1, 0, 0);
        glRotated(r.z, 0, 0, 1);
        glScaled(s.x, s.y, s.z);
        if (mdl) {
            for (auto& part : mdl->parts)
                drawPart(part.list, tex ? tex : part.tex, {tint.x * part.color.x, tint.y * part.color.y, tint.z * part.color.z}, alpha);
        } else {
            drawPart(meshBase + m, tex, tint, alpha);
        }
        glPopMatrix();
        return Value();
    });

    // O jogador é sempre o último argumento e vale 1 por padrão; o teclado é sempre o jogador 1.
    vm.addNative("button_check", [](Instance&, Args& a) { return Value(held[player(a, 1)][button(a)]); });
    vm.addNative("button_check_pressed", [](Instance&, Args& a) { return Value(justPressed(player(a, 1), button(a))); });
    vm.addNative("stick_x", [](Instance&, Args& a) { return Value(stickX[player(a, 0)]); });  // analógico esquerdo, -1..1
    vm.addNative("stick_y", [](Instance&, Args& a) { return Value(stickY[player(a, 0)]); });  // 1 = para cima/para frente
    vm.addNative("look_x", [](Instance&, Args& a) { return Value(lookX[player(a, 0)]); });    // analógico direito
    vm.addNative("look_y", [](Instance&, Args& a) { return Value(lookY[player(a, 0)]); });
    vm.addNative("trigger_l", [](Instance&, Args& a) { return Value(gatilhoL[player(a, 0)]); });  // 0..1
    vm.addNative("trigger_r", [](Instance&, Args& a) { return Value(gatilhoR[player(a, 0)]); });
    vm.addNative("pad_vibrate", [](Instance&, Args& a) {  // (motor esquerdo 0..1, motor direito 0..1, segundos, jogador = 1)
        int p = player(a, 3);
        double segundos = argNum(a, 2);
        XINPUT_VIBRATION v = {};
        if (segundos > 0) {
            v.wLeftMotorSpeed = WORD(std::clamp(argNum(a, 0), 0.0, 1.0) * 65535);
            v.wRightMotorSpeed = WORD(std::clamp(argNum(a, 1), 0.0, 1.0) * 65535);
        }
        XInputSetState(p, &v);
        vibraAte[p] = segundos > 0 ? elapsed + segundos : 0;
        return Value();
    });
    vm.addNative("pad_connected", [](Instance&, Args& a) {
        XINPUT_STATE xs = {};
        return Value(XInputGetState(player(a, 0), &xs) == ERROR_SUCCESS);
    });

    // Devolvem um id para audio_stop_sound(id).
    vm.addNative("audio_play_sound", [](Instance&, Args& a) {  // ("tiro.wav", volume = 1)
        Voice* v = playWav(sound(str(a, 0)), a.size() > 1 ? argNum(a, 1) : 1, false);
        return Value(double(v ? v->id : 0));
    });
    vm.addNative("audio_play_tone", [](Instance&, Args& a) {  // (Hz, ms): bipe sintetizado, sem arquivo
        auto t = makeTone(argNum(a, 0), argNum(a, 1));
        Voice* v = playWav(*t, 1, false);
        if (v) v->tone = t;
        return Value(double(v ? v->id : 0));
    });
    vm.addNative("audio_play_loop", [](Instance&, Args& a) {  // ("music.wav", volume = 1) -> id; plays until stopped
        Voice* v = playWav(sound(str(a, 0)), a.size() > 1 ? argNum(a, 1) : 1, true);
        return Value(double(v ? v->id : 0));
    });
    vm.addNative("audio_stop_sound", [](Instance&, Args& a) {  // (id) stops that sound; () stops everything
        int id = a.empty() ? 0 : (int)argNum(a, 0);
        stopVoices([&](const Voice& v) { return !id || v.id == id; });
        return Value();
    });

    // use AudioSource: the object's `sound` plays at its `position` and fades out at `range` from the camera
    vm.components["AudioSource"] = {{"position", Value(Vec3{})}, {"sound", Value(std::string())}, {"volume", Value(1.0)},
                                    {"loop", Value(false)}, {"range", Value(20.0)}};
    vm.addNative("audio_source_play", [](Instance& self, Args&) {
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
    vm.addNative("audio_source_stop", [](Instance& self, Args&) {
        stopVoices([&](const Voice& v) { return v.source.lock().get() == &self; });
        return Value();
    });

    vm.addNative("delta_time", [](Instance&, Args&) { return Value(dt * timeScale); });
    vm.addNative("delta_time_real", [](Instance&, Args&) { return Value(dt); });  // ignores pause (for UI)
    vm.addNative("time_scale", [](Instance&, Args&) { return Value(timeScale); });
    vm.addNative("time_set_scale", [](Instance&, Args& a) {  // 0 = paused, 1 = normal, 0.5 = slow motion
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
    vm.addNative("clock_time", [now](Instance&, Args&) { return now("%H:%M"); });
    vm.addNative("clock_date", [now](Instance&, Args&) { return now("%d/%m"); });

    // store: installed games come from games/; saves live in saves/<id>.sav (the online store doesn't exist yet)
    vm.addNative("game_list", [](Instance&, Args&) {
        auto list = std::make_shared<Array>();
        for (auto& id : installedGames()) list->push_back(Value(id));
        return Value(list);
    });
    vm.addNative("game_installed", [](Instance&, Args& a) { return Value(isGame(gameDir(str(a, 0)))); });

    // loja online (só o firmware): o catálogo e os downloads rodam na thread da loja
    vm.addNative("store_set_url", [](Instance&, Args& a) {
        storeUrl = str(a, 0);
        return Value();
    });
    vm.addNative("store_refresh", [](Instance&, Args&) {
        storeRun(fetchCatalog);
        return Value();
    });
    vm.addNative("store_install", [](Instance&, Args& a) {
        std::string id = str(a, 0);
        checkName(id, false);
        storeRun([id] { installGame(id); });
        return Value();
    });
    vm.addNative("store_uninstall", [](Instance&, Args& a) {
        std::string id = str(a, 0);
        checkName(id, false);
        if (!fs::exists(gameDir(id) / ".loja")) throw std::runtime_error("só a loja desinstala o que ela instalou");
        std::error_code ec;
        fs::remove_all(gameDir(id), ec);
        return Value();
    });
    vm.addNative("store_can_uninstall", [](Instance&, Args& a) {  // só o que veio da loja, nunca um jogo seu
        return Value(fs::exists(gameDir(str(a, 0)) / ".loja"));
    });
    vm.addNative("store_busy", [](Instance&, Args&) { return Value(storeBusy.load()); });
    vm.addNative("store_progress", [](Instance&, Args&) { return Value(storeProgress.load()); });
    vm.addNative("store_ready", [](Instance&, Args&) {
        std::lock_guard<std::mutex> g(storeMutex);
        return Value(catalogReady);
    });
    vm.addNative("store_error", [](Instance&, Args&) {
        std::lock_guard<std::mutex> g(storeMutex);
        return Value(storeError);
    });
    vm.addNative("store_available", [](Instance&, Args&) {
        auto list = std::make_shared<Array>();
        std::lock_guard<std::mutex> g(storeMutex);
        for (auto& it : catalog) list->push_back(Value(it.id));
        return Value(list);
    });
    vm.addNative("store_title", [](Instance&, Args& a) { return Value(catalogItem(str(a, 0)).title); });
    vm.addNative("store_info", [](Instance&, Args& a) { return Value(catalogItem(str(a, 0)).info); });
    vm.addNative("store_size", [](Instance&, Args& a) { return Value((double)catalogItem(str(a, 0)).size); });
    vm.addNative("game_title", [](Instance&, Args& a) {  // "titulo:" line of games/<id>/info.txt, else the id
        std::string id = str(a, 0);
        std::ifstream f(gameDir(id) / "info.txt");
        for (std::string line; std::getline(f, line);) {
            if (line.rfind("titulo:", 0) != 0) continue;
            size_t b = line.find_first_not_of(" \t", 7), e = line.find_last_not_of(" \t\r");
            if (b != std::string::npos) return Value(line.substr(b, e - b + 1));
        }
        return Value(id);
    });
    vm.addNative("save_set", [](Instance&, Args& a) {  // (key, value): the running program's own save data
        if (a.size() < 2) throw std::runtime_error("store.save espera (chave, valor)");
        auto kv = readSave(active->id);
        kv[str(a, 0)] = a[1];
        std::string text = encodeSave(kv);  // validates before touching the file
        fs::create_directories(root / "saves");
        std::ofstream(savePath(active->id), std::ios::binary) << text;
        return Value();
    });
    vm.addNative("save_get", [](Instance&, Args& a) {  // (key, default)
        auto kv = readSave(active->id);
        auto it = kv.find(str(a, 0));
        return it != kv.end() ? it->second : a.size() > 1 ? a[1] : Value();
    });
    vm.addNative("game_save_data", [](Instance&, Args& a) {  // a game's raw save ("" = none)
        std::string id = str(a, 0);
        if (active != &firmware && id != active->id) throw std::runtime_error("um jogo só pode ler o próprio save");
        std::error_code ec;
        return Value(fs::exists(savePath(id), ec) ? readFile(savePath(id)) : std::string());
    });

    // system: firmware only (enforced by the compiler)
    vm.addNative("system_launch", [](Instance&, Args& a) { pendingLaunch = str(a, 0); return Value(); });
    vm.addNative("system_volume", [](Instance&, Args& a) {  // 0..1, the whole console
        if (master) master->SetVolume((float)std::clamp(argNum(a, 0), 0.0, 1.0));
        return Value();
    });
    vm.addNative("system_delete_save", [](Instance&, Args& a) {
        std::error_code ec;
        fs::remove(savePath(str(a, 0)), ec);
        return Value();
    });
}

// ---------- apresentação: o quadro do console vai para a tela do PC ----------

// Copia os W x H pixels desenhados e desenha ampliado, em número inteiro de vezes (2x, 3x...) e sem
// suavizar: é o que faz a limitação de resolução aparecer em vez de virar só um sistema de coordenadas.
static void present(const RECT& rc) {
    static GLuint tela = 0;
    if (!tela) {
        glGenTextures(1, &tela);
        glBindTexture(GL_TEXTURE_2D, tela);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, W, H, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);  // GL 1.1: sem repetir na borda
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    }
    glBindTexture(GL_TEXTURE_2D, tela);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, W, H);

    int escala = std::min(rc.right / W, rc.bottom / H);
    int vw = escala > 0 ? W * escala : std::min<int>(rc.right, rc.bottom * W / H);
    int vh = escala > 0 ? H * escala : std::max(1, vw * H / W);
    int vx = (rc.right - vw) / 2, vy = (rc.bottom - vh) / 2;

    if (shadersOk) glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_LIGHTING);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glViewport(0, 0, rc.right, rc.bottom);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);  // tarjas pretas em volta
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, rc.right, 0, rc.bottom, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor4d(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2d(0, 0); glVertex2i(vx, vy);
    glTexCoord2d(1, 0); glVertex2i(vx + vw, vy);
    glTexCoord2d(1, 1); glVertex2i(vx + vw, vy + vh);
    glTexCoord2d(0, 1); glVertex2i(vx, vy + vh);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    mode = -1;  // o próximo quadro refaz projeção e estado do zero
}

// ---------- main loop ----------

static void frame() {
    if (!crash.empty()) {
        rect(0, 0, W, H, 0x301010);
        text(10, 8, "Erro", 16, 0xFF6060);
        std::wstring msg = widen(crash);
        for (size_t i = 0; i * 48 < msg.size() && i < 11; i++) textW(10, 30 + i * 11.0, msg.substr(i * 48, 48), 9, 0xFFFFFF);
        text(10, H - 16, "HOME (Esc) para voltar", 9, 0xAAAAAA);
        if (justPressed(0, BTN_HOME)) {
            crash.clear();
            if (active == &game) backToFirmware(); else guarded(bootFirmware);  // a crashed firmware reboots from disk
        }
        return;
    }
    if (active == &game && justPressed(0, BTN_HOME)) {  // HOME always belongs to the system
        guarded([] { unload(game); });
        backToFirmware();
        return;
    }
    guarded([] {
        // Frame lifecycle: update() on everyone -> physics (+ on_collision) -> draw() -> destroy() on the dead.
        // Indexed loops: instances spawned during the frame join in right away.
        auto& scene = active->scene;
        if (scene.empty()) return;
        if (timeScale > 0) tickAlarms(vm, scene);  // pausa (escala 0) segura os alarmes também
        for (size_t i = 0; i < scene.size(); i++) if (scene[i]->alive) vm.call(*scene[i], "step");
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
            program(dir, privileged);
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

// ---------- modo console: tela cheia sem borda ----------

static bool fullscreen = false;

// ponytail: tela cheia sem borda (nao troca a resolucao do monitor); o letterbox 4:3 do loop cuida do resto
static void setFullscreen(HWND hwnd, bool on) {
    static RECT saved = {};
    static bool savedOk = false;
    if (on == fullscreen) return;
    fullscreen = on;
    if (on) {
        savedOk = GetWindowRect(hwnd, &saved);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        UINT flags = SWP_FRAMECHANGED | SWP_NOZORDER | (savedOk ? 0 : SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(hwnd, nullptr, saved.left, saved.top, saved.right - saved.left, saved.bottom - saved.top, flags);
    }
    ShowCursor(!on);  // so nas transicoes, para o contador do ShowCursor nao desandar
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && !(l & 0x40000000)) { setFullscreen(hwnd, !fullscreen); return 0; }  // Alt+Enter
        break;
    case WM_KEYDOWN:
        if (!(l & 0x40000000)) {  // ignora a repeticao da tecla presa
            if (w == VK_F11) { setFullscreen(hwnd, !fullscreen); return 0; }
            if (w == VK_F3) { showFps = !showFps; return 0; }
        }
        keyDown[w & 0xFF] = true;
        return 0;
    case WM_KEYUP: keyDown[w & 0xFF] = false; return 0;
    case WM_KILLFOCUS: memset(keyDown, 0, sizeof keyDown); return 0;
    case WM_GETMINMAXINFO: {  // a janela nunca fica menor que a tela do console
        RECT r = {0, 0, W, H};
        AdjustWindowRect(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE);
        ((MINMAXINFO*)l)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_CLOSE: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}

int main(int argc, char** argv) {
    bool check = false, console = false;
    std::string buildDir, buildOut, cripto, cripto1, cripto2, cripto3;
    root = fs::u8path(DOODLE_ROOT);
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--check") check = true;
        else if (arg == "--build" && i + 2 < argc) { buildDir = argv[++i]; buildOut = argv[++i]; }  // jogo -> .doobc
        else if (arg == "--keygen" && i + 2 < argc) { cripto = "keygen"; cripto1 = argv[++i]; cripto2 = argv[++i]; }
        else if (arg == "--sign" && i + 3 < argc) {  // (arquivo, chave privada, saída .sig)
            cripto = "sign";
            cripto1 = argv[++i];
            cripto2 = argv[++i];
            cripto3 = argv[++i];
        } else if (arg == "--verify" && i + 3 < argc) {  // (arquivo, .sig, chave pública)
            cripto = "verify";
            cripto1 = argv[++i];
            cripto2 = argv[++i];
            cripto3 = argv[++i];
        }
        else if (arg == "--console") console = true;          // liga direto em tela cheia
        else if (arg == "--fps") showFps = fpsLog = true;     // contador na tela e uma linha por segundo no console
        else root = arg;
    }
    if (!cripto.empty()) {
        try {
            auto grava = [](const std::string& caminho, const std::string& dados) {
                std::ofstream out(fs::u8path(caminho), std::ios::binary);
                out.write(dados.data(), (std::streamsize)dados.size());
                if (!out) throw std::runtime_error("não consegui escrever " + caminho);
            };
            if (cripto == "keygen") {
                std::string priv, pub;
                assinatura::gerar(priv, pub);
                grava(cripto1, priv);
                grava(cripto2, pub);
                printf("ok    chaves em %s (guarde fora do repositório) e %s\n", cripto1.c_str(), cripto2.c_str());
            } else if (cripto == "sign") {
                grava(cripto3, assinatura::assinar(readFile(fs::u8path(cripto1)), readFile(fs::u8path(cripto2))));
                printf("ok    %s assinado em %s\n", cripto1.c_str(), cripto3.c_str());
            } else {
                bool ok = assinatura::confere(readFile(fs::u8path(cripto1)), readFile(fs::u8path(cripto2)),
                                              readFile(fs::u8path(cripto3)));
                printf("%s %s\n", ok ? "ok   " : "ERRO ", ok ? "a assinatura confere" : "a assinatura NÃO confere");
                return ok ? 0 : 1;
            }
            return 0;
        } catch (const std::exception& e) {
            printf("ERRO  %s\n", e.what());
            return 1;
        }
    }
    registerSdk();
    if (check) return checkAll();
    if (!buildDir.empty()) {
        try {
            std::string bc = saveBytecode(compileAll(sources(buildDir), vm, false, prefabs()), vm);
            std::ofstream out(fs::u8path(buildOut), std::ios::binary);
            out.write(bc.data(), (std::streamsize)bc.size());
            if (!out) throw std::runtime_error("não consegui escrever " + buildOut);
            printf("ok    %s -> %s (%zu bytes)\n", buildDir.c_str(), buildOut.c_str(), bc.size());
            return 0;
        } catch (const std::exception& e) {
            printf("ERRO  %s\n", e.what());
            return 1;
        }
    }

    SetProcessDPIAware();
    WNDCLASSW wc = {};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"Doodle";
    RegisterClassW(&wc);
    RECT r = {0, 0, W * 3, H * 3};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"Doodle", L"Doodle Simulator", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (console) setFullscreen(hwnd, true);
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
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // WIC (images) is COM
    initAudio();
    guarded(bootFirmware);
    auto last = std::chrono::steady_clock::now();
    for (;;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                pararVibracao();  // controle não fica tremendo depois que o console fecha
                if (storeThread.joinable()) storeThread.detach();  // download em curso morre com o processo
                return 0;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (IsIconic(hwnd)) { Sleep(50); continue; }
        auto now = std::chrono::steady_clock::now();
        dt = std::min(std::chrono::duration<double>(now - last).count(), 0.1);
        last = now;

        static double fpsAcc = 0, fpsPeak = 0;   // media de 1 s e o pior quadro do intervalo
        static int fpsFrames = 0;
        fpsAcc += dt;
        fpsFrames++;
        fpsPeak = std::max(fpsPeak, dt);
        if (fpsAcc >= 1) {
            fpsValue = fpsFrames / fpsAcc;
            fpsWorst = fpsPeak;
            fpsAcc = fpsPeak = 0;
            fpsFrames = 0;
            if (fpsLog) { printf("%.1f fps   pior quadro %.1f ms\n", fpsValue, fpsWorst * 1000); fflush(stdout); }
        }

        RECT rc;
        GetClientRect(hwnd, &rc);
        glViewport(0, 0, W, H);  // o console desenha sempre no canto, em 320 x 180
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glScissor(0, 0, W, H);
        glEnable(GL_SCISSOR_TEST);
        pollInput();
        elapsed += dt;
        quadro = {};  // o orçamento conta um quadro por vez
        beginFrameLighting();
        frame();
        long long midia = texBytes + audioBytes;
        bool estourou = quadro.tris > MAX_TRIS || quadro.chamadas > MAX_CHAMADAS || midia > MAX_MIDIA;
        pico.tris = std::max(pico.tris, quadro.tris);
        pico.chamadas = std::max(pico.chamadas, quadro.chamadas);
        if (estourou && elapsed - avisou > 3) {  // avisa, não bloqueia: no PC dá, no alvo da Fase 3 pode não dar
            avisou = elapsed;
            fprintf(stderr, "orçamento do console estourado: %lld triângulos (máx %lld), %lld desenhos (máx %lld), %.1f MB de mídia (máx %lld MB)\n",
                    quadro.tris, MAX_TRIS, quadro.chamadas, MAX_CHAMADAS, midia / 1048576.0, MAX_MIDIA >> 20);
        }
        if (showFps) {
            char buf[80];
            snprintf(buf, sizeof buf, "%.0f fps  %.1f ms  pior %.1f", fpsValue, fpsValue > 0 ? 1000 / fpsValue : 0.0,
                     fpsWorst * 1000);
            rect(W - 116, 3, 113, 23, 0x000000, 0.45);
            text(W - 113, 4, buf, 9, 0x9BE86B);
            snprintf(buf, sizeof buf, "%lld tri  %lld des  %.1f MB", quadro.tris, quadro.chamadas, midia / 1048576.0);
            text(W - 113, 14, buf, 9, estourou ? 0xFF6B6B : 0x9BE86B);
        }
        present(rc);
        updateAudio();
        SwapBuffers(dc);
    }
}
