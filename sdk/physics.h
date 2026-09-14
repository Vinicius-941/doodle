// Doodle SDK physics: collider/Rigidbody components and the per-frame step. No platform code.
#pragma once
#include "vm.h"

// Registers the `use` components and the fields they add:
//   BoxCollider (size, rotation), SphereCollider (radius), CapsuleCollider (radius, height) — all with position, trigger
//   Rigidbody (velocity, gravity, grounded, slope_limit)
//   TerrainCollider (size, heights) — a heightfield; plus terrain_height(x, z) for the calling terrain
void registerPhysics(VM& vm);

// Ground height of a TerrainCollider instance at world (x, z); false outside it. Matches the rendered mesh.
bool terrainHeight(Instance& terrain, double x, double z, double& y);
// O mesmo, e também a normal da superfície naquele ponto (para saber se é chão ou ladeira íngreme).
bool terrainSurface(Instance& terrain, double x, double z, double& y, Vec3& normal);

// Moves Rigidbodies (gravity + velocity), pushes them out of solid colliders, then calls
// collision(other) on both sides of every touching pair that has at least one Rigidbody.
void physicsStep(VM& vm, const std::vector<std::shared_ptr<Instance>>& scene, double dt);
