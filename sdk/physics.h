// Doodle SDK physics: collider/Rigidbody components and the per-frame step. No platform code.
#pragma once
#include "vm.h"

// Registers the `use` components and the fields they add:
//   BoxCollider (size), SphereCollider (radius), CapsuleCollider (radius, height) — all with position, trigger
//   Rigidbody (velocity, gravity, grounded)
//   TerrainCollider (size, heights) — a heightfield; plus physics.terrain_height(x, z) for the calling terrain
void registerPhysics(VM& vm);

// Ground height of a TerrainCollider instance at world (x, z); false outside it. Matches the rendered mesh.
bool terrainHeight(Instance& terrain, double x, double z, double& y);

// Moves Rigidbodies (gravity + velocity), pushes them out of solid colliders, then calls
// on_collision(other) on both sides of every touching pair that has at least one Rigidbody.
void physicsStep(VM& vm, const std::vector<std::shared_ptr<Instance>>& scene, double dt);
