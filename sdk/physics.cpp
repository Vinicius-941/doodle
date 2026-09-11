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
    for (auto& s : scene) {
        Body b;
        if (s->alive && toBody(s, b)) bodies.push_back(b);
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
        *i.field("position") = Value(b.pos);
        *i.field("velocity") = Value(v);
        *i.field("grounded") = Value(grounded);
    }

    // on_collision(other) every frame while touching (like GameMaker's collision event)
    std::vector<std::pair<Body*, Body*>> hits;
    for (size_t a = 0; a < bodies.size(); a++) {
        for (size_t c = 0; c < bodies.size(); c++) {
            Body &x = bodies[a], &y = bodies[c];
            Vec3 push;
            if (a == c || !x.rigid || !x.collider || !y.collider || (y.rigid && c < a) || !overlap(x, y, push)) continue;
            hits.push_back({&x, &y});
        }
    }
    for (auto [x, y] : hits) {
        if (x->inst->alive && y->inst->alive) vm.call(*x->inst, "on_collision", {Value(Ref{y->inst})});
        if (x->inst->alive && y->inst->alive) vm.call(*y->inst, "on_collision", {Value(Ref{x->inst})});
    }
}
