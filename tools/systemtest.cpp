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

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>

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

// --- fake objects with the game's layout -------------------------------------------------------------

static void* __fastcall ReturnSelf(void* self, void*) { return self; }
static void* __fastcall ReturnNull(void*, void*) { return nullptr; }
static void __fastcall DeleteNothing(void*, void*) {}

static void* gNodeVtbl[16], *gGeomVtbl[16], *gOtherVtbl[16];
static void InitVtables()
{
	for (int i = 0; i < 16; i++) gNodeVtbl[i] = gGeomVtbl[i] = gOtherVtbl[i] = (void*)ReturnNull;
	gNodeVtbl[1] = gGeomVtbl[1] = gOtherVtbl[1] = (void*)DeleteNothing;
	gNodeVtbl[3] = (void*)ReturnSelf;   // IsNode
	gGeomVtbl[6] = (void*)ReturnSelf;   // IsGeometry
}

template <class T>
static T* Alloc(size_t size, void** vtbl)
{
	auto* p = (T*)calloc(1, std::max(size, sizeof(T)));
	((RE::NiObject*)p)->vtbl = vtbl;
	((RE::NiObject*)p)->refCount = 1000;   // never freed during the test
	return p;
}

static RE::NiNode* NewNode(const std::string& name)
{
	auto* n = Alloc<RE::NiNode>(0xAC, gNodeVtbl);
	n->m_name = _strdup(name.c_str());
	n->local = RE::NiTransform();   // identity: calloc left the rotations all zero
	n->world = RE::NiTransform();
	return n;
}

static void AddChild(RE::NiNode* parent, RE::NiAVObject* child)
{
	auto** arr = (RE::NiAVObject**)realloc(parent->children, sizeof(void*) * (parent->childFirstFree + 1));
	parent->children = arr;
	arr[parent->childFirstFree++] = child;
	parent->childCount = parent->childFirstFree;
	child->parent = parent;
}

static void AddStringExtra(RE::NiObjectNET* obj, const char* name, const std::string& value)
{
	auto* e = Alloc<RE::NiStringExtraData>(0x10, gOtherVtbl);
	e->name = _strdup(name);
	e->value = _strdup(value.c_str());
	obj->extraData = (RE::NiExtraData**)realloc(obj->extraData, sizeof(void*) * (obj->extraDataCount + 1));
	obj->extraData[obj->extraDataCount++] = e;
}

static RE::NiTransform ToRE(const nif::Transform& t)
{
	RE::NiTransform r;
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.rotate.entry[i][j] = t.rot.m[i][j];
	r.translate = { t.trans.x, t.trans.y, t.trans.z };
	r.scale = t.scale;
	return r;
}

namespace hdt
{
	RE::NiNode* createNode(const char* name) { return NewNode(name); }

	static std::string gXmlPath;
	std::string readAllFile(const char*)
	{
		std::ifstream in(gXmlPath, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}
}

static std::string StrName(const nif::File& f, int block)
{
	nif::Reader r(f.blocks[block].data(), f.blocks[block].size());
	uint32_t s = r.u32();
	return s < f.strings.size() ? f.strings[s] : "";
}

// --- the skeleton -------------------------------------------------------------------------------------

struct Skeleton
{
	RE::NiNode* actor;   // the actor's 3D root (what the plugin calls the skeleton)
	RE::NiNode* bip01;   // the skeleton root the game points unknown skin slots at
	std::map<std::string, RE::NiNode*> bones;
	std::map<std::string, RE::NiTransform> bind;   // world bind pose
};

static const char* kParents[][2] = {
	{ "Bip01 Pelvis", "Bip01" }, { "Bip01 Spine", "Bip01 Pelvis" }, { "Bip01 Spine1", "Bip01 Spine" }, { "Bip01 Spine2", "Bip01 Spine1" },
	{ "Bip01 Neck1", "Bip01 Spine2" }, { "Bip01 L Clavicle", "Bip01 Spine2" }, { "Bip01 R Clavicle", "Bip01 Spine2" },
	{ "Bip01 L UpperArm", "Bip01 L Clavicle" }, { "Bip01 R UpperArm", "Bip01 R Clavicle" },
	{ "Bip01 LUpArmTwistBone", "Bip01 L UpperArm" }, { "Bip01 RUpArmTwistBone", "Bip01 R UpperArm" },
	{ "Bip01 L Thigh", "Bip01 Pelvis" }, { "Bip01 R Thigh", "Bip01 Pelvis" }, { "Bip01 L Calf", "Bip01 L Thigh" }, { "Bip01 R Calf", "Bip01 R Thigh" },
	{ "Bip01 L Foot", "Bip01 L Calf" }, { "Bip01 R Foot", "Bip01 R Calf" }, { "Bip01 L Toe0", "Bip01 L Foot" }, { "Bip01 R Toe0", "Bip01 R Foot" },
};

static const char* ParentOf(const std::string& name)
{
	for (auto& p : kParents) if (name == p[0]) return p[1];
	return "Bip01";
}

static void BuildSkeleton(const nif::File& f, Skeleton& sk)
{
	sk.actor = NewNode("TestActor");
	sk.bip01 = NewNode("Bip01");
	AddChild(sk.actor, sk.bip01);
	sk.bones["Bip01"] = sk.bip01;
	sk.bind["Bip01"] = RE::NiTransform();
	// World bind of every bone any shape is skinned to: the inverse of its skin-to-bone (skin space
	// is the NIF's root space, which the actor's root sits at).
	for (size_t blk = 0; blk < f.blocks.size(); blk++) {
		const std::string& t = f.typeOf(blk);
		if (t != "NiSkinInstance" && t != "BSDismemberSkinInstance") continue;
		nif::SkinInstance si;
		si.read(f.blocks[blk], t == "BSDismemberSkinInstance");
		nif::SkinData sd;
		sd.read(f.blocks[si.data]);
		for (size_t i = 0; i < si.bones.size(); i++) {
			std::string name = StrName(f, si.bones[i]);
			if (name.rfind("Bip01", 0) != 0 || sk.bind.count(name)) continue;
			sk.bind[name] = ToRE(sd.skinTransform.inverse() * sd.bones[i].skinToBone.inverse());
		}
	}
	// Parents first, so every local is relative to a placed parent.
	std::function<RE::NiNode*(const std::string&)> make = [&](const std::string& name) -> RE::NiNode* {
		if (auto it = sk.bones.find(name); it != sk.bones.end()) return it->second;
		std::string parentName = ParentOf(name);
		RE::NiNode* parent = sk.bind.count(parentName) ? make(parentName) : sk.bip01;
		auto* node = NewNode(name);
		AddChild(parent, node);
		node->world = sk.bind[name];
		node->local = parent->world.Invert() * node->world;
		sk.bones[name] = node;
		return node;
	};
	for (auto& [name, xf] : sk.bind) make(name);
}

// --- the item -----------------------------------------------------------------------------------------

struct Shape
{
	RE::NiGeometry* geometry;
	std::vector<std::string> boneNames;
};

static RE::NiGeometry* BuildShape(const nif::File& f, int blk, Skeleton& sk, std::vector<std::string>& boneNames)
{
	nif::Geometry g;
	g.read(f.blocks[blk]);
	bool strips = f.typeOf(g.data) == "NiTriStripsData";
	nif::GeometryData gd;
	gd.read(f.blocks[g.data], strips);
	auto* geom = Alloc<RE::NiGeometry>(0xC4, gGeomVtbl);
	geom->m_name = _strdup(StrName(f, blk).c_str());
	geom->local = RE::NiTransform();
	geom->world = RE::NiTransform();
	for (int e : g.av.extraData) {
		if (f.typeOf(e) != "NiStringExtraData") continue;
		nif::Reader r(f.blocks[e].data(), f.blocks[e].size());
		uint32_t n = r.u32(), v = r.u32();
		AddStringExtra(geom, f.strings[n].c_str(), f.strings[v]);
	}

	auto* data = Alloc<RE::NiTriShapeData>(0x58, gOtherVtbl);
	data->vertexCount = (uint16_t)gd.vertices.size();
	data->vertices = (RE::NiPoint3*)malloc(sizeof(RE::NiPoint3) * gd.vertices.size());
	for (size_t i = 0; i < gd.vertices.size(); i++) data->vertices[i] = { gd.vertices[i].x, gd.vertices[i].y, gd.vertices[i].z };
	data->triangleCount = (uint16_t)gd.triangles.size();
	data->triListLength = (uint32_t)gd.triangles.size() * 3;
	data->triList = (uint16_t*)malloc(sizeof(uint16_t) * data->triListLength);
	for (size_t t = 0; t < gd.triangles.size(); t++) for (int k = 0; k < 3; k++) data->triList[t * 3 + k] = gd.triangles[t].v[k];
	geom->data = data;

	if (g.skin < 0) return geom;
	nif::SkinInstance si;
	si.read(f.blocks[g.skin], f.typeOf(g.skin) == "BSDismemberSkinInstance");
	nif::SkinData sd;
	sd.read(f.blocks[si.data]);
	nif::SkinPartition sp;
	sp.read(f.blocks[si.partition]);

	auto* skin = Alloc<RE::NiSkinInstance>(0x34, gOtherVtbl);
	auto* skinData = Alloc<RE::NiSkinData>(0x48, gOtherVtbl);
	skinData->rootParentToSkin = ToRE(sd.skinTransform);
	skinData->boneCount = (uint32_t)sd.bones.size();
	skinData->boneData = (RE::NiSkinData::BoneData*)calloc(sd.bones.size(), sizeof(RE::NiSkinData::BoneData));
	for (size_t b = 0; b < sd.bones.size(); b++) {
		auto& bd = skinData->boneData[b];
		bd.skinToBone = ToRE(sd.bones[b].skinToBone);
		bd.bound = { { sd.bones[b].center.x, sd.bones[b].center.y, sd.bones[b].center.z }, sd.bones[b].radius };
	}
	skin->skinData = skinData;
	skin->boneCount = (uint32_t)si.bones.size();
	skin->bones = (RE::NiAVObject**)calloc(si.bones.size(), sizeof(void*));
	// The game's attach: slots of skeleton bones point at them, the rest at the skeleton root.
	for (size_t i = 0; i < si.bones.size(); i++) {
		std::string name = StrName(f, si.bones[i]);
		boneNames.push_back(name);
		auto it = sk.bones.find(name);
		skin->bones[i] = it != sk.bones.end() ? (RE::NiAVObject*)it->second : (RE::NiAVObject*)sk.bip01;
	}

	// NiSkinPartition: count, then 0x2C-byte partition records.
	struct PartitionNV { void** vtbl; volatile long ref; uint32_t count; uint8_t* parts; };
	auto* part = (PartitionNV*)calloc(1, sizeof(PartitionNV));
	part->vtbl = gOtherVtbl;
	part->count = (uint32_t)sp.parts.size();
	part->parts = (uint8_t*)calloc(sp.parts.size(), 0x2C);
	for (size_t i = 0; i < sp.parts.size(); i++) {
		auto& p = sp.parts[i];
		uint8_t* rec = part->parts + i * 0x2C;
		auto copy16 = [](const std::vector<uint16_t>& v) { auto* a = (uint16_t*)malloc(v.size() * 2 + 2); memcpy(a, v.data(), v.size() * 2); return a; };
		*(uint16_t**)(rec + 0x04) = copy16(p.bones);
		auto* w = (float*)malloc(p.weights.size() * 4 + 4);
		memcpy(w, p.weights.data(), p.weights.size() * 4);
		*(float**)(rec + 0x08) = w;
		*(uint16_t**)(rec + 0x0C) = copy16(p.vertexMap);
		auto* pal = (uint8_t*)malloc(p.boneIndices.size() + 1);
		memcpy(pal, p.boneIndices.data(), p.boneIndices.size());
		*(uint8_t**)(rec + 0x10) = pal;
		std::vector<uint16_t> tri;
		std::vector<uint16_t> lens;
		if (p.strips.empty()) for (auto& t : p.triangles) { tri.push_back(t.v[0]); tri.push_back(t.v[1]); tri.push_back(t.v[2]); }
		else for (auto& s : p.strips) { lens.push_back((uint16_t)s.size()); tri.insert(tri.end(), s.begin(), s.end()); }
		*(uint16_t**)(rec + 0x14) = copy16(tri);
		*(uint16_t**)(rec + 0x18) = lens.empty() ? nullptr : copy16(lens);
		*(uint16_t*)(rec + 0x1C) = (uint16_t)p.vertexMap.size();
		*(uint16_t*)(rec + 0x1E) = (uint16_t)p.triangles.size();
		*(uint16_t*)(rec + 0x20) = (uint16_t)p.bones.size();
		*(uint16_t*)(rec + 0x22) = (uint16_t)p.strips.size();
		*(uint16_t*)(rec + 0x24) = p.numWeightsPerVertex;
	}
	skin->skinPartition = part;
	geom->skinInstance = skin;
	return geom;
}

// A vertex in the world as the game skins it: sum of weight * bone world * skin-to-bone * vertex.
static RE::NiPoint3 SkinVertex(RE::NiGeometry* g, int v, const std::vector<std::vector<std::pair<int, float>>>& weights)
{
	RE::NiPoint3 out{};
	for (auto& [slot, w] : weights[v]) {
		auto* b = g->skinInstance->bones[slot];
		RE::NiPoint3 p = (b->world * g->skinInstance->skinData->boneData[slot].skinToBone) * g->data->vertices[v];
		out = out + p * w;
	}
	return out;
}

static std::vector<std::vector<std::pair<int, float>>> WeightsOf(RE::NiGeometry* g)
{
	std::vector<std::vector<std::pair<int, float>>> w(g->data->vertexCount);
	struct PartitionNV { void** vtbl; volatile long ref; uint32_t count; uint8_t* parts; };
	auto* sp = (PartitionNV*)g->skinInstance->skinPartition;
	for (uint32_t i = 0; i < sp->count; i++) {
		uint8_t* rec = sp->parts + i * 0x2C;
		auto* bones = *(uint16_t**)(rec + 0x04);
		auto* ws = *(float**)(rec + 0x08);
		auto* map = *(uint16_t**)(rec + 0x0C);
		auto* pal = *(uint8_t**)(rec + 0x10);
		uint16_t nv = *(uint16_t*)(rec + 0x1C), per = *(uint16_t*)(rec + 0x24);
		for (uint16_t v = 0; v < nv; v++) {
			if (!w[map[v]].empty()) continue;
			for (uint16_t k = 0; k < per; k++) if (ws[v * per + k] > 0) w[map[v]].push_back({ bones[pal[v * per + k]], ws[v * per + k] });
		}
	}
	return w;
}

static RE::NiMatrix3 RotX(float a)
{
	RE::NiMatrix3 m;
	m.entry[1][1] = cosf(a); m.entry[1][2] = -sinf(a);
	m.entry[2][1] = sinf(a); m.entry[2][2] = cosf(a);
	return m;
}

static RE::NiMatrix3 RotZ(float a)
{
	RE::NiMatrix3 m;
	m.entry[0][0] = cosf(a); m.entry[0][1] = -sinf(a);
	m.entry[1][0] = sinf(a); m.entry[1][1] = cosf(a);
	return m;
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		printf("usage: systemtest item.nif physics.xml [frames]\n");
		return 1;
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	int frames = argc > 3 ? atoi(argv[3]) : 600;
	hdt::gXmlPath = argv[2];
	InitVtables();
	try {
		nif::File f;
		f.load(argv[1]);
		Skeleton sk;
		BuildSkeleton(f, sk);
		printf("skeleton: %zu bones\n", sk.bones.size());

		// The item's root under the actor, its shapes under it, as the game attaches armor.
		auto* model = NewNode("Scene Root");
		AddChild(sk.actor, model);
		{
			nif::AVObject root;
			nif::Reader r(f.blocks[f.roots[0]].data(), f.blocks[f.roots[0]].size());
			root.read(r, true);
			for (int e : root.extraData) {
				nif::Reader er(f.blocks[e].data(), f.blocks[e].size());
				uint32_t n = er.u32(), v = er.u32();
				AddStringExtra(model, f.strings[n].c_str(), f.strings[v]);
			}
		}
		std::map<std::string, RE::NiGeometry*> shapes;
		for (size_t blk = 0; blk < f.blocks.size(); blk++) {
			const std::string& t = f.typeOf(blk);
			if (t != "NiTriShape" && t != "NiTriStrips") continue;
			std::vector<std::string> names;
			auto* g = BuildShape(f, (int)blk, sk, names);
			AddChild(model, g);
			shapes[g->m_name] = g;
		}
		hdt::updateTransformUpDown(sk.actor);

		// Build, as the actor manager does.
		hdt::DefaultBBP::PhysicsFile_t file{ "physics.xml", {} };
		hdt::SkyrimSystemCreator creator;
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
		if (coat) for (int v = 0; v < coat->data->vertexCount; v++) for (auto& [slot, w] : coatWeights[v]) if (std::string(coat->skinInstance->bones[slot]->m_name).rfind("SMP Coat", 0) == 0 && w > 0.3f) { skirt.push_back(v); break; }
		printf("%zu skirt vertices on simulated bones\n", skirt.size());

		auto* pelvis = sk.bones["Bip01 Pelvis"];
		auto* lThigh = sk.bones["Bip01 L Thigh"], * rThigh = sk.bones["Bip01 R Thigh"];
		auto* lCalf = sk.bones["Bip01 L Calf"], * rCalf = sk.bones["Bip01 R Calf"];
		RE::NiTransform lThigh0 = lThigh->local, rThigh0 = rThigh->local, lCalf0 = lCalf->local, rCalf0 = rCalf->local;
		hdt::SkyrimBone* hem = nullptr;
		for (auto& b : system->getBones()) if (b->m_name == RE::BSFixedString("SMP Coat C05 R4")) hem = static_cast<hdt::SkyrimBone*>(b.get());

		const float dt = 1.0f / 60.0f;
		float maxSwing = 0, minClear = 1e9f, sumClear = 0;
		int clearSamples = 0;
		bool bad = false;
		const bool still = getenv("SYSTEMTEST_STILL") != nullptr;
		RE::NiPoint3 hemRest;   // the hem in the pelvis' frame at rest
		float swingByPhase[4] = {};
		const char* phaseNames[4] = { "standing", "walking", "turning", "stopped" };
		float bob = 0;
		for (int frame = 0; frame < frames; frame++) {
			float t = frame * dt;
			// Stand for a second; walk at 150 units/s (thighs +/-28 deg at 1 Hz, calves bending on the back
			// swing, the pelvis bobbing 2 units twice a step); at 4 s turn 90 degrees in half a second; at
			// 7 s stop dead.
			int phaseId = still ? 0 : t < 1.0f ? 0 : t < 4.0f ? 1 : t < 4.5f ? 2 : t < 7.0f ? 1 : 3;
			float walk = (phaseId == 1 || phaseId == 2) ? 1.0f : 0.0f;
			float phase = t * 2.0f * 3.14159f;
			float turn = (still || t < 4.0f) ? 0.0f : std::min((t - 4.0f) / 0.5f, 1.0f) * 1.5708f;
			sk.actor->local.rotate = RotZ(turn);
			float newBob = 2.0f * sinf(phase * 2.0f) * walk;
			sk.actor->local.translate = sk.actor->local.translate + (sk.actor->local.rotate * RE::NiPoint3(0, 150.0f * dt * walk, newBob - bob));
			bob = newBob;
			float swing = 0.49f * sinf(phase) * walk;
			lThigh->local = lThigh0; rThigh->local = rThigh0; lCalf->local = lCalf0; rCalf->local = rCalf0;
			lThigh->local.rotate = lThigh0.rotate * RotX(swing);
			rThigh->local.rotate = rThigh0.rotate * RotX(-swing);
			lCalf->local.rotate = lCalf0.rotate * RotX(std::max(0.0f, -swing) * 1.2f);
			rCalf->local.rotate = rCalf0.rotate * RotX(std::max(0.0f, swing) * 1.2f);
			sk.actor->world = sk.actor->local;
			hdt::updateTransformUpDown(sk.bip01);
			hdt::updateTransformUpDown(model);

			world->doUpdate(dt);

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
					sumClear += best;
					clearSamples++;
				}
			}
		}
		printf("hem swing from rest, most: standing %.1f, walking %.1f, turning %.1f, after the stop %.1f units\n",
			swingByPhase[0], swingByPhase[1], swingByPhase[2], swingByPhase[3]);
		if (clearSamples) printf("skirt to legs: closest %.2f units, average %.2f units\n", minClear, sumClear / clearSamples);
		printf("step time %.3f ms\n", world->m_averageStepMs);
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
