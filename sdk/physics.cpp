#include "physics.h"
#include <algorithm>
#include <cmath>

// Every collider is a "rounded box": an inner box (half extents) grown by a radius.
//   Box: half = size/2, r = 0   Sphere: half = 0, r = radius   Capsule (upright): half.y = height/2 - radius, r = radius
// A BoxCollider pode girar (`rotation`, em graus, na mesma ordem do draw_mesh). O teste de contato roda no
// referencial da caixa girada, e o outro corpo entra lá como a caixa alinhada que o contém.
// ponytail: um corpo que não é a referência vira a caixa que o envolve — exato em face (rampa, parede
// girada), um pouco gordo em quina e entre duas caixas giradas; GJK/SAT se isso aparecer em jogo.

struct Mat3 {
    double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 operator*(const Vec3& v) const {
        return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
                m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
    }
    Mat3 operator*(const Mat3& o) const {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
        return r;
    }
    Mat3 transposta() const {
        Mat3 r;
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i];
        return r;
    }
};

// Graus -> rotação local para mundo, na ordem da Unity (e do draw_mesh): Z, depois X, depois Y.
static Mat3 rotacao(Vec3 graus) {
    const double k = 3.14159265358979323846 / 180;
    double cx = std::cos(graus.x * k), sx = std::sin(graus.x * k), cy = std::cos(graus.y * k), sy = std::sin(graus.y * k),
           cz = std::cos(graus.z * k), sz = std::sin(graus.z * k);
    Mat3 x, y, z;
    x.m[1][1] = cx; x.m[1][2] = -sx; x.m[2][1] = sx; x.m[2][2] = cx;
    y.m[0][0] = cy; y.m[0][2] = sy; y.m[2][0] = -sy; y.m[2][2] = cy;
    z.m[0][0] = cz; z.m[0][1] = -sz; z.m[1][0] = sz; z.m[1][1] = cz;
    return y * x * z;
}

struct Body {
    std::shared_ptr<Instance> inst;
    Vec3 pos, half, vel;
    double r = 0;
    bool rigid = false, collider = false, solid = false;  // solid = collider that isn't a trigger
    bool grounded = false;
    bool girado = false;
    double massa = 1;
    Mat3 rot;  // local -> mundo (identidade sem girar)
};

static bool uses(const Instance& i, const char* component);
static bool toBody(const std::shared_ptr<Instance>& s, Body& b);

void registerPhysics(VM& vm) {
    Value zero(Vec3{}), no(false);
    vm.components["BoxCollider"] = {{"position", zero}, {"size", Value(Vec3{1, 1, 1})}, {"rotation", zero}, {"trigger", no}};
    vm.components["SphereCollider"] = {{"position", zero}, {"radius", Value(0.5)}, {"trigger", no}};
    vm.components["CapsuleCollider"] = {{"position", zero}, {"radius", Value(0.5)}, {"height", Value(2.0)},
                                        {"rotation", zero}, {"trigger", no}};
    // slope_limit: rampa até esse ângulo é chão (fica parado nela); mais íngreme que isso, escorrega
    // mass: quem tem mais massa cede menos no empurrão; 0 = não sai do lugar (plataforma, porta)
    vm.components["Rigidbody"] = {{"position", zero}, {"velocity", zero}, {"gravity", Value(20.0)}, {"grounded", no},
                                  {"slope_limit", Value(45.0)}, {"mass", Value(1.0)}};
    // heights: rows of 0..1 (nil = flat) spread over size.x × size.z around position, scaled by size.y
    vm.components["TerrainCollider"] = {{"position", zero}, {"size", Value(Vec3{10, 1, 10})}, {"heights", Value()}};
    // (x, y, z) -> altura do chão sólido mais alto abaixo desse ponto, ou nil se não houver nada embaixo.
    // Serve para sombra, para largar objeto no chão e para saber se há piso adiante.
    vm.addNative("ground_below", [&vm](Instance&, std::vector<Value>& a) {
        double x = argNum(a, 0), y = argNum(a, 1), z = argNum(a, 2);
        bool achou = false;
        double melhor = 0;
        auto candidato = [&](double topo) {
            if (topo > y + 1e-4 || (achou && topo <= melhor)) return;
            melhor = topo;
            achou = true;
        };
        if (vm.scene)
            for (auto& s : *vm.scene) {
                if (!s->alive) continue;
                if (uses(*s, "TerrainCollider")) {
                    double h;
                    if (terrainHeight(*s, x, z, h)) candidato(h);
                }
                Body b;
                if (!toBody(s, b) || b.rigid || !b.solid) continue;  // só o cenário parado faz chão
                if (std::fabs(x - b.pos.x) <= b.half.x + b.r && std::fabs(z - b.pos.z) <= b.half.z + b.r)
                    candidato(b.pos.y + b.half.y + b.r);
            }
        return achou ? Value(melhor) : Value();
    });
    vm.addNative("terrain_height", [](Instance& self, std::vector<Value>& a) {  // (x, z) on the caller's terrain
        double y;
        return terrainHeight(self, argNum(a, 0), argNum(a, 1), y) ? Value(y) : Value();  // nil outside it
    });
}

static bool uses(const Instance& i, const char* component) {
    auto& u = i.def->uses;
    return std::find(u.begin(), u.end(), component) != u.end();
}

// Component fields always exist; these only check that the game kept the right type in them.
static double number(Instance& i, const char* name) {
    if (auto n = std::get_if<double>(i.field(name))) return *n;
    throw std::runtime_error(i.def->file + ": '" + name + "' precisa ser um número");
}

static double number(const Value& v, const std::string& file) {
    if (auto n = std::get_if<double>(&v)) return *n;
    throw std::runtime_error(file + ": 'heights' precisa ser uma lista de linhas de números");
}

bool terrainHeight(Instance& t, double x, double z, double& y) {
    Vec3 n;
    return terrainSurface(t, x, z, y, n);
}

bool terrainSurface(Instance& t, double x, double z, double& y, Vec3& normal) {
    Value* pv = t.field("position");
    Value* sv = t.field("size");
    if (!pv || !sv || !std::holds_alternative<Vec3>(*pv) || !std::holds_alternative<Vec3>(*sv)) return false;
    Vec3 pos = std::get<Vec3>(*pv), size = std::get<Vec3>(*sv);
    double u = (x - pos.x) / size.x + 0.5, v = (z - pos.z) / size.z + 0.5;
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) return false;
    auto rows = std::get_if<std::shared_ptr<Array>>(t.field("heights"));
    if (!rows || (*rows)->size() < 2) {  // no heightmap: a flat plane
        y = pos.y;
        normal = {0, 1, 0};
        return true;
    }
    auto row = [&](size_t i) -> const Array& {
        auto r = std::get_if<std::shared_ptr<Array>>(&(**rows)[i]);
        if (!r || (*r)->size() < 2) throw std::runtime_error(t.def->file + ": cada linha de 'heights' precisa de 2+ números");
        return **r;
    };
    size_t rowsN = (*rows)->size(), cols = row(0).size();
    double fx = u * (cols - 1), fz = v * (rowsN - 1);
    size_t j = std::min(size_t(fx), cols - 2), i = std::min(size_t(fz), rowsN - 2);
    double tx = fx - j, tz = fz - i;
    auto h = [&](size_t r, size_t c) { return number(row(r).at(c), t.def->file); };
    // same split as the rendered mesh: triangles (00, 10, 01) and (10, 11, 01)
    bool primeiro = tx + tz <= 1;
    double k = primeiro ? h(i, j) + (h(i, j + 1) - h(i, j)) * tx + (h(i + 1, j) - h(i, j)) * tz
                        : h(i + 1, j + 1) + (h(i + 1, j) - h(i + 1, j + 1)) * (1 - tx) + (h(i, j + 1) - h(i + 1, j + 1)) * (1 - tz);
    y = pos.y + k * size.y;
    // inclinação do triângulo (a mesma do desenho) -> normal da superfície
    double dkx = primeiro ? h(i, j + 1) - h(i, j) : h(i + 1, j + 1) - h(i + 1, j);
    double dkz = primeiro ? h(i + 1, j) - h(i, j) : h(i + 1, j + 1) - h(i, j + 1);
    double dydx = dkx * size.y * (cols - 1) / size.x, dydz = dkz * size.y * (rowsN - 1) / size.z;
    double len = std::sqrt(dydx * dydx + 1 + dydz * dydz);
    normal = {-dydx / len, 1 / len, -dydz / len};
    return true;
}

static Vec3 vec(Instance& i, const char* name) {
    if (auto v = std::get_if<Vec3>(i.field(name))) return *v;
    throw std::runtime_error(i.def->file + ": '" + name + "' precisa ser um vec3");
}

static bool toBody(const std::shared_ptr<Instance>& s, Body& b) {
    Instance& i = *s;
    b = Body{s};
    b.rigid = uses(i, "Rigidbody");
    b.collider = uses(i, "BoxCollider") || uses(i, "SphereCollider") || uses(i, "CapsuleCollider");
    if (!b.rigid && !b.collider) return false;
    b.pos = vec(i, "position");
    if (uses(i, "BoxCollider")) {
        b.half = vec(i, "size") * 0.5;
    } else if (uses(i, "SphereCollider")) {
        b.r = number(i, "radius");
    } else if (uses(i, "CapsuleCollider")) {
        b.r = number(i, "radius");
        b.half.y = std::max(0.0, number(i, "height") / 2 - b.r);
    }
    if (uses(i, "BoxCollider") || uses(i, "CapsuleCollider")) {  // caixa e cápsula giram; esfera é redonda
        Vec3 g = vec(i, "rotation");
        b.girado = g.x != 0 || g.y != 0 || g.z != 0;
        if (b.girado) b.rot = rotacao(g);
    }
    if (b.rigid) b.massa = std::max(0.0, number(i, "mass"));
    b.solid = b.collider && !truthy(*i.field("trigger"));
    return true;
}

// Meia-medida alinhada da caixa `half` depois de girada por M: a caixa alinhada que a contém.
static Vec3 envolve(const Mat3& M, const Vec3& half) {
    return {std::fabs(M.m[0][0]) * half.x + std::fabs(M.m[0][1]) * half.y + std::fabs(M.m[0][2]) * half.z,
            std::fabs(M.m[1][0]) * half.x + std::fabs(M.m[1][1]) * half.y + std::fabs(M.m[1][2]) * half.z,
            std::fabs(M.m[2][0]) * half.x + std::fabs(M.m[2][1]) * half.y + std::fabs(M.m[2][2]) * half.z};
}

static bool overlapAlinhado(const Vec3& dv, const Vec3& ha, const Vec3& hb, double R, Vec3& push);

// Touching test between two rounded boxes; push = how far to move `a` so they stop overlapping.
static bool overlap(const Body& a, const Body& b, Vec3& push) {
    if (!a.girado && !b.girado) return overlapAlinhado(a.pos - b.pos, a.half, b.half, a.r + b.r, push);
    bool refB = b.girado;  // referencial: a caixa girada (b, se as duas estiverem)
    const Mat3& ref = refB ? b.rot : a.rot;
    Mat3 volta = ref.transposta();
    Vec3 ha = refB ? envolve(volta * a.rot, a.half) : a.half;  // rot de quem não gira é a identidade
    Vec3 hb = refB ? b.half : envolve(volta * b.rot, b.half);
    Vec3 local;
    if (!overlapAlinhado(volta * (a.pos - b.pos), ha, hb, a.r + b.r, local)) return false;
    push = ref * local;
    return true;
}

static bool overlapAlinhado(const Vec3& dv, const Vec3& ha, const Vec3& hb, double R, Vec3& push) {
    double d[3] = {dv.x, dv.y, dv.z};
    double ext[3] = {ha.x + hb.x, ha.y + hb.y, ha.z + hb.z};
    double gap[3], out[3], dist2 = 0;
    int k = 0;  // axis of least penetration
    for (int i = 0; i < 3; i++) {
        gap[i] = std::fabs(d[i]) - ext[i];  // < 0: inner boxes overlap on this axis
        out[i] = std::max(gap[i], 0.0) * (d[i] < 0 ? -1 : 1);
        dist2 += out[i] * out[i];
        if (gap[i] > gap[k]) k = i;
    }
    if (gap[k] < 0) {  // inner boxes overlap: push out along the shallowest axis
        double p[3] = {};
        p[k] = (d[k] < 0 ? -1 : 1) * (R - gap[k]);
        push = {p[0], p[1], p[2]};
        return true;
    }
    double dist = std::sqrt(dist2);
    if (dist > R + 1e-4) return false;
    push = dist > 0 ? Vec3{out[0], out[1], out[2]} * (std::max(0.0, R - dist) / dist) : Vec3{};
    return true;
}

void physicsStep(VM& vm, const std::vector<std::shared_ptr<Instance>>& scene, double dt) {
    std::vector<Body> bodies;
    std::vector<std::shared_ptr<Instance>> terrains;
    for (auto& s : scene) {
        Body b;
        if (s->alive && toBody(s, b)) bodies.push_back(b);
        if (s->alive && uses(*s, "TerrainCollider")) terrains.push_back(s);
    }

    // ponytail: O(n²) pairs; ganha uma broadphase quando o número de corpos justificar
    for (auto& b : bodies) {
        if (!b.rigid) continue;
        Instance& i = *b.inst;
        b.vel = vec(i, "velocity");
        b.vel.y -= number(i, "gravity") * dt;
        b.pos = b.pos + b.vel * dt;
        double chao = std::cos(std::clamp(number(i, "slope_limit"), 0.0, 89.0) * 3.14159265358979323846 / 180);
        // Encostou numa superfície de normal n, afundando `fundo`: se n é chão (rampa até slope_limit), sobe
        // na vertical e fica parado nela; se é íngreme ou parede, sai pela normal e escorrega.
        auto apoia = [&](Vec3 n, double fundo) {
            if (n.y >= chao) {
                b.pos.y += fundo / n.y;
                if (b.vel.y < 0) b.vel.y = 0;
                b.grounded = true;
            } else {
                b.pos = b.pos + n * fundo;
                double vn = b.vel.dot(n);
                if (vn < 0) b.vel = b.vel - n * vn;  // stop moving into the surface
            }
        };
        for (auto& o : bodies) {  // contra o cenário parado
            Vec3 push;
            if (&o == &b || o.rigid || !o.solid || !b.solid || !overlap(b, o, push)) continue;
            double len = std::sqrt(push.dot(push));
            if (len > 0) apoia(push * (1 / len), len);
        }
        for (auto& t : terrains) {
            Vec3 n;
            double ground, bottom = b.pos.y - b.half.y - b.r;
            if (!b.solid || !terrainSurface(*t, b.pos.x, b.pos.z, ground, n) || bottom >= ground) continue;
            apoia(n, (ground - bottom) * n.y);  // o vão vertical, medido na direção da normal
        }
    }

    // Dois Rigidbody se empurram, cada um cedendo na medida da sua massa: o mais pesado sai menos do lugar,
    // e massa 0 não sai. Uma passada só por quadro, então pilha alta acomoda em alguns quadros.
    for (size_t a = 0; a < bodies.size(); a++) {
        for (size_t c = a + 1; c < bodies.size(); c++) {
            Body &x = bodies[a], &y = bodies[c];
            Vec3 push;
            if (!x.rigid || !y.rigid || !x.solid || !y.solid || !overlap(x, y, push)) continue;
            double px = x.massa > 0 ? 1 / x.massa : 0, py = y.massa > 0 ? 1 / y.massa : 0;
            if (px + py == 0) continue;  // dois imóveis: ninguém cede
            double fx = px / (px + py), fy = py / (px + py);
            x.pos = x.pos + push * fx;
            y.pos = y.pos - push * fy;
            double len = std::sqrt(push.dot(push));
            if (len == 0) continue;
            Vec3 n = push * (1 / len);
            double rel = (x.vel - y.vel).dot(n);
            if (rel < 0) {  // estão se aproximando: cancela essa parte da velocidade, na medida da massa
                x.vel = x.vel - n * (rel * fx);
                y.vel = y.vel + n * (rel * fy);
            }
            if (n.y > 0.7) x.grounded = true;   // x ficou por cima de y
            if (n.y < -0.7) y.grounded = true;
        }
    }

    for (auto& b : bodies) {
        if (!b.rigid) continue;
        *b.inst->field("position") = Value(b.pos);
        *b.inst->field("velocity") = Value(b.vel);
        *b.inst->field("grounded") = Value(b.grounded);
    }

    // on_collision(other) every frame while touching (like GameMaker's collision event)
    std::vector<std::pair<std::shared_ptr<Instance>, std::shared_ptr<Instance>>> hits;
    for (size_t a = 0; a < bodies.size(); a++) {
        Body& x = bodies[a];
        if (!x.rigid || !x.collider) continue;
        for (size_t c = 0; c < bodies.size(); c++) {
            Body& y = bodies[c];
            Vec3 push;
            if (a == c || !y.collider || (y.rigid && c < a) || !overlap(x, y, push)) continue;
            hits.push_back({x.inst, y.inst});
        }
        for (auto& t : terrains) {
            double ground;
            if (terrainHeight(*t, x.pos.x, x.pos.z, ground) && x.pos.y - x.half.y - x.r <= ground + 1e-3) hits.push_back({x.inst, t});
        }
    }
    for (auto& [x, y] : hits) {
        if (x->alive && y->alive) vm.call(*x, "collision", {Value(Ref{y})});
        if (x->alive && y->alive) vm.call(*y, "collision", {Value(Ref{x})});
    }
}
