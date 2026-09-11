// Doodle SDK physics: collider/Rigidbody components and the per-frame step. No platform code.
#pragma once
#include "vm.h"

// Registers the `use` components and the fields they add:
//   BoxCollider (size), SphereCollider (radius), CapsuleCollider (radius, height) — all with position, trigger
//   Rigidbody (velocity, gravity, grounded)
void registerPhysics(VM& vm);

// Moves Rigidbodies (gravity + velocity), pushes them out of solid colliders, then calls
// on_collision(other) on both sides of every touching pair that has at least one Rigidbody.
void physicsStep(VM& vm, const std::vector<std::shared_ptr<Instance>>& scene, double dt);
