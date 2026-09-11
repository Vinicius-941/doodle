#include "obj.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>
#include <tuple>

static std::string restOfLine(std::istringstream& ls) {  // file names may contain spaces
    std::string s;
    std::getline(ls >> std::ws, s);
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

static std::map<std::string, ObjPart> parseMtl(const std::string& text) {  // material name -> color/texture
    std::map<std::string, ObjPart> mats;
    ObjPart* cur = nullptr;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        std::istringstream ls(line);
        std::string key, name;
        ls >> key;
        if (key == "newmtl" && ls >> name) cur = &mats[name];
        else if (cur && key == "Kd") ls >> cur->color.x >> cur->color.y >> cur->color.z;
        else if (cur && key == "map_Kd") cur->texture = restOfLine(ls);
    }
    return mats;
}

std::vector<ObjPart> parseObj(const std::string& obj, const std::function<std::string(const std::string&)>& readFile) {
    std::vector<Vec3> pos, nrm;
    std::vector<std::pair<double, double>> uv;
    std::map<std::string, ObjPart> mats;
    std::vector<ObjPart> parts(1);  // parts[0]: faces before any usemtl
    std::map<std::string, size_t> partOf;
    size_t cur = 0;
    int lineNo = 0;
    auto fail = [&](const std::string& m) { return std::runtime_error("linha " + std::to_string(lineNo) + ": " + m); };

    std::istringstream in(obj);
    for (std::string line; std::getline(in, line);) {
        lineNo++;
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "v") {
            Vec3 p;
            ls >> p.x >> p.y >> p.z;
            pos.push_back(p);
        } else if (key == "vt") {
            double u = 0, v = 0;
            ls >> u >> v;
            uv.push_back({u, v});
        } else if (key == "vn") {
            Vec3 n;
            ls >> n.x >> n.y >> n.z;
            nrm.push_back(n);
        } else if (key == "mtllib") {
            for (auto& m : parseMtl(readFile(restOfLine(ls)))) mats.insert(m);
        } else if (key == "usemtl") {
            std::string name;
            ls >> name;
            auto it = partOf.find(name);
            if (it == partOf.end()) {
                it = partOf.emplace(name, parts.size()).first;
                auto m = mats.find(name);
                parts.push_back(m != mats.end() ? m->second : ObjPart{});
                parts.back().tris.clear();
            }
            cur = it->second;
        } else if (key == "f") {
            std::vector<ObjVertex> poly;
            for (std::string corner; ls >> corner;) {  // "v", "v/vt", "v//vn" or "v/vt/vn"
                long idx[3] = {0, 0, 0};                 // 0 = absent
                size_t start = 0;
                for (int k = 0; k < 3; k++) {
                    size_t slash = corner.find('/', start);
                    std::string num = corner.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
                    char* end;
                    if (!num.empty()) idx[k] = strtol(num.c_str(), &end, 10);
                    if (!num.empty() && *end) throw fail("vértice inválido '" + corner + "'");
                    if (slash == std::string::npos) break;
                    start = slash + 1;
                }
                auto at = [&](long i, size_t n) {  // 1-based, or negative = counted from the end
                    long r = i > 0 ? i - 1 : (long)n + i;
                    if (i == 0 || r < 0 || r >= (long)n) throw fail("índice fora da lista em '" + corner + "'");
                    return (size_t)r;
                };
                ObjVertex vx;
                vx.pos = pos[at(idx[0], pos.size())];
                if (idx[1]) std::tie(vx.u, vx.v) = uv[at(idx[1], uv.size())];
                if (idx[2]) vx.normal = nrm[at(idx[2], nrm.size())];
                poly.push_back(vx);
            }
            if (poly.size() < 3) throw fail("face com menos de 3 vértices");
            for (size_t i = 1; i + 1 < poly.size(); i++) {  // fan: (0, i, i+1)
                ObjVertex tri[3] = {poly[0], poly[i], poly[i + 1]};
                Vec3 a = tri[1].pos - tri[0].pos, b = tri[2].pos - tri[0].pos;
                Vec3 flat{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
                double len = std::sqrt(flat.dot(flat));
                if (len > 0) flat = flat * (1 / len);
                for (auto& t : tri) {
                    if (t.normal == Vec3{}) t.normal = flat;
                    parts[cur].tris.push_back(t);
                }
            }
        }
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const ObjPart& p) { return p.tris.empty(); }), parts.end());
    if (parts.empty()) throw std::runtime_error("o modelo não tem nenhuma face");
    return parts;
}
