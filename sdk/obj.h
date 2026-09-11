// Wavefront .obj/.mtl reader for render.mesh models. No platform code.
#pragma once
#include <functional>
#include "vm.h"

struct ObjVertex {
    Vec3 pos, normal;
    double u = 0, v = 0;  // texture coords, origin bottom-left
};

struct ObjPart {                  // the triangles of one material
    Vec3 color{1, 1, 1};          // Kd
    std::string texture;          // map_Kd, relative to the .obj ("" = none)
    std::vector<ObjVertex> tris;  // 3 vertices per triangle
};

// Reads v, vt, vn, f, usemtl and mtllib; readFile(name) loads the .mtl files it references.
// Polygons are fanned into triangles; corners without vn get the face's flat normal.
std::vector<ObjPart> parseObj(const std::string& obj, const std::function<std::string(const std::string&)>& readFile);
