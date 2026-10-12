// systemtest: runs NV SMP Physics' system builder and physics world on an smpify'd NIF, outside the
// game. The NIF's shapes, skins and partitions are laid out in memory as Fallout New Vegas has them
// (the plugin's NetImmerseUtils.h types), attached to a skeleton the way the game attaches armor
// (skin slots pointed at the skeleton's nodes by name, unknown ones at the skeleton root), and walked.
//
//   systemtest item.nif physics.xml [frames]
//
// Reports the build (bones, bodies, constraints), the hem's swing and the closest the simulated coat
// gets to the legs, and fails on NaN or a bone flying off.
#include "hdtSkyrimPhysicsWorld.h"
#include "hdtSkyrimSystem.h"
#include "nif.h"
#include "scene.h"
#include "sampler.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <set>

using namespace scene;

namespace nvsmp
{
	bool LogEnabled(LogLevel level) { return level >= LogLevel::Info; }
	void LogLine(LogLevel level, const std::string& text)
	{
		static const char* kNames[] = { "trace", "debug", "info", "warn", "error" };
		if (level >= LogLevel::Warn || getenv("SYSTEMTEST_VERBOSE"))
			printf("  [%s] %s\n", kNames[(int)level], text.c_str());
	}
}


// A picture of the skinned shapes, flat-shaded with a depth buffer, orthographic in the given frame
// (y forward, z up): view 0 from the front (looking down -y), 1 from the right side (looking down -x).
// Written as a binary PPM; each shape gets its own colour.
static void Render(const char* path, const std::vector<RE::NiGeometry*>& geoms, const RE::NiTransform& frame, int view)
{
	const int W = 600, H = 900;
	std::vector<float> depth(W * H, -1e30f);
	std::vector<uint8_t> rgb(W * H * 3, 40);
	const float colours[][3] = { { 150, 110, 70 }, { 90, 90, 110 }, { 80, 150, 80 }, { 160, 80, 80 }, { 160, 160, 80 }, { 80, 160, 160 } };
	RE::NiTransform inv = frame.Invert();
	// The frame's origin is at the pelvis: show 60 units above to 100 below, 50 either side.
	auto toScreen = [&](RE::NiPoint3 p, float& sx, float& sy, float& sz) {
		float across = view == 0 ? -p.x : p.y, toward = view == 0 ? p.y : p.x;
		sx = (across + 50.0f) / 100.0f * W;
		sy = (60.0f - p.z) / 160.0f * H;
		sz = toward;   // the camera sits on +y (front) / +x (right side): nearer = larger
	};
	for (size_t gi = 0; gi < geoms.size(); gi++) {
		auto* g = geoms[gi];
		auto weights = WeightsOf(g);
		std::vector<RE::NiPoint3> pts(g->data->vertexCount);
		for (int v = 0; v < g->data->vertexCount; v++) pts[v] = inv * SkinVertex(g, v, weights);
		auto* td = (RE::NiTriShapeData*)g->data;
		const float* c = colours[gi % 6];
		for (uint32_t t = 0; t + 2 < td->triListLength; t += 3) {
			RE::NiPoint3 a = pts[td->triList[t]], b = pts[td->triList[t + 1]], d = pts[td->triList[t + 2]];
			RE::NiPoint3 e1 = b - a, e2 = d - a;
			RE::NiPoint3 n(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
			float len = n.Length();
			if (len < 1e-8f) continue;
			float facing = std::fabs((view == 0 ? n.y : n.x) / len);
			float shade = 0.35f + 0.65f * facing;
			float x[3], y[3], z[3];
			toScreen(a, x[0], y[0], z[0]); toScreen(b, x[1], y[1], z[1]); toScreen(d, x[2], y[2], z[2]);
			int x0 = std::max(0, (int)std::floor(std::min({ x[0], x[1], x[2] }))), x1 = std::min(W - 1, (int)std::ceil(std::max({ x[0], x[1], x[2] })));
			int y0 = std::max(0, (int)std::floor(std::min({ y[0], y[1], y[2] }))), y1 = std::min(H - 1, (int)std::ceil(std::max({ y[0], y[1], y[2] })));
			float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
			if (std::fabs(area) < 1e-6f) continue;
			for (int py = y0; py <= y1; py++)
				for (int px = x0; px <= x1; px++) {
					float fx = px + 0.5f, fy = py + 0.5f;
					float w0 = ((x[1] - fx) * (y[2] - fy) - (x[2] - fx) * (y[1] - fy)) / area;
					float w1 = ((x[2] - fx) * (y[0] - fy) - (x[0] - fx) * (y[2] - fy)) / area;
					float w2 = 1 - w0 - w1;
					if (w0 < 0 || w1 < 0 || w2 < 0) continue;
					float zz = w0 * z[0] + w1 * z[1] + w2 * z[2];
					int i = py * W + px;
					if (zz <= depth[i]) continue;
					depth[i] = zz;
					for (int k = 0; k < 3; k++) rgb[i * 3 + k] = (uint8_t)std::min(255.0f, c[k] * shade);
				}
		}
	}
	FILE* f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	fwrite(rgb.data(), 1, rgb.size(), f);
	fclose(f);
}


int main(int argc, char** argv)
{
	if (argc < 3) {
		printf("usage: systemtest item.nif physics.xml [frames]\n");
		return 1;
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	int frames = argc > 3 ? atoi(argv[3]) : 600;
	// The NIF names its physics file relative to Data; the test has it on the command line. The file
	// name must match, as the game's lookup would need it to.
	const std::string xmlPath = argv[2];
	scene::gReadFile = [xmlPath](const char* path) -> std::string {
		auto base = [](std::string p) {
			for (auto& c : p) if (c == '/') c = '\\';
			auto s = p.find_last_of('\\');
			return s == std::string::npos ? p : p.substr(s + 1);
		};
		if (_stricmp(base(path).c_str(), base(xmlPath).c_str()) != 0) {
			printf("  the NIF names physics file '%s', not '%s'\n", path, xmlPath.c_str());
			return {};
		}
		std::ifstream in(xmlPath, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	};
	try {
		scene::Item item = scene::Load(argv[1]);
		auto& sk = item.skeleton;
		auto* model = item.model;
		auto& shapes = item.byName;
		printf("skeleton: %zu bones\n", sk.bones.size());

		// Build, as the actor manager does: from the file the item's root names.
		const char* named = item.physicsFile.empty() ? nullptr : item.physicsFile.c_str();
		if (!named) {
			printf("FAIL: the NIF's root names no physics file\n");
			return 2;
		}
		printf("physics file: %s\n", named);
		hdt::DefaultBBP::PhysicsFile_t file{ named, {} };
		hdt::SkyrimSystemCreator creator;
		creator.m_ground = getenv("SYSTEMTEST_NOGROUND") == nullptr;
		auto system = creator.createOrUpdateSystem(sk.actor, model, &file, {}, nullptr);
		if (!system) {
			printf("FAIL: no system built\n");
			return 2;
		}
		int dynamic = 0;
		for (auto& b : system->getBones()) dynamic += !b->m_rig.isStaticOrKinematicObject();
		printf("system: %zu bones (%d simulated), %zu collision bodies\n", system->getBones().size(), dynamic, system->meshes().size());
		auto* world = hdt::SkyrimPhysicsWorld::get();
		world->addSkinnedMeshSystem(system.get());

		auto* coat = shapes.count("Upperbody:Coat") ? shapes["Upperbody:Coat"] : nullptr;
		auto* legs = shapes.count("Legs") ? shapes["Legs"] : nullptr;
		auto coatWeights = coat ? WeightsOf(coat) : decltype(WeightsOf(nullptr)){};
		auto legWeights = legs ? WeightsOf(legs) : decltype(WeightsOf(nullptr)){};
		std::vector<int> skirt;
		if (coat) for (int v = 0; v < coat->data->vertexCount; v++) for (auto& [slot, w] : coatWeights[v]) if (_strnicmp(coat->skinInstance->bones[slot]->m_name, "SMP Coat", 8) == 0 && w > 0.3f) { skirt.push_back(v); break; }
		printf("%zu skirt vertices on simulated bones\n", skirt.size());

		auto* pelvis = sk.bones["Bip01 Pelvis"];
		auto* lThigh = sk.bones["Bip01 L Thigh"], * rThigh = sk.bones["Bip01 R Thigh"];
		auto* lCalf = sk.bones["Bip01 L Calf"], * rCalf = sk.bones["Bip01 R Calf"];
		RE::NiTransform lThigh0 = lThigh->local, rThigh0 = rThigh->local, lCalf0 = lCalf->local, rCalf0 = rCalf->local;
		hdt::SkyrimBone* hem = nullptr;
		for (auto& b : system->getBones()) if (b->m_name == RE::BSFixedString("SMP Coat C05 R4")) hem = static_cast<hdt::SkyrimBone*>(b.get());

		const float dt = 1.0f / 60.0f;
		// SYSTEMTEST_CROUCH=units: the body sinks that far over the first second (the actor's root, and with it
		// the ground plane, stays): the lowest the coat then gets, against the ground.
		const float crouch = getenv("SYSTEMTEST_CROUCH") ? (float)atof(getenv("SYSTEMTEST_CROUCH")) : 0.0f;
		const RE::NiTransform bip01Rest = sk.bip01->local;
		float lowestCoat = 1e9f;
		float maxSwing = 0, minClear = 1e9f, sumClear = 0;
		int clearSamples = 0;
		bool bad = false;
		const bool still = getenv("SYSTEMTEST_STILL") != nullptr;
		// SYSTEMTEST_RUN: a run instead of a walk (400 units/s, thighs +/-45 deg at 1.5 Hz).
		const bool run = getenv("SYSTEMTEST_RUN") != nullptr;
		const float speed = run ? 400.0f : 150.0f, stride = run ? 0.79f : 0.49f, rate = run ? 1.5f : 1.0f;

		// The legs as tapered capsules (thigh to calf, calf to foot), as thick as the leg mesh around each end
		// at rest: a skirt vertex closer to a leg's bone line than that is inside the leg.
		struct Segment { RE::NiAVObject* a; RE::NiAVObject* b; float ra, rb; };
		std::vector<Segment> segs;
		for (const char* side : { "L", "R" }) {
			auto* th = sk.bones[std::string("Bip01 ") + side + " Thigh"];
			auto* ca = sk.bones[std::string("Bip01 ") + side + " Calf"];
			auto* fo = sk.bones[std::string("Bip01 ") + side + " Foot"];
			if (th && ca) segs.push_back({ th, ca, 0, 0 });
			if (ca && fo) segs.push_back({ ca, fo, 0, 0 });
		}
		// Distance from a segment's bone line, and where along it.
		auto segDistT = [](const Segment& g, const RE::NiPoint3& p, float& t) {
			RE::NiPoint3 a = g.a->world.translate, ab = g.b->world.translate - a, ap = p - a;
			float len2 = std::max(ab.x * ab.x + ab.y * ab.y + ab.z * ab.z, 1e-6f);
			t = std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2, 0.0f, 1.0f);
			return (p - (a + ab * t)).Length();
		};
		auto segDist = [&](const Segment& g, const RE::NiPoint3& p) { float t; return segDistT(g, p, t); };
		// How deep a point is inside a segment's tapered capsule (negative: outside).
		auto segDepth = [&](const Segment& g, const RE::NiPoint3& p) { float t; float d = segDistT(g, p, t); return g.ra + (g.rb - g.ra) * t - d; };
		if (legs) {
			std::vector<std::vector<float>> all(segs.size()), nearA(segs.size()), nearB(segs.size());
			for (int v = 0; v < legs->data->vertexCount; v++) {
				RE::NiPoint3 q = SkinVertex(legs, v, legWeights);
				int best = -1;
				float bd = 1e9f, bt = 0;
				for (size_t i = 0; i < segs.size(); i++) {
					float t, x = segDistT(segs[i], q, t);
					if (x < bd) { bd = x; bt = t; best = (int)i; }
				}
				if (best < 0) continue;
				all[best].push_back(bd);
				if (bt < 0.33f) nearA[best].push_back(bd);
				if (bt > 0.67f) nearB[best].push_back(bd);
			}
			auto quartile = [](std::vector<float> v) { std::sort(v.begin(), v.end()); return v[v.size() / 4]; };
			for (size_t i = 0; i < segs.size(); i++) {
				if (all[i].empty()) continue;
				float whole = quartile(all[i]);   // the leg's thinner side: a lower bound
				segs[i].ra = nearA[i].size() >= 10 ? quartile(nearA[i]) : whole;
				segs[i].rb = nearB[i].size() >= 10 ? quartile(nearB[i]) : whole;
			}
		}
		int insideSamples = 0, insideCount = 0;
		// The same for the coat at hip height alone: vertices mostly on the top two grid rows (R0, R1).
		std::set<int> upper;
		if (coat)
			for (int v : skirt)
				for (auto& [slot, w] : coatWeights[v]) {
					std::string n = coat->skinInstance->bones[slot]->m_name;
					if (w > 0.3f && n.size() > 3 && (n.compare(n.size() - 3, 3, " R0") == 0 || n.compare(n.size() - 3, 3, " R1") == 0)) { upper.insert(v); break; }
				}
		int upperSamples = 0, upperInside = 0;
		float upperWorst = 0;
		float worstDepth = 0;
		RE::NiPoint3 hemRest;   // the hem in the pelvis' frame at rest
		float swingByPhase[4] = {};
		const char* phaseNames[4] = { "standing", "walking", "turning", "stopped" };
		float bob = 0;
		const bool sample = getenv("SYSTEMTEST_SAMPLE") != nullptr;
		if (sample)
			sampler::Start();
		for (int frame = 0; frame < frames; frame++) {
			float t = frame * dt;
			// Stand for a second; walk at 150 units/s (thighs +/-28 deg at 1 Hz, calves bending on the back
			// swing, the pelvis bobbing 2 units twice a step); at 4 s turn 90 degrees in half a second; at
			// 7 s stop dead.
			int phaseId = still ? 0 : t < 1.0f ? 0 : t < 4.0f ? 1 : t < 4.5f ? 2 : t < 7.0f ? 1 : 3;
			float walk = (phaseId == 1 || phaseId == 2) ? 1.0f : 0.0f;
			float phase = t * 2.0f * 3.14159f * rate;
			float turn = (still || t < 4.0f) ? 0.0f : std::min((t - 4.0f) / 0.5f, 1.0f) * 1.5708f;
			sk.actor->local.rotate = RotZ(turn);
			float newBob = 2.0f * sinf(phase * 2.0f) * walk;
			sk.actor->local.translate = sk.actor->local.translate + (sk.actor->local.rotate * RE::NiPoint3(0, speed * dt * walk, newBob - bob));
			bob = newBob;
			float swing = stride * sinf(phase) * walk;
			lThigh->local = lThigh0; rThigh->local = rThigh0; lCalf->local = lCalf0; rCalf->local = rCalf0;
			// SYSTEMTEST_POSE="spread forward" (degrees): a stance, both thighs out and the right one forward,
			// as an aiming idle stands; applied on top of the walk.
			if (const char* pose = getenv("SYSTEMTEST_POSE")) {
				float spread = 0, fwd = 0;
				sscanf(pose, "%f %f", &spread, &fwd);
				lThigh0 = lThigh->parent ? lThigh0 : lThigh0;
				static bool posed = false;
				if (!posed) {
					posed = true;
					const float d = 3.14159f / 180.0f;
					lThigh0.rotate = lThigh0.rotate * RotZ(spread * d);
					rThigh0.rotate = rThigh0.rotate * RotZ(-spread * d) * RotX(-fwd * d);
				}
				lThigh->local = lThigh0; rThigh->local = rThigh0;
			}
			lThigh->local.rotate = lThigh0.rotate * RotX(swing);
			rThigh->local.rotate = rThigh0.rotate * RotX(-swing);
			lCalf->local.rotate = lCalf0.rotate * RotX(std::max(0.0f, -swing) * 1.2f);
			rCalf->local.rotate = rCalf0.rotate * RotX(std::max(0.0f, swing) * 1.2f);
			if (crouch > 0) {
				sk.bip01->local = bip01Rest;
				sk.bip01->local.translate.z -= crouch * std::min(1.0f, t);
			}
			sk.actor->world = sk.actor->local;
			hdt::updateTransformUpDown(sk.bip01);
			hdt::updateTransformUpDown(model);

			sampler::gActive = true;
			world->doUpdate(dt);
			sampler::gActive = false;

			// SYSTEMTEST_REBUILD: at 2 s, drop the system and build it again without the ground, as the menu's
			// Ground collision switch does in game.
			if (getenv("SYSTEMTEST_REBUILD") && frame == 120) {
				printf("rebuilding without the ground\n");
				world->removeSkinnedMeshSystem(system.get());
				system = nullptr;
				hdt::SkyrimSystemCreator again;
				again.m_ground = false;
				system = again.createOrUpdateSystem(sk.actor, model, &file, {}, nullptr);
				if (!system) { printf("FAIL: no system on the rebuild\n"); return 2; }
				world->addSkinnedMeshSystem(system.get());
				hem = nullptr;
				for (auto& b : system->getBones()) if (b->m_name == RE::BSFixedString("SMP Coat C05 R4")) hem = static_cast<hdt::SkyrimBone*>(b.get());
				printf("rebuilt: %zu bones, %zu collision bodies\n", system->getBones().size(), system->meshes().size());
			}

			if (crouch > 0 && coat && frame > 60)
				for (int v : skirt) lowestCoat = std::min(lowestCoat, SkinVertex(coat, v, coatWeights).z - sk.actor->world.translate.z);

			// Standing still: how far each coat vertex settles from where it was drawn at rest, and
			// where the worst ones are (pelvis frame) and what they are skinned to.
			if (still && coat) {
				static std::vector<RE::NiPoint3> restPts;
				if (frame == 0)
					for (int v = 0; v < coat->data->vertexCount; v++) restPts.push_back(SkinVertex(coat, v, coatWeights));
				// SYSTEMTEST_RENDER=prefix: pictures at rest (frame 0) and settled (last frame), front and side.
				if (const char* prefix = getenv("SYSTEMTEST_RENDER"); prefix && (frame == 0 || frame == frames - 1)) {
					std::vector<RE::NiGeometry*> geoms;
					for (const char* n : { "Upperbody:Coat", "Legs", "Holster", "Upperbody:003" })
						if (shapes.count(n)) geoms.push_back(shapes[n]);
					RE::NiTransform view = sk.actor->world;
					view.translate = pelvis->world.translate;
					for (int side = 0; side < 2; side++) {
						char path[512];
						snprintf(path, sizeof path, "%s_%s_%s.ppm", prefix, frame == 0 ? "rest" : "settled", side ? "side" : "front");
						Render(path, geoms, view, side);
					}
				}
				if (frame == frames - 1) {
					std::vector<std::pair<float, int>> moved;
					for (int v = 0; v < coat->data->vertexCount; v++)
						moved.push_back({ (SkinVertex(coat, v, coatWeights) - restPts[v]).Length(), v });
					std::sort(moved.rbegin(), moved.rend());
					int over2 = 0;
					for (auto& m : moved) over2 += m.first > 2.0f;
					printf("coat vertices settled more than 2 units from rest: %d of %d\n", over2, (int)moved.size());
					for (int k = 0; k < 8 && k < (int)moved.size(); k++) {
						int v = moved[k].second;
						RE::NiPoint3 p = pelvis->world.Invert() * restPts[v];
						printf("  v%-5d moved %5.1f  at pelvis-frame (%6.1f %6.1f %6.1f) ", v, moved[k].first, p.x, p.y, p.z);
						for (auto& [slot, w] : coatWeights[v]) printf(" %s %.2f", (const char*)coat->skinInstance->bones[slot]->m_name, w);
						printf("\n");
					}
				}
			}

			for (auto& b : system->getBones()) {
				auto o = b->m_rig.getWorldTransform().getOrigin();
				if (!std::isfinite(o.x()) || !std::isfinite(o.y()) || !std::isfinite(o.z())) bad = true;
			}
			if (hem) {
				// How far the hem is from where it hangs at rest, in the pelvis' frame: the coat's swing.
				RE::NiPoint3 rel = pelvis->world.Invert() * hem->m_node->world.translate;
				if (frame == 0) hemRest = rel;
				float d = (rel - hemRest).Length();
				if (frame > 30) maxSwing = std::max(maxSwing, d);
				swingByPhase[phaseId] = std::max(swingByPhase[phaseId], frame > 30 ? d : 0.0f);
				if (d > 200.0f) bad = true;
				if (frame % 30 == 0) printf("t=%4.1fs hem %5.1f below the pelvis, swung %4.1f from rest  %s\n", t,
					pelvis->world.translate.z - hem->m_node->world.translate.z, d, phaseNames[phaseId]);
			}
			// Clearance: each skirt vertex to the nearest leg vertex, every 10 frames.
			if (coat && legs && frame % 10 == 0 && frame > 30) {
				std::vector<RE::NiPoint3> legPts;
				for (int v = 0; v < legs->data->vertexCount; v += 2) legPts.push_back(SkinVertex(legs, v, legWeights));
				for (int v : skirt) {
					RE::NiPoint3 p = SkinVertex(coat, v, coatWeights);
					float best = 1e9f;
					for (auto& q : legPts) best = std::min(best, (p - q).Length());
					minClear = std::min(minClear, best);
					insideSamples++;
					float depth = 0;
					for (auto& g : segs)
						if (g.ra > 0) depth = std::max(depth, segDepth(g, p));
					if (depth > 0.5f) {
						insideCount++;
						worstDepth = std::max(worstDepth, depth);
					}
					if (upper.count(v)) {
						upperSamples++;
						if (depth > 0.5f) { upperInside++; upperWorst = std::max(upperWorst, depth); }
					}
					sumClear += best;
					clearSamples++;
				}
			}
		}
		if (sample)
			sampler::Report();
		printf("hem swing from rest, most: standing %.1f, walking %.1f, turning %.1f, after the stop %.1f units\n",
			swingByPhase[0], swingByPhase[1], swingByPhase[2], swingByPhase[3]);
		if (clearSamples) printf("skirt to legs: closest %.2f units, average %.2f units\n", minClear, sumClear / clearSamples);
		if (insideSamples) printf("skirt inside the legs: %.1f%% of samples, deepest %.1f units\n", 100.0f * insideCount / insideSamples, worstDepth);
		if (upperSamples) printf("  at hip height (top rows): %.1f%% of samples, deepest %.1f units\n", 100.0f * upperInside / upperSamples, upperWorst);
		if (crouch > 0) printf("crouched %.0f: lowest coat point %.1f units %s the ground\n", crouch, std::fabs(lowestCoat), lowestCoat < 0 ? "below" : "above");
		printf("step time %.3f ms\n", world->m_averageStepMs);
#ifdef NVSMP_PROFILE
		{
			double sum = 0;
			for (double t : nvsmp::prof::gTotal) sum += t;
			printf("per frame (%d frames):\n", frames);
			for (int s = 0; s < nvsmp::prof::Count; s++)
				printf("  %-24s %6.3f ms  %4.1f%%\n", nvsmp::prof::kNames[s], nvsmp::prof::gTotal[s] / frames, sum > 0 ? 100.0 * nvsmp::prof::gTotal[s] / sum : 0.0);
		}
#endif
		if (bad) {
			printf("FAIL: NaN or a bone flew off\n");
			return 3;
		}
		if (maxSwing < 0.5f) {
			printf("FAIL: the cloth does not move\n");
			return 4;
		}
		printf("OK\n");
	} catch (std::exception& e) {
		printf("error: %s\n", e.what());
		return 2;
	}
	return 0;
}
