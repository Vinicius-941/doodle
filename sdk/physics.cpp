#include "physics.h"
#include <algorithm>
#include <cmath>

// Every collider is a "rounded box": an axis-aligned inner box (half extents) grown by a radius.
//   Box: half = size/2, r = 0   Sphere: half = 0, r = radius   Capsule (upright): half.y = height/2 - radius, r = radius
// ponytail: no rotation (boxes stay axis-aligned, capsules upright); add OBB/GJK when rotated colliders are needed
struct Body {
    std::shared_ptr<Instance> inst;
    Vec3 pos, half;
    double r = 0;
    bool rigid = false, collider = false, solid = false;  // solid = collider that isn't a trigger
};

void registerPhysics(VM& vm) {
    Value zero(Vec3{}), no(false);
    vm.components["BoxCollider"] = {{"position", zero}, {"size", Value(Vec3{1, 1, 1})}, {"trigger", no}};
    vm.components["SphereCollider"] = {{"position", zero}, {"radius", Value(0.5)}, {"trigger", no}};
    vm.components["CapsuleCollider"] = {{"position", zero}, {"radius", Value(0.5)}, {"height", Value(2.0)}, {"trigger", no}};
    vm.components["Rigidbody"] = {{"position", zero}, {"velocity", zero}, {"gravity", Value(20.0)}, {"grounded", no}};
    // heights: rows of 0..1 (nil = flat) spread over size.x × size.z around position, scaled by size.y
    vm.components["TerrainCollider"] = {{"position", zero}, {"size", Value(Vec3{10, 1, 10})}, {"heights", Value()}};
    vm.addNative("physics.terrain_height", [](Instance& self, std::vector<Value>& a) {  // (x, z) on the caller's terrain
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
    Value* pv = t.field("position");
    Value* sv = t.field("size");
    if (!pv || !sv || !std::holds_alternative<Vec3>(*pv) || !std::holds_alternative<Vec3>(*sv)) return false;
    Vec3 pos = std::get<Vec3>(*pv), size = std::get<Vec3>(*sv);
    double u = (x - pos.x) / size.x + 0.5, v = (z - pos.z) / size.z + 0.5;
    if (!(u >= 0 && u <= 1 && v >= 0 && v <= 1)) return false;
    auto rows = std::get_if<std::shared_ptr<Array>>(t.field("heights"));
    if (!rows || (*rows)->size() < 2) {  // no heightmap: a flat plane
        y = pos.y;
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
    double k = tx + tz <= 1 ? h(i, j) + (h(i, j + 1) - h(i, j)) * tx + (h(i + 1, j) - h(i, j)) * tz
                            : h(i + 1, j + 1) + (h(i + 1, j) - h(i + 1, j + 1)) * (1 - tx) + (h(i, j + 1) - h(i + 1, j + 1)) * (1 - tz);
    y = pos.y + k * size.y;
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
    b.solid = b.collider && !truthy(*i.field("trigger"));
    return true;
}

// Touching test between two rounded boxes; push = how far to move `a` so they stop overlapping.
static bool overlap(const Body& a, const Body& b, Vec3& push) {
    double d[3] = {a.pos.x - b.pos.x, a.pos.y - b.pos.y, a.pos.z - b.pos.z};
    double ext[3] = {a.half.x + b.half.x, a.half.y + b.half.y, a.half.z + b.half.z};
    double gap[3], out[3], dist2 = 0;
    int k = 0;  // axis of least penetration
    for (int i = 0; i < 3; i++) {
        gap[i] = std::fabs(d[i]) - ext[i];  // < 0: inner boxes overlap on this axis
        out[i] = std::max(gap[i], 0.0) * (d[i] < 0 ? -1 : 1);
        dist2 += out[i] * out[i];
        if (gap[i] > gap[k]) k = i;
    }
    double R = a.r + b.r;
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

    // ponytail: O(n²) pairs, and Rigidbody-vs-Rigidbody only reports contacts (no push); add a broadphase/impulses when needed
    for (auto& b : bodies) {
        if (!b.rigid) continue;
        Instance& i = *b.inst;
        Vec3 v = vec(i, "velocity");
        v.y -= number(i, "gravity") * dt;
        b.pos = b.pos + v * dt;
        bool grounded = false;
        for (auto& o : bodies) {
            Vec3 push;
            if (&o == &b || o.rigid || !o.solid || !b.solid || !overlap(b, o, push)) continue;
            b.pos = b.pos + push;
            double len = std::sqrt(push.dot(push));
            if (len == 0) continue;
            Vec3 n = push * (1 / len);
            double vn = v.dot(n);
            if (vn < 0) v = v - n * vn;       // stop moving into the surface
            if (n.y > 0.7) grounded = true;  // pushed up = standing on it
        }
        // ponytail: terrain pushes straight up (any slope is walkable); use the surface normal for slides if needed
        for (auto& t : terrains) {
            double ground, bottom = b.pos.y - b.half.y - b.r;
            if (!b.solid || !terrainHeight(*t, b.pos.x, b.pos.z, ground) || bottom >= ground) continue;
            b.pos.y += ground - bottom;
            if (v.y < 0) v.y = 0;
            grounded = true;
        }
        *i.field("position") = Value(b.pos);
        *i.field("velocity") = Value(v);
        *i.field("grounded") = Value(grounded);
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
        if (x->alive && y->alive) vm.call(*x, "on_collision", {Value(Ref{y})});
        if (x->alive && y->alive) vm.call(*y, "on_collision", {Value(Ref{x})});
    }
}
