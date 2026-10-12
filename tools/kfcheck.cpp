// kfcheck: reads .kf animations with tools/kf.cpp and checks them against a skeleton.
//   kfcheck skeleton.nif anim.kf [anim.kf ...]
// For each file: its tracks and how they are stored, then the skeleton posed with the animation at its
// start, both ways the rotations could map onto the skeleton's matrices: the right one keeps the feet
// and hands near where an idle or walk has them (bone lengths and the leg chain intact).
#include "kf.h"

#include <cstdio>
#include <functional>
#include <map>

using namespace nif;

struct Bone
{
	std::string name, parent;
	Transform local;
};

static std::map<std::string, Bone> LoadSkeleton(const std::string& path)
{
	File f;
	f.load(path);
	std::map<std::string, Bone> bones;
	std::function<void(int, const std::string&)> walk = [&](int blk, const std::string& parent) {
		const std::string& t = f.typeOf(blk);
		if (t.find("Node") == std::string::npos) return;
		Reader r(f.blocks[blk].data(), f.blocks[blk].size());
		AVObject av;
		av.read(r, true);
		std::string name = av.name < f.strings.size() ? f.strings[av.name] : "";
		bones[name] = { name, parent, av.local };
		for (int c : av.children)
			if (c >= 0) walk(c, name);
	};
	walk(f.roots[0], "");
	return bones;
}

static Vec3 WorldPos(const std::map<std::string, Bone>& bones, const std::map<std::string, Transform>& locals, const std::string& name)
{
	Transform w;
	std::string n = name;
	std::vector<Transform> chain;
	while (!n.empty()) {
		auto it = bones.find(n);
		if (it == bones.end()) break;
		auto lo = locals.find(n);
		chain.push_back(lo != locals.end() ? lo->second : it->second.local);
		n = it->second.parent;
	}
	for (auto i = chain.rbegin(); i != chain.rend(); ++i) w = w * *i;
	return w.trans;
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		printf("usage: kfcheck skeleton.nif anim.kf [anim.kf ...]\n");
		return 1;
	}
	try {
		auto bones = LoadSkeleton(argv[1]);
		printf("skeleton: %zu nodes\n", bones.size());
		for (int a = 2; a < argc; a++) {
			kf::Clip clip = kf::Load(argv[a]);
			int splines = 0, keyed = 0, matched = 0;
			for (auto& t : clip.tracks) {
				(t.spline ? splines : keyed)++;
				matched += bones.count(t.node) > 0;
			}
			printf("\n%s\n  '%s' %.2f s %s, accum root '%s', %zu tracks (%d B-spline, %d keyed), %d on skeleton nodes\n", argv[a], clip.name.c_str(),
				clip.Duration(), clip.loops ? "loop" : "once", clip.accumRoot.c_str(), clip.tracks.size(), splines, keyed, matched);
			// Which order XYZ-Euler keys compose in: the one that puts an idle's pelvis at the skeleton's rest.
			if (getenv("KFCHECK_EULER"))
				for (auto& t : clip.tracks) {
					if (t.euler[0].empty() || !bones.count(t.node)) continue;
					float a[3];
					for (int k = 0; k < 3; k++) a[k] = t.euler[k].empty() ? 0.0f : t.euler[k][0].v;
					auto R = [](int axis, float ang) {
						Mat3 m;
						float c = std::cos(ang), s = std::sin(ang);
						int i = (axis + 1) % 3, j = (axis + 2) % 3;
						m.m[i][i] = c; m.m[i][j] = -s; m.m[j][i] = s; m.m[j][j] = c;
						return m;
					};
					Mat3 X = R(0, a[0]), Y = R(1, a[1]), Z = R(2, a[2]);
					Mat3 cands[] = { X * Y * Z, Z * Y * X, (X * Y * Z).transposed(), (Z * Y * X).transposed() };
					const char* names[] = { "XYZ", "ZYX", "XYZ^T", "ZYX^T" };
					kf::Quat rest = kf::FromMatrix(bones[t.node].local.rot);
					printf("    %-22s", t.node.c_str());
					for (int c = 0; c < 4; c++) {
						kf::Quat q = kf::FromMatrix(cands[c]);
						printf("  %s %.2f", names[c], std::fabs(q.w * rest.w + q.x * rest.x + q.y * rest.y + q.z * rest.z));
					}
					printf("\n");
				}
			// The root's heading through the clip (KFCHECK_HEADING): where NonAccum's X axis points, in degrees.
			if (getenv("KFCHECK_HEADING"))
				for (auto& t : clip.tracks) {
					if (t.node != "Bip01 NonAccum") continue;
					printf("    heading:");
					for (int s = 0; s <= 8; s++) {
						float time = clip.start + clip.Duration() * s / 8.0f;
						Mat3 m = kf::ToMatrix(kf::Sample(t, time).rotation);
						printf(" %.0f", std::atan2(m.m[1][0], m.m[0][0]) * 57.2958f);
						if (!t.euler[0].empty()) {
							auto e = [&](int k) { return t.euler[k].empty() ? 0.0f : t.euler[k][std::min<size_t>(t.euler[k].size() - 1, t.euler[k].size() * s / 8)].v * 57.2958f; };
							printf("(e %.0f,%.0f,%.0f)", e(0), e(1), e(2));
						}
					}
					printf("\n");
				}
			if (getenv("KFCHECK_TRACKS"))
				for (auto& t : clip.tracks) {
					if (t.node.find("Bip01") != 0 || t.node.find("Pelvis") == std::string::npos && t.node != "Bip01" && t.node != "Bip01 NonAccum") continue;
					kf::Pose p = kf::Sample(t, clip.start);
					printf("    %-16s rot keys %zu euler %zu/%zu/%zu trans keys %zu  rest(R%d T%d) q(%.2f %.2f %.2f %.2f) t(%.1f %.1f %.1f)\n", t.node.c_str(), t.rotations.size(), t.euler[0].size(), t.euler[1].size(), t.euler[2].size(), t.translations.size(), t.rest.hasRotation, t.rest.hasTranslation, p.rotation.w, p.rotation.x, p.rotation.y, p.rotation.z, p.translation.x, p.translation.y, p.translation.z);
					auto it = bones.find(t.node);
					if (it != bones.end()) { auto q = kf::FromMatrix(it->second.local.rot); printf("    %-16s skeleton q(%.2f %.2f %.2f %.2f) t(%.1f %.1f %.1f)\n", "", q.w, q.x, q.y, q.z, it->second.local.trans.x, it->second.local.trans.y, it->second.local.trans.z); }
				}
			for (int transpose = 0; transpose < 2; transpose++) {
				std::map<std::string, Transform> locals;
				// The accumulation root carries only the actor's motion: at rest it is the identity (the
				// skeleton's own Bip01 rotation and height are in NonAccum's keys).
				if (bones.count(clip.accumRoot)) locals[clip.accumRoot] = Transform();
				for (auto& t : clip.tracks) {
					if (t.node == clip.accumRoot) continue;
					auto it = bones.find(t.node);
					if (it == bones.end()) continue;
					kf::Pose p = kf::Sample(t, clip.start);
					Transform l = it->second.local;
					if (p.hasRotation) { l.rot = kf::ToMatrix(p.rotation); if (transpose) l.rot = l.rot.transposed(); }
					if (p.hasTranslation) l.trans = p.translation;
					if (p.hasScale) l.scale = p.scale;
					locals[t.node] = l;
				}
				printf("  %s:", transpose ? "transposed" : "as is     ");
				for (const char* n : { "Bip01 Pelvis", "Bip01 L Foot", "Bip01 R Foot", "Bip01 Head", "Bip01 L Hand" }) {
					if (!bones.count(n)) continue;
					Vec3 rest = WorldPos(bones, {}, n), posed = WorldPos(bones, locals, n);
					printf("  %s z %.0f->%.0f (xy %.0f,%.0f)", n + 6, rest.z, posed.z, posed.x, posed.y);
				}
				printf("\n");
			}
		}
	} catch (const std::exception& e) {
		printf("error: %s\n", e.what());
		return 1;
	}
	return 0;
}
