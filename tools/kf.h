#pragma once
// Fallout 3 / New Vegas animations (.kf: an NiControllerSequence, or BSAnimGroupSequence, of per-node
// transform interpolators), read and sampled. Both interpolator kinds the game's files use are read:
// NiTransformInterpolator with NiTransformData keys (linear, quadratic, TBC and XYZ-Euler rotations) and
// the compressed NiBSplineCompTransformInterpolator (open uniform cubic B-splines over quantised control
// points). Tangents of quadratic and TBC keys are ignored: those keys are interpolated linearly.
#include "nif.h"

#include <string>
#include <vector>

namespace kf
{
	struct Quat
	{
		float w = 1, x = 0, y = 0, z = 0;
	};
	Quat Slerp(Quat a, Quat b, float t);
	nif::Mat3 ToMatrix(const Quat& q);   // v' = M * v, the NiTransform convention
	Quat FromMatrix(const nif::Mat3& m);

	// A node's pose at a moment: which parts the animation sets.
	struct Pose
	{
		bool hasRotation = false, hasTranslation = false, hasScale = false;
		Quat rotation;
		nif::Vec3 translation;
		float scale = 1;
	};

	struct Track
	{
		std::string node;
		// The interpolator's own pose: used where it has no keys (or a spline handle is unused).
		Pose rest;
		// Keyframed.
		struct QKey { float t; Quat q; };
		struct VKey { float t; nif::Vec3 v; };
		struct FKey { float t; float v; };
		std::vector<QKey> rotations;
		std::vector<FKey> euler[3];          // XYZ rotations (radians), when rotations are given that way
		std::vector<VKey> translations;
		std::vector<FKey> scales;
		// B-spline.
		bool spline = false;
		float start = 0, stop = 0;
		int controlPoints = 0;
		std::vector<float> splineT, splineR, splineS;   // dequantised control points (3, 4, 1 per point), or empty
	};

	struct Clip
	{
		std::string name;
		std::string accumRoot;   // the node whose motion moves the actor (usually "Bip01")
		float start = 0, stop = 0;
		bool loops = true;
		std::vector<Track> tracks;
		float Duration() const { return stop > start ? stop - start : 0.0f; }
	};

	// Throws std::runtime_error on a file the reader cannot parse.
	Clip Load(const std::string& path);
	Pose Sample(const Track& track, float time);
}
