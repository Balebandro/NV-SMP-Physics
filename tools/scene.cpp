#include "scene.h"

#include <fstream>

// --- fake objects with the game's layout -------------------------------------------------------------

namespace
{
	void* __fastcall ReturnSelf(void* self, void*) { return self; }
	void* __fastcall ReturnNull(void*, void*) { return nullptr; }
	void __fastcall DeleteNothing(void*, void*) {}

	void* gNodeVtbl[16], *gGeomVtbl[16], *gOtherVtbl[16];
	void InitVtables()
	{
		static bool done = false;
		if (done) return;
		done = true;
		for (int i = 0; i < 16; i++) gNodeVtbl[i] = gGeomVtbl[i] = gOtherVtbl[i] = (void*)ReturnNull;
		gNodeVtbl[1] = gGeomVtbl[1] = gOtherVtbl[1] = (void*)DeleteNothing;
		gNodeVtbl[3] = (void*)ReturnSelf;   // IsNode
		gGeomVtbl[6] = (void*)ReturnSelf;   // IsGeometry
	}

	template <class T>
	T* Alloc(size_t size, void** vtbl)
	{
		auto* p = (T*)calloc(1, std::max(size, sizeof(T)));
		((RE::NiObject*)p)->vtbl = vtbl;
		((RE::NiObject*)p)->refCount = 1000;   // never freed
		return p;
	}

	RE::NiNode* NewNode(const std::string& name)
	{
		auto* n = Alloc<RE::NiNode>(0xAC, gNodeVtbl);
		n->m_name = _strdup(name.c_str());
		n->local = RE::NiTransform();   // identity: calloc left the rotations all zero
		n->world = RE::NiTransform();
		return n;
	}

	void AddChild(RE::NiNode* parent, RE::NiAVObject* child)
	{
		auto** arr = (RE::NiAVObject**)realloc(parent->children, sizeof(void*) * (parent->childFirstFree + 1));
		parent->children = arr;
		arr[parent->childFirstFree++] = child;
		parent->childCount = parent->childFirstFree;
		child->parent = parent;
	}

	void AddStringExtra(RE::NiObjectNET* obj, const char* name, const std::string& value)
	{
		auto* e = Alloc<RE::NiStringExtraData>(0x10, gOtherVtbl);
		e->name = _strdup(name);
		e->value = _strdup(value.c_str());
		obj->extraData = (RE::NiExtraData**)realloc(obj->extraData, sizeof(void*) * (obj->extraDataCount + 1));
		obj->extraData[obj->extraDataCount++] = e;
	}

	RE::NiTransform ToRE(const nif::Transform& t)
	{
		RE::NiTransform r;
		for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.rotate.entry[i][j] = t.rot.m[i][j];
		r.translate = { t.trans.x, t.trans.y, t.trans.z };
		r.scale = t.scale;
		return r;
	}

	std::string StrName(const nif::File& f, int block)
	{
		nif::Reader r(f.blocks[block].data(), f.blocks[block].size());
		uint32_t s = r.u32();
		return s < f.strings.size() ? f.strings[s] : "";
	}

	// --- the skeleton -------------------------------------------------------------------------------

	const char* kParents[][2] = {
		{ "Bip01 Pelvis", "Bip01" }, { "Bip01 Spine", "Bip01 Pelvis" }, { "Bip01 Spine1", "Bip01 Spine" }, { "Bip01 Spine2", "Bip01 Spine1" },
		{ "Bip01 Neck1", "Bip01 Spine2" }, { "Bip01 Head", "Bip01 Neck1" }, { "Bip01 L Clavicle", "Bip01 Spine2" }, { "Bip01 R Clavicle", "Bip01 Spine2" },
		{ "Bip01 L UpperArm", "Bip01 L Clavicle" }, { "Bip01 R UpperArm", "Bip01 R Clavicle" },
		{ "Bip01 LUpArmTwistBone", "Bip01 L UpperArm" }, { "Bip01 RUpArmTwistBone", "Bip01 R UpperArm" },
		{ "Bip01 L Forearm", "Bip01 L UpperArm" }, { "Bip01 R Forearm", "Bip01 R UpperArm" },
		{ "Bip01 L ForeTwist", "Bip01 L Forearm" }, { "Bip01 R ForeTwist", "Bip01 R Forearm" },
		{ "Bip01 L Hand", "Bip01 L Forearm" }, { "Bip01 R Hand", "Bip01 R Forearm" },
		{ "Bip01 L Thumb1", "Bip01 L Hand" }, { "Bip01 R Thumb1", "Bip01 R Hand" },
		{ "Bip01 L Thigh", "Bip01 Pelvis" }, { "Bip01 R Thigh", "Bip01 Pelvis" }, { "Bip01 L Calf", "Bip01 L Thigh" }, { "Bip01 R Calf", "Bip01 R Thigh" },
		{ "Bip01 L Foot", "Bip01 L Calf" }, { "Bip01 R Foot", "Bip01 R Calf" }, { "Bip01 L Toe0", "Bip01 L Foot" }, { "Bip01 R Toe0", "Bip01 R Foot" },
	};

	const char* ParentOf(const std::string& name)
	{
		for (auto& p : kParents) if (name == p[0]) return p[1];
		return "Bip01";
	}

	void BuildSkeleton(const nif::File& f, scene::Skeleton& sk)
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

	// The game's own skeleton (skeleton.nif): every node, with its parent and rest transform, under a
	// root as the actor's 3D has it.
	void BuildSkeletonFromNif(const std::string& path, scene::Skeleton& sk)
	{
		nif::File f;
		f.load(path);
		std::function<void(int, RE::NiNode*)> walk = [&](int blk, RE::NiNode* parent) {
			if (blk < 0 || f.typeOf(blk).find("Node") == std::string::npos) return;
			nif::Reader r(f.blocks[blk].data(), f.blocks[blk].size());
			nif::AVObject av;
			av.read(r, true);
			std::string name = av.name < f.strings.size() ? f.strings[av.name] : std::string();
			RE::NiNode* node;
			if (!parent) {
				node = sk.actor = NewNode(name.empty() ? "Skeleton" : name);
			} else {
				node = NewNode(name);
				AddChild(parent, node);
			}
			node->local = ToRE(av.local);
			if (!name.empty() && !sk.bones.count(name)) sk.bones[name] = node;
			for (int c : av.children) walk(c, node);
		};
		walk(f.roots[0], nullptr);
		sk.bip01 = sk.bones.count("Bip01") ? sk.bones["Bip01"] : sk.actor;
		sk.actor->world = sk.actor->local;
		hdt::updateTransformUpDown(sk.actor);
		for (auto& [name, node] : sk.bones) sk.bind[name] = node->world;
	}

	// The diffuse texture a shape's BSShaderPPLightingProperty names (its texture set's first slot).
	std::string DiffuseOf(const nif::File& f, const nif::Geometry& g)
	{
		for (int p : g.av.properties) {
			if (p < 0 || f.typeOf(p) != "BSShaderPPLightingProperty") continue;
			nif::Reader r(f.blocks[p].data(), f.blocks[p].size());
			r.u32();                                   // name
			uint32_t extra = r.u32();
			r.skip(extra * 4);
			r.i32();                                   // controller
			r.u16();                                   // flags
			r.u32(); r.u32(); r.u32();                 // shader type, shader flags 1 and 2
			r.f32();                                   // environment map scale
			r.u32();                                   // texture clamp mode
			int32_t set = r.i32();
			if (set < 0 || f.typeOf(set) != "BSShaderTextureSet") continue;
			nif::Reader s(f.blocks[set].data(), f.blocks[set].size());
			if (s.i32() < 1) continue;
			uint32_t len = s.u32();
			return std::string((const char*)f.blocks[set].data() + s.pos, len);
		}
		return {};
	}

	bool HasAlpha(const nif::File& f, const nif::Geometry& g)
	{
		for (int p : g.av.properties)
			if (p >= 0 && f.typeOf(p) == "NiAlphaProperty") return true;
		return false;
	}

	RE::NiGeometry* BuildShape(const nif::File& f, int blk, scene::Skeleton& sk, scene::ShapeInfo& info)
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
		info.normals = gd.normals;
		for (size_t i = 0; i + 1 < gd.uvs.size(); i += 2) info.uvs.push_back({ gd.uvs[i], gd.uvs[i + 1] });
		info.diffuse = DiffuseOf(f, g);
		info.alpha = HasAlpha(f, g);

		auto* data = Alloc<RE::NiTriShapeData>(0x58, gOtherVtbl);
		data->vertexCount = (uint16_t)gd.vertices.size();
		data->vertices = (RE::NiPoint3*)malloc(sizeof(RE::NiPoint3) * gd.vertices.size());
		for (size_t i = 0; i < gd.vertices.size(); i++) data->vertices[i] = { gd.vertices[i].x, gd.vertices[i].y, gd.vertices[i].z };
		data->triangleCount = (uint16_t)gd.triangles.size();
		data->triListLength = (uint32_t)gd.triangles.size() * 3;
		data->triList = (uint16_t*)malloc(sizeof(uint16_t) * data->triListLength + 2);
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
			auto p = sp.parts[i];
			// What an exporter may leave out (3ds Max's, for one), filled in as the game reads it: no vertex
			// map, the partition's vertices are the shape's in order; no bone indices, a vertex's weights
			// follow the partition's bones in order; no weights, the first bone takes it all. Indices out
			// of range (corrupt data) are clamped rather than read past the arrays.
			const uint16_t per = std::max<uint16_t>(1, p.numWeightsPerVertex);
			const uint16_t nb = (uint16_t)p.bones.size();
			if (p.vertexMap.empty()) { p.vertexMap.resize(p.numVertices); for (uint16_t v = 0; v < p.numVertices; v++) p.vertexMap[v] = v; }
			const size_t nv = p.vertexMap.size();
			if (p.boneIndices.size() < nv * per) { p.boneIndices.assign(nv * per, 0); for (size_t v = 0; v < nv; v++) for (uint16_t k = 0; k < per; k++) p.boneIndices[v * per + k] = (uint8_t)std::min<int>(k, std::max(0, nb - 1)); }
			if (p.weights.size() < nv * per) { p.weights.assign(nv * per, 0.0f); for (size_t v = 0; v < nv; v++) p.weights[v * per] = 1.0f; }
			for (auto& b : p.boneIndices) if (nb && b >= nb) b = 0;
			for (auto& b : p.bones) if (b >= si.bones.size()) b = 0;
			for (auto& m : p.vertexMap) if (m >= gd.vertices.size()) m = 0;
			p.numWeightsPerVertex = per;
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
}

namespace scene
{
	std::function<std::string(const char* path)> gReadFile;

	Item Load(const std::string& nifPath, const std::string& skeletonPath)
	{
		InitVtables();
		nif::File f;
		f.load(nifPath);
		Item item;
		if (!skeletonPath.empty()) BuildSkeletonFromNif(skeletonPath, item.skeleton);
		else BuildSkeleton(f, item.skeleton);

		// The item's root under the actor, its shapes under it, as the game attaches armor.
		item.model = NewNode("Scene Root");
		AddChild(item.skeleton.actor, item.model);
		{
			nif::AVObject root;
			nif::Reader r(f.blocks[f.roots[0]].data(), f.blocks[f.roots[0]].size());
			root.read(r, true);
			for (int e : root.extraData) {
				if (f.typeOf(e) != "NiStringExtraData") continue;
				nif::Reader er(f.blocks[e].data(), f.blocks[e].size());
				uint32_t n = er.u32(), v = er.u32();
				AddStringExtra(item.model, f.strings[n].c_str(), f.strings[v]);
			}
		}
		for (size_t blk = 0; blk < f.blocks.size(); blk++) {
			const std::string& t = f.typeOf(blk);
			if (t != "NiTriShape" && t != "NiTriStrips") continue;
			ShapeInfo info;
			info.geometry = BuildShape(f, (int)blk, item.skeleton, info);
			AddChild(item.model, info.geometry);
			item.byName[info.geometry->m_name] = info.geometry;
			item.shapes.push_back(std::move(info));
		}
		hdt::updateTransformUpDown(item.skeleton.actor);
		if (const char* named = item.model->FindStringExtra("HDT Skinned Mesh Physics Object"))
			item.physicsFile = named;
		return item;
	}

	RE::NiPoint3 SkinVertex(RE::NiGeometry* g, int v, const Weights& weights)
	{
		RE::NiPoint3 out{};
		for (auto& [slot, w] : weights[v]) {
			auto* b = g->skinInstance->bones[slot];
			RE::NiPoint3 p = (b->world * g->skinInstance->skinData->boneData[slot].skinToBone) * g->data->vertices[v];
			out = out + p * w;
		}
		return out;
	}

	RE::NiPoint3 SkinNormal(RE::NiGeometry* g, const nif::Vec3& n, int v, const Weights& weights)
	{
		RE::NiPoint3 out{};
		RE::NiPoint3 nn{ n.x, n.y, n.z };
		for (auto& [slot, w] : weights[v]) {
			auto* b = g->skinInstance->bones[slot];
			RE::NiTransform xf = b->world * g->skinInstance->skinData->boneData[slot].skinToBone;
			out = out + (xf.rotate * nn) * w;
		}
		float l = out.Length();
		return l > 1e-6f ? out * (1.0f / l) : RE::NiPoint3{ 0, 0, 1 };
	}

	Weights WeightsOf(RE::NiGeometry* g)
	{
		Weights w(g->data->vertexCount);
		if (!g->skinInstance) return w;
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

	RE::NiMatrix3 RotX(float a)
	{
		RE::NiMatrix3 m;
		m.entry[1][1] = cosf(a); m.entry[1][2] = -sinf(a);
		m.entry[2][1] = sinf(a); m.entry[2][2] = cosf(a);
		return m;
	}

	RE::NiMatrix3 RotY(float a)
	{
		RE::NiMatrix3 m;
		m.entry[0][0] = cosf(a); m.entry[0][2] = sinf(a);
		m.entry[2][0] = -sinf(a); m.entry[2][2] = cosf(a);
		return m;
	}

	RE::NiMatrix3 RotZ(float a)
	{
		RE::NiMatrix3 m;
		m.entry[0][0] = cosf(a); m.entry[0][1] = -sinf(a);
		m.entry[1][0] = sinf(a); m.entry[1][1] = cosf(a);
		return m;
	}
}

namespace hdt
{
	RE::NiNode* createNode(const char* name) { return NewNode(name); }
	std::string readAllFile(const char* path) { return scene::gReadFile ? scene::gReadFile(path) : std::string(); }
}
