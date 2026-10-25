#include "ActorManager.h"
#include "Game.h"
#include "Settings.h"
#include "hdtSkyrimPhysicsWorld.h"

#include <algorithm>
#include <unordered_set>

namespace nvsmp
{
	static const char* kPhysicsKey = "HDT Skinned Mesh Physics Object";

	// smpdress --proxy collision stand-ins (shapes named "SMP Proxy:...") are hidden (app-culled, NiAVObject flags at
	// 0x30 bit 0) when their physics is built (generateMeshBody). Culled before the game binds the item's skin, a
	// stand-in loses its collisions, and the game does not bind the skin again after a re-equip of the same model: so
	// they are shown again whenever the item's physics goes (the model may be attached again as it is). The player's
	// first-person model gets no physics: hidden every frame.
	static void SetProxiesHidden(RE::NiAVObject* obj, bool hidden, int depth = 0)
	{
		if (!obj || depth > 48)
			return;
		if (auto* node = obj->AsNode()) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				SetProxiesHidden(node->children[i], hidden, depth + 1);
			return;
		}
		if (!obj->m_name || _strnicmp(obj->m_name, "SMP Proxy:", 10) != 0)
			return;
		auto& flags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + 0x30);
		flags = hidden ? (flags | 1) : (flags & ~1u);
	}

	// The game drops an item's own bone nodes from the scene when it attaches the item, but until it binds the item's
	// skin to the skeleton (after the Pip-Boy closes, seconds later) the skin still points at them. Nothing places them
	// any more: they sit at the world origin, or hold garbage. So the item was drawn at the origin (invisible to the
	// player), and a garbage one gave it an infinite bound, which took the whole player with it. Those nodes may be
	// freed, or their memory reused by something else (writing them crashed the game), so they are never written:
	// while the plugin waits for the bind, each slot not on a live skeleton bone gets a node of the plugin's own with
	// the same name, put at its rest pose every frame -- onto the skeleton bone of that name, or at its bind offset
	// from one (as SkyrimSystemCreator::createExtraBoneNode places its nodes), a slot already on a live skeleton bone
	// preferred as that one. The game's bind overwrites the slots as it did the old pointers.

	// The plugin's own stand-ins for stale skin slots during a bind wait. Never released: the game may still hold them
	// in a skin's slots after the plugin lets go of the item (a few hundred bytes a node).
	static std::vector<RE::NiPointer<RE::NiNode>> g_waitNodes;
	static std::unordered_set<const RE::NiAVObject*> g_waitSet;

	// A skin slot's node, under SEH (a slot left from an earlier attach may point at a freed object): null if unreadable.
	static RE::NiAVObject* ReadSlot(RE::NiAVObject** bones, uint32_t i)
	{
		__try {
			return bones[i];
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	// The node's name into `out`, under SEH: false if unreadable or unnamed.
	static bool CopyName(RE::NiAVObject* node, char* out, size_t size)
	{
		__try {
			const char* n = node->m_name;
			if (!n) return false;
			size_t i = 0;
			for (; i + 1 < size && n[i]; ++i) out[i] = n[i];
			out[i] = 0;
			return i > 0;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Whether the node is a live skeleton bone: under `actor`, outside `model` (the item's subtree). Under SEH, and at
	// most 256 parents up: a stale slot can point at reused memory whose parent chain loops, which hung the main thread
	// here on a save load (2026-10-23, three stack snapshots in this loop); such a slot is not a live bone.
	static bool IsLiveBone(RE::NiAVObject* node, RE::NiAVObject* actor, RE::NiAVObject* model)
	{
		__try {
			int depth = 0;
			for (RE::NiAVObject* p = node; p && depth < 256; p = p->parent, ++depth) {
				if (p == model) return false;
				if (p == actor) return true;
			}
			return false;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// A node by name under `obj`, outside `skip` (the item's subtree).
	static RE::NiAVObject* FindOutside(RE::NiAVObject* obj, RE::NiAVObject* skip, const char* name, int depth = 0)
	{
		if (!obj || obj == skip || depth > 64)
			return nullptr;
		if (obj->m_name && !_stricmp(obj->m_name, name))
			return obj;
		if (auto* node = obj->AsNode())
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				if (auto* found = FindOutside(node->children[i], skip, name, depth + 1))
					return found;
		return nullptr;
	}

	static void PlaceDroppedBones(RE::NiAVObject* obj, RE::NiNode* actor, RE::NiAVObject* model, int depth = 0)
	{
		if (!obj || depth > 64)
			return;
		if (auto* g = hdt::castNiGeometry(obj)) {
			auto* skin = g->skinInstance;
			if (!g->FindStringExtra("SMP Bones") || !skin || !skin->bones || !skin->skinData || !skin->skinData->boneData)
				return;
			const uint32_t n = skin->skinData->boneCount;
			std::vector<RE::NiAVObject*> nodes(n, nullptr), skeleton(n, nullptr);
			int reference = -1;
			bool liveReference = false;
			for (uint32_t i = 0; i < n; ++i) {
				RE::NiAVObject* slot = ReadSlot(skin->bones, i);
				if (!slot) continue;
				char name[128];
				const bool named = CopyName(slot, name, sizeof(name));
				if (!g_waitSet.count(slot)) {
					// A weapon keeps its own nodes in the scene (only skinned armor has them dropped): one still under the
					// actor is placed by the game, as a skeleton bone is.
					if (IsLiveBone(slot, actor, nullptr)) {
						skeleton[i] = slot;   // the best reference, left as it is
						if (!liveReference) { reference = (int)i; liveReference = true; }
						continue;
					}
					RE::NiPointer<RE::NiNode> own(hdt::createNode(named ? name : "SMP Wait"));
					if (!own) continue;
					g_waitNodes.push_back(own);
					g_waitSet.insert(own.get());
					skin->bones[i] = own.get();
					slot = own.get();
				}
				nodes[i] = slot;
				if (named)
					if (RE::NiAVObject* bone = FindOutside(actor, model, name)) {
						skeleton[i] = bone;
						if (reference < 0) reference = (int)i;
					}
			}
			if (reference < 0)
				return;   // no skeleton bone to place them from: they stay at the origin (finite, so no broken bound)
			const auto* data = skin->skinData->boneData;
			const RE::NiTransform skinToWorld = skeleton[reference]->world * data[reference].skinToBone;
			for (uint32_t i = 0; i < n; ++i)
				if (nodes[i])
					nodes[i]->world = skeleton[i] ? skeleton[i]->world : skinToWorld * data[i].skinToBone.Invert();
			return;
		}
		if (auto* node = obj->AsNode())
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				PlaceDroppedBones(node->children[i], actor, model, depth + 1);
	}

	// The race menu's face with a race head's bones (see RaceHead): no physics there, and the slots of the head's own bones hold
	// the game's nodes for the NIF's, never placed (at the world origin: the nose was drawn stretched down to it). Each frame
	// they are put on the menu's physics bones of their names (`names`: the slots' lower-case names), or else at rest from a
	// skeleton bone of the same skin, in place (the slots keep their nodes, nothing is held).
	static void PlaceFaceBones(RE::NiGeometry* g, RE::NiAVObject* actor, const std::vector<std::string>& names = {},
		const std::unordered_map<std::string, RE::NiAVObject*>* bones = nullptr)
	{
		__try {
			auto* skin = g->skinInstance;
			if (!skin || !skin->bones || !skin->skinData || !skin->skinData->boneData) return;
			const uint32_t n = skin->skinData->boneCount;
			int reference = -1;
			for (uint32_t i = 0; i < n && reference < 0; ++i)
				if (RE::NiAVObject* slot = skin->bones[i]; slot && IsLiveBone(slot, actor, nullptr)) reference = (int)i;
			if (reference < 0) return;
			const auto* data = skin->skinData->boneData;
			const RE::NiTransform skinToWorld = skin->bones[reference]->world * data[reference].skinToBone;
			for (uint32_t i = 0; i < n; ++i)
				if (RE::NiAVObject* slot = skin->bones[i]; slot && !IsLiveBone(slot, actor, nullptr)) {
					RE::NiAVObject* bone = nullptr;
					if (bones && i < names.size())
						if (auto it = bones->find(names[i]); it != bones->end()) bone = it->second;
					slot->world = bone ? bone->world : skinToWorld * data[i].skinToBone.Invert();
				}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	// A safety net for the player: while the root's bound radius is not finite (one broken bone in any skinned item makes
	// it so, and the game then draws nothing of the player), the last good radius is written back so the view test
	// passes. The bound (a pointer at NiAVObject +0x20 in New Vegas) is read under SEH; the game recomputes it each
	// frame, so this holds only while it is needed.
	static bool KeepRadius(RE::NiAVObject* obj, float& lastGood)
	{
		__try {
			auto* b = *reinterpret_cast<RE::NiBound**>(reinterpret_cast<uint8_t*>(obj) + 0x20);
			if (!b || !std::isfinite(b->center.x) || !std::isfinite(b->center.y) || !std::isfinite(b->center.z))
				return false;
			if (std::isfinite(b->radius) && b->radius > 0)
				lastGood = b->radius;
			else if (lastGood > 0)
				b->radius = lastGood;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Race hair. The game's FaceGen loader keeps only a hair NIF's shapes, renamed "FaceGenHair" + the shape's name (under
	// BSFaceGenNiNodeSkinned when skinned; the game binds their skeleton bones), and replaces their extra data with its
	// own: the physics file and the "SMP Bones" lists are gone, and so are the NIF's other shapes. They are read from the
	// hair's NIF instead (the actor's hair form's model), and the system is built on the shapes under their new names. A
	// collider proxy ("SMP Proxy:...") goes with the dropped shapes, so race hair has no collision yet.
	struct RaceHairFile
	{
		std::string physics;                                   // the physics file ("" if the hair has none)
		std::unordered_map<std::string, std::string> lists;    // game shape name, lower case -> its "SMP Bones"
		std::unordered_map<std::string, int> verts;            // and its vertex count, to tell it is the drawn hair
		std::unordered_map<std::string, std::vector<RE::NiPoint3>> base;   // and its vertices in the file (no face morphs)
		std::unordered_map<std::string, std::string> nifName;  // and its name in the NIF (its .egm is <model><name>.egm)
		hdt::DefaultBBP::NameMap_t names;                      // the physics file's shape names -> the game's
		std::unordered_map<std::string, hdt::FileMesh> proxies;   // its collision proxies (the game drops them), lower case
		std::unordered_map<std::string, std::vector<RE::NiPoint3>> normals;   // the shapes' normals and triangles in the file
		std::unordered_map<std::string, std::vector<uint16_t>> triangles;     // (a recycled copy may come without them)
	};

	// The extra data the game strips, from the NIF file (20.2.0.7, Fallout 3 / New Vegas): the root's or any shape's
	// physics file, each shape's "SMP Bones" (and "SMP Bones 2"... continuations) and vertex count.
	static RaceHairFile ParseRaceHair(const std::string& bytes, const std::string& prefix = "FaceGenHair")
	{
		RaceHairFile out;
		const uint8_t* d = (const uint8_t*)bytes.data();
		const size_t n = bytes.size();
		size_t o = 0;
		auto need = [&](size_t k) { if (o + k > n) throw std::runtime_error("short NIF"); };
		auto u8 = [&]() { need(1); return d[o++]; };
		auto u16 = [&]() { need(2); uint16_t v; memcpy(&v, d + o, 2); o += 2; return v; };
		auto u32 = [&]() { need(4); uint32_t v; memcpy(&v, d + o, 4); o += 4; return v; };
		auto sized = [&]() { uint32_t l = u32(); need(l); std::string s((const char*)d + o, l); o += l; return s; };
		auto shortStr = [&]() { uint8_t l = u8(); need(l); o += l; };
		while (o < n && d[o] != '\n') ++o;
		++o;
		const uint32_t version = u32(); u8(); const uint32_t user = u32(); const uint32_t blocks = u32(); u32();
		if (version != 0x14020007 || user != 11) throw std::runtime_error("not a Fallout 3 / New Vegas NIF");
		shortStr(); shortStr(); shortStr();
		std::vector<std::string> types(u16());
		for (auto& t : types) t = sized();
		std::vector<uint16_t> typeOf(blocks);
		for (auto& t : typeOf) t = u16() & 0x7FFF;
		std::vector<uint32_t> sizes(blocks);
		for (auto& s : sizes) s = u32();
		std::vector<std::string> strings(u32());
		u32();
		for (auto& s : strings) s = sized();
		const uint32_t groups = u32();
		o += 4 * groups;
		std::vector<size_t> at(blocks);
		for (uint32_t b = 0; b < blocks; ++b) { at[b] = o; o += sizes[b]; }
		if (o > n) throw std::runtime_error("short NIF");
		auto str = [&](uint32_t i) { return i < strings.size() ? strings[i] : std::string(); };
		auto rd32 = [&](size_t p) { uint32_t v; memcpy(&v, d + p, 4); return v; };
		auto extras = [&](uint32_t b) {   // NiObjectNET: name, extra data refs
			std::vector<std::pair<std::string, std::string>> e;
			const uint32_t count = rd32(at[b] + 4);
			for (uint32_t i = 0; i < count; ++i) {
				const int32_t r = (int32_t)rd32(at[b] + 8 + 4 * i);
				if (r >= 0 && r < (int32_t)blocks && types[typeOf[r]] == "NiStringExtraData") e.push_back({ str(rd32(at[r])), str(rd32(at[r] + 4)) });
			}
			return e;
		};
		for (uint32_t b = 0; b < blocks; ++b) {
			const std::string& t = types[typeOf[b]];
			const bool shape = t == "NiTriShape" || t == "NiTriStrips";
			if (!shape && b != 0) continue;
			std::string list;
			for (auto& [key, value] : extras(b)) {
				if (!_stricmp(key.c_str(), kPhysicsKey) && out.physics.empty()) out.physics = value;
				if (!_stricmp(key.c_str(), "SMP Bones")) list = value + list;
			}
			for (int k = 2; shape && !list.empty(); ++k) {   // "SMP Bones 2", "SMP Bones 3"... in order
				std::string more;
				for (auto& [key, value] : extras(b)) if (!_stricmp(key.c_str(), ("SMP Bones " + std::to_string(k)).c_str())) more = value;
				if (more.empty()) break;
				list += more;
			}
			if (!shape) continue;
			const std::string name = str(rd32(at[b]));
			// NiAVObject after the extra data: controller, flags, transform (52 bytes), properties, collision; then data.
			size_t p = at[b] + 8 + 4 * rd32(at[b] + 4) + 4 + 4 + 52;
			p += 4 + 4 * rd32(p) + 4;
			const int32_t data = (int32_t)rd32(p);
			// A collision proxy: dropped by the game's hair loader, so read here, whole (see hdt::FileMesh).
			if (_strnicmp(name.c_str(), "SMP Proxy:", 10) == 0) {
				const int32_t skinRef = (int32_t)rd32(p + 4);
				if (data < 0 || data >= (int32_t)blocks || skinRef < 0 || skinRef >= (int32_t)blocks) continue;
				hdt::FileMesh m;
				// NiTriShapeData: group id, vertex count, keep/compress flags, vertices, BS flags, normals (+ tangents),
				// bound, colours, UV sets, consistency, additional data, then the triangles.
				size_t q = at[data] + 4;
				uint16_t nv; memcpy(&nv, d + q, 2); q += 4;
				if (d[q++]) { m.vertices.resize(nv); memcpy(m.vertices.data(), d + q, 12 * (size_t)nv); q += 12 * (size_t)nv; }
				uint16_t bsFlags; memcpy(&bsFlags, d + q, 2); q += 2;
				if (d[q++]) { q += 12 * (size_t)nv; if (bsFlags & 4096) q += 24 * (size_t)nv; }
				q += 16;
				if (d[q++]) q += 16 * (size_t)nv;
				if (bsFlags & 1) q += 8 * (size_t)nv;
				q += 2 + 4;
				uint16_t nt; memcpy(&nt, d + q, 2); q += 2 + 4;
				if (d[q++])
					for (uint16_t t = 0; t < nt; ++t) {
						uint16_t i3[3]; memcpy(i3, d + q + 6 * (size_t)t, 6);
						m.triangles.push_back({ i3[0], i3[1], i3[2] });
					}
				// The skin: its data and bones (NiSkinInstance / BSDismemberSkinInstance), then NiSkinData's bind per bone.
				const int32_t skinData = (int32_t)rd32(at[skinRef]);
				const uint32_t boneCount = rd32(at[skinRef] + 12);
				if (skinData < 0 || skinData >= (int32_t)blocks || m.vertices.empty() || m.triangles.empty()) continue;
				size_t s = at[skinData] + 52;
				const uint32_t dataBones = rd32(s); s += 4;
				const bool hasWeights = d[s++] != 0;
				m.weights.resize(m.vertices.size());
				for (uint32_t b = 0; b < dataBones && b < boneCount; ++b) {
					hdt::FileMesh::Bone bone;
					const int32_t node = (int32_t)rd32(at[skinRef] + 16 + 4 * b);
					bone.name = node >= 0 && node < (int32_t)blocks ? str(rd32(at[node])) : std::string();
					memcpy(&bone.skinToBone, d + s, 52); s += 52;
					memcpy(&bone.center, d + s, 12); s += 12;
					memcpy(&bone.radius, d + s, 4); s += 4;
					uint16_t count; memcpy(&count, d + s, 2); s += 2;
					for (uint16_t k = 0; hasWeights && k < count; ++k) {
						uint16_t v; float w;
						memcpy(&v, d + s, 2); memcpy(&w, d + s + 2, 4); s += 6;
						if (v < m.weights.size()) m.weights[v].push_back({ (int)b, w });
					}
					m.bones.push_back(bone);
				}
				std::string lower = name;
				for (auto& c : lower) c = (char)tolower((unsigned char)c);
				out.proxies[lower] = std::move(m);
				continue;
			}
			out.names[name] = { prefix + name };
			if (list.empty()) continue;
			std::string game = prefix + name;
			for (auto& c : game) c = (char)tolower((unsigned char)c);
			out.lists[game] = list;
			out.nifName[game] = name;
			if (data >= 0 && data < (int32_t)blocks) {
				// NiTriShapeData: group id, vertex count, keep and compress flags, has vertices, the vertices.
				uint16_t v; memcpy(&v, d + at[data] + 4, 2); out.verts[game] = v;
				if (d[at[data] + 8] && at[data] + 9 + 12 * (size_t)v <= n) {
					auto& b = out.base[game];
					b.resize(v);
					memcpy(b.data(), d + at[data] + 9, 12 * (size_t)v);
				}
				// Its normals and triangles (laid out as for a proxy above).
				const size_t end = at[data] + sizes[data];
				size_t q = at[data] + 8;
				if (d[q++]) q += 12 * (size_t)v;
				uint16_t bsFlags; memcpy(&bsFlags, d + q, 2); q += 2;
				if (d[q++]) {
					if (q + 12 * (size_t)v <= end) {
						auto& nn = out.normals[game];
						nn.resize(v);
						memcpy(nn.data(), d + q, 12 * (size_t)v);
					}
					q += 12 * (size_t)v;
					if (bsFlags & 4096) q += 24 * (size_t)v;
				}
				q += 16;
				if (q < end && d[q++]) q += 16 * (size_t)v;
				if (bsFlags & 1) q += 8 * (size_t)v;
				q += 2 + 4;
				if (q + 7 <= end) {
					uint16_t nt; memcpy(&nt, d + q, 2); q += 2 + 4;
					if (d[q++] && q + 6 * (size_t)nt <= end) {
						auto& tt = out.triangles[game];
						tt.resize(3 * (size_t)nt);
						memcpy(tt.data(), d + q, 6 * (size_t)nt);
					}
				}
			}
		}
		return out;
	}

	// The game's hat swap for an actor, as the race menu calls it (0x7B2E39): this = 0x7AF430(actor), then 0x6062E0(actor, 0),
	// the 0 for the actor's own 3D. Under SEH: false if it faulted.
	static bool RunHatSwap(void* actor)
	{
		__try {
			void* self = ((void*(__thiscall*)(void*))0x007AF430)(actor);
			((void(__thiscall*)(void*, void*, void*))0x006062E0)(self, actor, nullptr);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Race hair copies drawn in game (built and skinned here), by plain pointer: copies known to draw. When a hair is chosen again
	// in the race menu, an old copy of it known to draw is shown again rather than the game's new one, made from a recycled
	// shape (see the scan): those come unbound, with a stale shader slot for a while, and drew nothing before that was handled.
	static std::unordered_set<const RE::NiAVObject*> g_drawnHair;

	// The race hair files read so far, by model path (each is read once).
	static const RaceHairFile& RaceHair(const char* model)
	{
		static std::unordered_map<std::string, RaceHairFile> cache;
		std::string key = model;
		for (auto& c : key) c = (char)tolower((unsigned char)c);
		if (auto it = cache.find(key); it != cache.end()) return it->second;
		RaceHairFile& f = cache[key];
		const std::string bytes = hdt::readAllFile(("Meshes\\" + std::string(model)).c_str());
		if (bytes.empty()) logger::warn("[SMP] race hair Meshes\\{}: not found", model);
		else {
			try { f = ParseRaceHair(bytes); }
			catch (std::exception& e) { logger::warn("[SMP] race hair Meshes\\{}: {}", model, e.what()); }
			if (!f.physics.empty()) logger::info("[SMP] race hair Meshes\\{}: physics '{}', {} shape(s) with bones", model, f.physics, f.lists.size());
			else logger::info("[SMP] race hair Meshes\\{}: read ({} bytes), no physics", model, bytes.size());
		}
		return f;
	}

	// A head part (a piercing, a scar: BGSHeadPart) is drawn like race hair (the game's FaceGen loader keeps only its NIF's
	// shapes, unskinned under the head's FaceGen node), but every head part's shape is named "FaceGenAccessory": the one
	// whose NIF has physics is told by its vertex count and renamed "FaceGenAccessory <its name in the NIF>" (kHeadPartPrefix),
	// the name its file's game names give it here.
	static constexpr const char* kHeadPartPrefix = "FaceGenAccessory ";
	// The head-part shapes renamed so (kept alive): their FaceGen name back when the race menu opens, which rebuilds the face and
	// tells head parts by that name.
	static std::vector<RE::NiPointer<RE::NiAVObject>> g_renamedHeadParts;
	// The race menu's head-part copies with physics, their skin taken off there (by shape, plain pointers, as g_menuSkins):
	// the game never binds it (its bones are the NIF's own, at the world origin), and without it the shape is drawn at rest
	// (the NIF's Bip01 Head is where the game puts the head part). The copy that remains gets it back when the menu closes.
	static std::unordered_map<const RE::NiAVObject*, RE::NiSkinInstance*> g_menuHeadPartSkins;
	static const RaceHairFile& HeadPart(const char* model)
	{
		static std::unordered_map<std::string, RaceHairFile> cache;
		std::string key = model;
		for (auto& c : key) c = (char)tolower((unsigned char)c);
		if (auto it = cache.find(key); it != cache.end()) return it->second;
		RaceHairFile& f = cache[key];
		const std::string bytes = hdt::readAllFile(("Meshes\\" + std::string(model)).c_str());
		if (bytes.empty()) logger::warn("[SMP] head part Meshes\\{}: not found", model);
		else {
			try { f = ParseRaceHair(bytes, kHeadPartPrefix); }
			catch (std::exception& e) { logger::warn("[SMP] head part Meshes\\{}: {}", model, e.what()); }
			if (!f.physics.empty()) logger::info("[SMP] head part Meshes\\{}: physics '{}', {} shape(s) with bones", model, f.physics, f.lists.size());
		}
		return f;
	}

	// Of the head parts models[0..n) with physics, the one a FaceGen accessory shape is drawn from (-1 if none), and its
	// shape's game name in it: the same vertex count, and the file's vertices nearest on average (two head parts can be one
	// mesh mirrored: a pair of teeth).
	static int NearestHeadPart(const RE::NiGeometryData* d, const char* const* models, int n, std::string& game)
	{
		int best = -1;
		float bestDistance = FLT_MAX;
		for (int i = 0; i < n; ++i) {
			const RaceHairFile& f = HeadPart(models[i]);
			if (f.physics.empty()) continue;
			for (auto& [name, nifName] : f.nifName) {
				auto base = f.base.find(name);
				if (!f.lists.count(name) || base == f.base.end() || base->second.size() != d->vertexCount || !d->vertices) continue;
				float sum = 0;
				for (uint16_t v = 0; v < d->vertexCount; v += 8) sum += (d->vertices[v] - base->second[v]).Length();
				if (sum < bestDistance) { bestDistance = sum; best = i; game = name; }
			}
		}
		return best;
	}

	// The model path of an actor's hair (TESNPC::hair at 0x1B8, TESHair's TESModel at 0x24, its path at +4), under SEH.
	static const char* HairModel(void* ref)
	{
		__try {
			void* base = *(void**)((uint8_t*)ref + 0x20);   // TESObjectREFR::baseForm
			if (!base || *((uint8_t*)base + 4) != 0x2A)       // kFormType_NPC
				return nullptr;
			void* hair = *(void**)((uint8_t*)base + 0x1B8);
			return hair ? *(const char**)((uint8_t*)hair + 0x28) : nullptr;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	// The NPC's head parts' model paths (TESNPC::kHeadParts at 0x1DC, a BSSimpleList of BGSHeadPart, whose model path is at
	// 0x28 as a hair's): their count, -1 if not an NPC, -2 if unreadable.
	static int HeadPartModelList(void* ref, const char** out, int max)
	{
		int count = 0;
		__try {
			void* base = *(void**)((uint8_t*)ref + 0x20);
			if (!base || *((uint8_t*)base + 4) != 0x2A) return -1;
			struct Node { void* data; Node* next; };
			const Node* n = (const Node*)((uint8_t*)base + 0x1DC);
			for (int i = 0; n && i < 32 && count < max; n = n->next, ++i)
				if (n->data)
					if (const char* m = *(const char**)((uint8_t*)n->data + 0x28)) out[count++] = m;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return -2;
		}
		return count;
	}

	// The NPC's race head model path: TESNPC's race (TESRaceForm at 0x10C, the race at +4), its kHeadModels[sex][0] (0xB0, 0xC0
	// per sex, as TESRace::GetHeadPartModel 0x613970), the TESModel's path at +4; the sex is TESActorBaseData's flag 1 (0x34).
	static const char* RaceHeadModel(void* ref)
	{
		__try {
			void* base = *(void**)((uint8_t*)ref + 0x20);
			if (!base || *((uint8_t*)base + 4) != 0x2A) return nullptr;
			void* race = *(void**)((uint8_t*)base + 0x110);
			if (!race) return nullptr;
			const uint32_t female = *(uint32_t*)((uint8_t*)base + 0x34) & 1;
			return *(const char**)((uint8_t*)race + 0xB0 + 0xC0 * female + 4);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	// A race's head: the game's FaceGen loader keeps its NIF's face shape as "FaceGenFace" (TESRace::ppHeadPartNames[0]), skinned
	// and bound by the game, its extra data stripped as race hair's. Read from the head's NIF: the shape with bones there, whatever
	// its name, is the game's FaceGenFace.
	static const RaceHairFile& RaceHead(const char* model)
	{
		static std::unordered_map<std::string, RaceHairFile> cache;
		std::string key = model;
		for (auto& c : key) c = (char)tolower((unsigned char)c);
		if (auto it = cache.find(key); it != cache.end()) return it->second;
		RaceHairFile& f = cache[key];
		const std::string bytes = hdt::readAllFile(("Meshes\\" + std::string(model)).c_str());
		if (bytes.empty()) logger::warn("[SMP] race head Meshes\\{}: not found", model);
		else {
			try {
				RaceHairFile file = ParseRaceHair(bytes, "");
				for (auto& [lower, list] : file.lists) {
					f.physics = file.physics;
					f.lists["facegenface"] = list;
					if (auto v = file.verts.find(lower); v != file.verts.end()) f.verts["facegenface"] = v->second;
					f.names[file.nifName[lower]] = { "FaceGenFace" };
					break;
				}
			} catch (std::exception& e) { logger::warn("[SMP] race head Meshes\\{}: {}", model, e.what()); }
			if (!f.physics.empty()) logger::info("[SMP] race head Meshes\\{}: physics '{}'", model, f.physics);
		}
		return f;
	}

	static void* NpcOf(void* ref)
	{
		__try {
			void* base = ref ? *(void**)((uint8_t*)ref + 0x20) : nullptr;
			return base && *((uint8_t*)base + 4) == 0x2A ? base : nullptr;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return nullptr;
		}
	}

	// A FaceGen morph file (FREGM002): 64-byte header (magic, vertex count, symmetric and asymmetric morph counts), then
	// each morph: a scale and an int16 offset per vertex (x, y, z). A morph moves each vertex by offset * scale * the
	// NPC's value for it.
	struct Egm { uint32_t verts = 0, sym = 0, asym = 0; std::vector<float> scale; std::vector<int16_t> delta; };
	static bool LoadEgm(const std::string& path, Egm& e)
	{
		const std::string b = hdt::readAllFile(path.c_str());
		if (b.size() < 64 || b.compare(0, 8, "FREGM002") != 0) return false;
		memcpy(&e.verts, b.data() + 8, 4); memcpy(&e.sym, b.data() + 12, 4); memcpy(&e.asym, b.data() + 16, 4);
		const size_t morphs = e.sym + e.asym, each = 4 + 6 * (size_t)e.verts;
		if (b.size() < 64 + morphs * each) return false;
		e.scale.resize(morphs);
		e.delta.resize(morphs * 3 * (size_t)e.verts);
		for (size_t m = 0; m < morphs; ++m) {
			memcpy(&e.scale[m], b.data() + 64 + m * each, 4);
			memcpy(&e.delta[m * 3 * e.verts], b.data() + 64 + m * each + 4, 6 * (size_t)e.verts);
		}
		return true;
	}
	static void Morph(const std::vector<RE::NiPoint3>& base, const Egm& e, const float* sym, const float* asym, std::vector<RE::NiPoint3>& out)
	{
		out = base;
		for (uint32_t m = 0; m < e.sym + e.asym; ++m) {
			const float k = (m < e.sym ? sym[m] : asym[m - e.sym]) * e.scale[m];
			if (k == 0) continue;
			const int16_t* d = &e.delta[(size_t)m * 3 * e.verts];
			for (uint32_t v = 0; v < e.verts && v < out.size(); ++v)
				out[v] = out[v] + RE::NiPoint3{ d[3 * v] * k, d[3 * v + 1] * k, d[3 * v + 2] * k };
		}
	}

	// The NPC's face values (TESNPC::faceGenData at 0x134: geometry symmetric, then asymmetric, 0x20 each), read one of
	// four ways (the layout is not known for certain; CpuSkin picks the one that rebuilds what the game drew): the values
	// pointer at +0x0C as the floats, or offset by +0x10 bytes, or through one more pointer, with or without the offset.
	static bool FaceValues(void* npc, int entry, int way, float* out, uint32_t count)
	{
		__try {
			uint8_t* e = (uint8_t*)npc + 0x134 + 0x20 * entry;
			uint8_t* values = *(uint8_t**)(e + 0x0C);
			const uint32_t offset = *(uint32_t*)(e + 0x10);
			if (!values) return false;
			uint8_t* p = way & 2 ? *(uint8_t**)values : values;
			if (!p) return false;
			if (way & 1) p += offset;
			memcpy(out, p, count * sizeof(float));
			for (uint32_t i = 0; i < count; ++i) if (!std::isfinite(out[i]) || std::fabs(out[i]) > 100) return false;
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// A shape's shader property (NiPropertyState at 0x9C, shade at +0x0C) and its first flags word (BSShaderProperty +0x20:
	// bit 1 Skinned, bit 18 Hair); BSShaderPPLightingProperty::kHairTint at +0x84.
	static uint32_t& ShaderFlags(uint8_t* property) { return *reinterpret_cast<uint32_t*>(property + 0x20); }
	// The hair's shader property (its property state's shade slot), only if it is one: a hair shader's flags have Hair (bit
	// 18). A copy the race menu makes from a recycled shape can hold something else there for a while (flags 0xed1 seen),
	// which is not written to.
	static uint8_t* ShadeProperty(RE::NiAVObject* shape)
	{
		uint8_t* p = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(shape) + 0x9C + 0x0C);
		return p && (ShaderFlags(p) & 0x40000) ? p : nullptr;
	}
	// A head part's shader property: any lighting one (NiShadeProperty's type at +0x1C, PPLighting 8 .. Lighting30 0xC, the
	// range the game's FaceGen code checks).
	static uint8_t* LightingProperty(RE::NiAVObject* shape)
	{
		uint8_t* p = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(shape) + 0x9C + 0x0C);
		if (!p) return nullptr;
		const int32_t type = *reinterpret_cast<int32_t*>(p + 0x1C);
		return type >= 8 && type <= 0xC ? p : nullptr;
	}

	// A hair shape's .egm (<model without .nif><shape's name in the NIF>.egm), read once; null if there is none.
	static std::shared_ptr<Egm> HairEgm(const std::string& model, const RaceHairFile& f, const std::string& lowerShape)
	{
		static std::unordered_map<std::string, std::shared_ptr<Egm>> cache;
		std::string key = model + "|" + lowerShape;
		for (auto& c : key) c = (char)tolower((unsigned char)c);
		if (auto it = cache.find(key); it != cache.end()) return it->second;
		auto nif = f.nifName.find(lowerShape);
		std::string path = "Meshes\\" + model;
		if (path.size() > 4) path = path.substr(0, path.size() - 4);
		auto e = std::make_shared<Egm>();
		// A hair's is <model><shape>.egm; a head part's (one shape) <model>.egm.
		bool loaded = nif != f.nifName.end() && LoadEgm(path + nif->second + ".egm", *e);
		if (!loaded && f.nifName.size() == 1) { *e = Egm(); loaded = LoadEgm(path + ".egm", *e); }
		if (!loaded || e->sym > 50 || e->asym > 30) e = nullptr;
		return cache[key] = e;
	}

	ActorManager* ActorManager::instance()
	{
		static ActorManager manager;
		return &manager;
	}

	void ActorManager::found(RE::NiNode* root, RE::NiAVObject* model, const char* file, const hdt::DefaultBBP::NameMap_t* names,
		const std::unordered_map<std::string, std::string>* lists, RE::NiAVObject* hairShape, const char* headPartModel, bool raceHead)
	{
		// A head part (or the race's head) shares the FaceGen node with the hair and the other head parts: keyed by its own shape.
		auto key = std::make_pair((void*)root, (headPartModel || raceHead) && hairShape ? (void*)hairShape : (void*)model);
		auto it = m_items.find(key);
		if (it != m_items.end() && it->second.file != file) {
			// The same node now names another file: rebuild.
			if (it->second.system) {
				ReleaseCpuSkins(it->second);
				hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
			}
			SetProxiesHidden(it->second.model.get(), false);
			m_items.erase(it);
			it = m_items.end();
		}
		if (it == m_items.end()) {
			Item item;
			item.skeleton = root;
			item.model = model;
			item.file = file;
			item.worldObject = m_scanWorldObject;
			if (names) item.names = *names;
			if (lists && raceHead) {
				item.boneLists = *lists;
				item.raceHead = true;
			} else if (lists) {
				item.boneLists = *lists;
				item.npc = NpcOf(m_scanRef);
				if (headPartModel) { item.hairModel = headPartModel; item.headPart = true; }
				else if (const char* h = HairModel(m_scanRef)) item.hairModel = h;
			}
			item.buildAt = m_frame + kSettleFrames;
			it = m_items.emplace(key, std::move(item)).first;
		}
		it->second.frame = m_frame;
		// Race hair the game replaced (a new shape under the same node) or skinned again (rebuilt in place, the race
		// menu): built again on the shape as it is now.
		if (hairShape && it->second.system && !it->second.cpuSkins.empty()) {
			bool ours = false;
			bool reskinned = false;
			auto* g = hdt::castNiGeometry(hairShape);
			for (auto& s : it->second.cpuSkins)
				if (s.shape.get() == hairShape) { ours = true; reskinned = g && g->skinInstance && g->skinInstance != s.skin.get(); }
			if (!ours || reskinned) {
				logger::info("[SMP] {}: the game {} the hair, building again", hairShape->GetName(), ours ? "skinned again" : "replaced");
				it->second.system->m_itemDetached = true;
				ReleaseCpuSkins(it->second);
				hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
				it->second.system = nullptr;
				it->second.builds = 0;
				it->second.failed = false;
				it->second.buildAt = m_frame + kSettleFrames;
			}
		}
		// The game re-pointed the item's skin since it was built: it detached the model and attached it again while
		// the plugin was not looking (equipping in the Pip-Boy, which pauses the game), and the slots that drew from
		// the cloth bones now draw from the skeleton, collapsing the cloth. Built again, from scratch.
		if (it->second.system && rebound(it->second)) {
			logger::warn("[SMP] {}: the game re-pointed its skin (detached and attached again), building again", model->GetName());
			it->second.system->m_itemDetached = true;
			ReleaseCpuSkins(it->second);
			hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
			SetProxiesHidden(model, false);
			it->second.system = nullptr;
			it->second.builds = 0;
			it->second.failed = false;
			it->second.buildAt = m_frame + kSettleFrames;
		}
		// The game has not bound the item's skin (yet, or any more): no system until it has, then a few frames to
		// settle. Waiting does not use up the item's builds, so a long stay in the Pip-Boy doesn't end in a broken one.
		if (!it->second.failed && !skinsBound(model)) {
			if (it->second.system) {
				logger::warn("[SMP] {}: the game unbound its skin, physics paused until it is bound again", model->GetName());
				it->second.system->m_itemDetached = true;
				ReleaseCpuSkins(it->second);
				hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
				SetProxiesHidden(model, false);
				it->second.system = nullptr;
				it->second.builds = 0;
			}
			if (!it->second.waiting)
				logger::info("[SMP] {}: waiting for the game to bind its skin", model->GetName());
			// The item's stale bone slots are held at rest meanwhile (see PlaceDroppedBones).
			PlaceDroppedBones(model, root, model);
			it->second.waiting = true;
			it->second.buildAt = m_frame + kSettleFrames;
			return;
		}
		it->second.waiting = false;
		if (!it->second.system && !it->second.failed && m_frame >= it->second.buildAt)
			build(it->second);
	}

	bool ActorManager::rebound(const Item& item)
	{
		for (auto& s : item.system->m_boundSlots)
			if (!s.holds())
				return true;
		return false;
	}

	// Whether the game has bound the skin of every shape that lists its own bones: a skin with all its slots
	// pointing at nodes. After equipping in the Pip-Boy the game leaves the item's skins empty (0 bones) until
	// the Pip-Boy closes, however long that takes.
	bool ActorManager::skinsBound(RE::NiAVObject* obj, int depth)
	{
		if (!obj || depth > 64)
			return true;
		if (auto* g = hdt::castNiGeometry(obj)) {
			if (!g->FindStringExtra("SMP Bones"))
				return true;
			auto* skin = g->skinInstance;
			if (!skin || !skin->bones || !skin->boneCount)
				return false;
			for (uint32_t i = 0; i < skin->boneCount; ++i)
				if (!skin->bones[i])
					return false;
			return true;
		}
		if (auto* node = hdt::castNiNode(obj))
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				if (!skinsBound(node->children[i], depth + 1))
					return false;
		return true;
	}

	static uint32_t HeadSlot(const std::string& list);

	// The game attaches an item over several frames (the model, then its skin re-pointed at the
	// skeleton), so an item is built once it has been there kSettleFrames, and built again if the build
	// found the item's own bones missing, up to kMaxBuilds times; the last try is kept as it is.
	void ActorManager::build(Item& item)
	{
		auto* root = item.skeleton.get();
		auto* model = item.model.get();
		const char* file = item.file.c_str();
		++item.builds;
		hdt::DefaultBBP::PhysicsFile_t physicsFile{ item.file, item.names };
		hdt::SkyrimSystemCreator creator;
		creator.m_boneLists = item.boneLists;
		if (!item.hairModel.empty()) creator.m_fileMeshes = (item.headPart ? HeadPart(item.hairModel.c_str()) : RaceHair(item.hairModel.c_str())).proxies;   // its collision proxies, from the NIF
		// The ground plane is anchored on the item's root: an actor's feet, but a placed object's own origin (a cloth's top
		// edge, which would then hang below the plane and be shoved up through it).
		creator.m_ground = gSettings.ground && !item.worldObject;
		creator.m_worldObject = item.worldObject;
		m_builtGround = gSettings.ground;
		logger::info("[SMP] building '{}' for {} on '{}' (try {})", file, model->GetName(), root->GetName(), item.builds);
		// Race hair: its Bip01 Head slot on the skeleton's head. The game binds it by name and may take the hair NIF's own node
		// of that name: the file's root bone in a hair rigged on its root (it lands on the head), but a bone deep in a whole
		// skeleton copied into the NIF lands off the head and never moves (the hair floats away, stretched to its strands).
		if (!item.hairModel.empty())
			if (auto* node = model->AsNode())
				if (RE::NiAVObject* head = FindOutside(root, model, "Bip01 Head"))
					for (uint16_t i = 0; i < node->ChildEnd(); ++i) {
						auto* g = hdt::castNiGeometry(node->children[i]);
						if (!g || !g->skinInstance || !g->skinInstance->bones) continue;
						std::string lower = g->GetName();
						for (auto& c : lower) c = (char)tolower((unsigned char)c);
						auto list = item.boneLists.find(lower);
						if (list == item.boneLists.end()) continue;
						const uint32_t slot = HeadSlot(list->second);
						RE::NiAVObject* was = slot < g->skinInstance->boneCount ? g->skinInstance->bones[slot] : nullptr;
						if (!was || was == head) continue;
						logger::warn("[SMP] {}: its Bip01 Head slot {} held {:p} '{}' at {:.1f} {:.1f} {:.1f}, not the skeleton's head {:p} at {:.1f} {:.1f} {:.1f}: "
						             "put on the skeleton's head", g->GetName(), slot, (void*)was, was->m_name ? was->m_name : "", was->world.translate.x,
							was->world.translate.y, was->world.translate.z, (void*)head, head->world.translate.x, head->world.translate.y, head->world.translate.z);
						g->skinInstance->bones[slot] = head;
					}
		// Race hair: the game does not reshape a skinned hair, so it holds the file's vertices. Unless it is an old copy brought
		// back (another hair picked, then this one again), drawn by the race menu since: its vertices are then the menu's, already
		// posed. The file's go back in before the build, which makes the hair's collision shapes from them (and the drawing
		// starts from them too).
		if (!item.hairModel.empty())
			if (auto* node = model->AsNode()) {
				const RaceHairFile& hairFile = (item.headPart ? HeadPart(item.hairModel.c_str()) : RaceHair(item.hairModel.c_str()));
				for (uint16_t i = 0; i < node->ChildEnd(); ++i) {
					auto* g = hdt::castNiGeometry(node->children[i]);
					if (!g || !g->data || !g->data->vertices) continue;
					std::string lower = g->GetName();
					for (auto& c : lower) c = (char)tolower((unsigned char)c);
					auto base = hairFile.base.find(lower);
					if (base == hairFile.base.end() || base->second.size() != g->data->vertexCount) continue;
					float off = 0.0f;
					for (size_t v = 0; v < base->second.size(); ++v) off = std::max(off, (g->data->vertices[v] - base->second[v]).Length());
					if (off <= 0.01f) continue;
					logger::warn("[SMP] {}: its vertices are up to {:.2f} units off the hair file's (posed before, as by the race menu): the file's put back",
						g->GetName(), off);
					std::copy(base->second.begin(), base->second.end(), g->data->vertices);
					g->data->dirtyFlags |= 1;
				}
			}
		auto system = creator.createOrUpdateSystem(root, model, &physicsFile, {}, nullptr);
		if (!system) {
			item.failed = true;
			logger::error("[SMP] '{}' produced no physics for {}", file, model->GetName());
			return;
		}
		if (!creator.complete() && item.builds < kMaxBuilds) {
			logger::warn("[SMP] {}: incomplete, building again in {} frames", model->GetName(), kRetryFrames);
			system = nullptr;   // gives its skin slots back
			item.buildAt = m_frame + kRetryFrames;
			return;
		}
		item.system = system;
		item.firstPerson = root == m_firstPersonRoot;
		if (item.firstPerson)
			item.system->setFirstPerson();
		// A placed object's nodes are updated from their local transforms whenever the game updates its scene graph (a fade,
		// a touch): the simulated bones write theirs too, so such an update keeps the physics' pose.
		if (item.worldObject) {
			// Actors' stand-ins collide with nothing but this tag (see updateStandIns).
			for (auto& m : item.system->meshes()) m->m_tags.push_back("world");
			item.worldRest.clear();
			const RE::NiTransform toRoot = root->world.Invert();
			for (auto& b : item.system->getBones()) {
				auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
				bone->m_writeLocal = true;
				item.worldRest.push_back(bone->m_node ? toRoot * bone->m_node->world : RE::NiTransform());
			}
		}
		hdt::SkyrimPhysicsWorld::get()->addSkinnedMeshSystem(item.system.get());
		logger::info("[SMP] '{}': {} bones, {} collision bodies", file, item.system->getBones().size(), item.system->meshes().size());

		// Race hair: the game draws it without skinning (its hair shader has no skinned path), at the node holding it. Its
		// shapes are skinned here every frame instead (skinOnCpu), from their vertices as the game made them (with the face
		// sliders' morphs), and their skins taken off so nothing else skins them again (NVR's shadows did).
		if (!item.boneLists.empty() && !item.raceHead)
			if (auto* node = model->AsNode())
				for (uint16_t i = 0; i < node->ChildEnd(); ++i) {
					auto* g = hdt::castNiGeometry(node->children[i]);
					if (!g || !g->skinInstance || !g->data || !g->data->vertices || !g->skinInstance->skinData || !g->skinInstance->bones) continue;
					std::string lower = g->GetName();
					for (auto& c : lower) c = (char)tolower((unsigned char)c);
					if (!item.boneLists.count(lower)) continue;
					Item::CpuSkin s;
					s.shape = g;
					s.skin = g->skinInstance;
					s.bind.assign(g->data->vertices, g->data->vertices + g->data->vertexCount);   // the file's (see before the build)
					// The hair's colour is applied by the game's hair shader; the Skinned flag (needed by the NIF's skin, now
					// done here) sends the shape to the plain lighting shader instead, without it.
					// A head part has no hair shader: the Skinned flag alone goes (as the game does when it takes a head part's skin off).
					std::string shade = "no shader property";
					if (uint8_t* p = item.headPart ? LightingProperty(g) : nullptr) {
						shade = std::format("shader flags 0x{:08x} (skinned {})", ShaderFlags(p), (ShaderFlags(p) >> 1) & 1);
						if (ShaderFlags(p) & 2) { ShaderFlags(p) &= ~2u; s.unskinnedFlag = true; }
					} else if (uint8_t* p = item.headPart ? nullptr : ShadeProperty(g)) {
						const float* tint = reinterpret_cast<const float*>(p + 0x84);
						shade = std::format("shader flags 0x{:08x} (skinned {}, hair {}), tint {:.2f} {:.2f} {:.2f}", ShaderFlags(p), (ShaderFlags(p) >> 1) & 1, (ShaderFlags(p) >> 18) & 1, tint[0], tint[1], tint[2]);
						if (ShaderFlags(p) & 2) { ShaderFlags(p) &= ~2u; s.unskinnedFlag = true; }
						// No Alpha_Texture (0x100): the game clears it on a hair it loads (0x82040003 seen), not on the copies the
						// race menu makes from recycled shapes (0x82040103), which then draw nothing.
						ShaderFlags(p) &= ~0x100u;
					}
					logger::info("[SMP] {}: skinned on the CPU ({} vertices, {} bones; {}; its data used {} time(s))", g->GetName(), g->data->vertexCount,
						s.skin->skinData->boneCount, shade, (long)g->data->refCount);

					// The face sliders: the game does not reshape a skinned hair, so its .egm is applied here, to the file's
					// vertices, with the NPC's face values (the values pointer read as the floats: confirmed in game), and
					// again whenever those change. The hair colour: the NPC's (TESNPC::hairColor at 0x1D8, RGB a byte each,
					// confirmed in game) is kept in the tint.
					if (item.npc && !item.hairModel.empty()) {
						const RaceHairFile& f = (item.headPart ? HeadPart(item.hairModel.c_str()) : RaceHair(item.hairModel.c_str()));
						auto base = f.base.find(lower);
						auto egm = HairEgm(item.hairModel, f, lower);
						float sym[50], asym[30];
						if (base != f.base.end() && base->second.size() == s.bind.size() && egm && egm->verts == s.bind.size() &&
							FaceValues(item.npc, 0, 0, sym, egm->sym) && FaceValues(item.npc, 1, 0, asym, egm->asym)) {
							s.symWay = s.asymWay = 0;
							memcpy(s.sym, sym, sizeof(sym));
							memcpy(s.asym, asym, sizeof(asym));
							s.fileBase = base->second;
							s.egm = egm;
							Morph(s.fileBase, *s.egm, s.sym, s.asym, s.bind);   // the sliders as they are now
							logger::info("[SMP] {}: face sliders applied here", g->GetName());
						} else
							logger::warn("[SMP] {}: face sliders not applied: no file vertices, .egm or face values", g->GetName());
						s.colourWay = 0;
					}
					g->skinInstance = nullptr;
					g_drawnHair.insert(g);
					item.cpuSkins.push_back(std::move(s));
				}
		// The player's race hair built after the race menu closed: the game's hat swap once more (see update).
		if (m_hatSwapAfterBuild && !item.cpuSkins.empty() && root == Game::Get3D(Game::Player())) {
			m_hatSwapAfterBuild = false;
			logger::info("[SMP] race hair built: the game's hat swap run again for the player{}", RunHatSwap(Game::Player()) ? "" : " (failed)");
		}
	}

	// The skins given back and the vertices as the game made them, when the physics goes (the shapes may stay).
	// A placed object's bones (its own nodes, which stay in its 3D) are put back at rest too: the simulated ones wrote their
	// swung pose into them (world and local), and the next build (another file, a re-pointed skin, the object back in reach,
	// changed collision settings) would take that pose for the rest one, the cloth rebuilt wherever it had swung to.
	void ActorManager::ReleaseCpuSkins(Item& item)
	{
		if (item.worldObject && item.system && item.skeleton && item.worldRest.size() == item.system->getBones().size()) {
			const auto& bones = item.system->getBones();
			// Worlds first, then each local from its parent's world (a parent may be one of the bones, put back just above).
			for (size_t i = 0; i < bones.size(); ++i)
				if (auto* node = static_cast<hdt::SkyrimBone*>(bones[i].get())->m_node.get()) node->world = item.skeleton->world * item.worldRest[i];
			for (auto& b : bones)
				if (auto* node = static_cast<hdt::SkyrimBone*>(b.get())->m_node.get(); node && node->parent) node->local = node->parent->world.Invert() * node->world;
		}
		for (auto& s : item.cpuSkins) {
			auto* g = hdt::castNiGeometry(s.shape.get());
			if (!g) continue;
			// The vertices go back unless the game wrote its own since (a new shape from the race menu).
			const bool ours = s.written.empty() || (g->data && g->data->vertices && g->data->vertexCount == s.written.size() &&
				memcmp(s.written.data(), g->data->vertices, s.written.size() * sizeof(RE::NiPoint3)) == 0);
			if (ours && g->data && g->data->vertices && g->data->vertexCount == s.bind.size()) {
				std::copy(s.bind.begin(), s.bind.end(), g->data->vertices);
				g->data->dirtyFlags |= 1;
			}
			if (!g->skinInstance) g->skinInstance = s.skin.get();
			if (uint8_t* p = s.unskinnedFlag ? (item.headPart ? LightingProperty(g) : ShadeProperty(g)) : nullptr) ShaderFlags(p) |= 2;
		}
		item.cpuSkins.clear();
	}

	// The skins taken off race hair in the race menu (the game draws a skinned shape from the buffers it packed, not from
	// the vertices written here), by shape. Plain pointers, nothing held: a copy the menu throws away keeps its skin
	// (leaked, never touched again); the copy that remains gets its skin back when the menu closes (scan).
	struct MenuSkin { RE::NiSkinInstance* skin; std::string model; };   // the skin taken off, and the hair file the copy is of
	static std::unordered_map<const RE::NiAVObject*, MenuSkin> g_menuSkins;
	// Old copies hidden and renamed here (see the scan), by plain pointer, with the name the game gave them: kept across race
	// menu visits (the game shows such a copy again when its hair is chosen again); only acted on when the scan finds the
	// object in the scene still carrying the mark.
	static std::unordered_map<const RE::NiAVObject*, std::string> g_renamedHair;
	static constexpr const char* kOldHairMark = " (SMP old copy)";

	// Binds a race hair copy's skin the game left unbound (the race menu's copies made from recycled shapes): each slot on the
	// skeleton's bone of its "SMP Bones" name, the hair's own bones (the physics' to come, "SMP ...") on the head for now
	// (the physics build puts its nodes there), the root on the actor's. A new slot array from the game's allocator, which
	// frees it with the skin (the old one, of unknown size, is left).
	static std::vector<std::string> SlotNames(const std::string& list);
	static bool BindHairSkin(RE::NiGeometry* g, RE::NiNode* root, const std::string& model, bool headPart = false)
	{
		auto* sk = g->skinInstance;
		if (!sk || !sk->skinData || !sk->skinData->boneCount || !g->parent)
			return false;
		std::string lower = g->GetName();
		for (auto& c : lower) c = (char)tolower((unsigned char)c);
		const RaceHairFile& f = headPart ? HeadPart(model.c_str()) : RaceHair(model.c_str());
		auto list = f.lists.find(lower);
		RE::NiAVObject* head = FindOutside(root, g->parent, "Bip01 Head");
		if (list == f.lists.end() || !head)
			return false;
		const std::vector<std::string> names = SlotNames(list->second);
		const uint32_t n = sk->skinData->boneCount;
		auto** bones = static_cast<RE::NiAVObject**>(((void*(__cdecl*)(size_t))Game::kNiNew)(n * sizeof(RE::NiAVObject*)));
		if (!bones)
			return false;
		int named = 0;
		for (uint32_t i = 0; i < n; ++i) {
			RE::NiAVObject* node = nullptr;
			if (i < names.size() && !names[i].empty() && names[i].rfind("smp ", 0) != 0)
				node = FindOutside(root, g->parent, names[i].c_str());
			named += node != nullptr;
			bones[i] = node ? node : head;
		}
		sk->bones = bones;
		sk->boneCount = n;
		sk->rootParent = root;
		logger::info("[SMP] race menu: {} {:p} came with its skin unbound: bound here ({} slots, {} on skeleton bones)", g->GetName(), (void*)g, n, named);

		// Its data, drawn here from its own vertices with the skin taken off: a recycled shape's may lack the triangles or
		// normals (skinned, the game draws from the skin partition's): from the file then, kept (keep flags normals 0x02,
		// indices 0x10), in arrays from the game's allocator.
		auto* td = static_cast<RE::NiTriShapeData*>(g->data);
		const auto tris = f.triangles.find(lower);
		const auto norms = f.normals.find(lower);
		logger::info("[SMP] race menu: {} data: {} triangles (list {:p}, {} indices), normals {:p}, keep {:#x}; the file has {} triangles, {} normals",
			g->GetName(), td->triangleCount, (void*)td->triList, td->triListLength, (void*)td->normals, td->keepFlags,
			tris != f.triangles.end() ? tris->second.size() / 3 : 0, norms != f.normals.end() ? norms->second.size() : 0);
		if ((!td->triList || !td->triangleCount) && tris != f.triangles.end() && !tris->second.empty()) {
			auto* list = static_cast<uint16_t*>(((void*(__cdecl*)(size_t))Game::kNiNew)(tris->second.size() * sizeof(uint16_t)));
			if (list) {
				memcpy(list, tris->second.data(), tris->second.size() * sizeof(uint16_t));
				td->triList = list;
				td->triangleCount = td->activeTriangleCount = (uint16_t)(tris->second.size() / 3);
				td->triListLength = (uint32_t)tris->second.size();
				logger::info("[SMP] race menu: {} given its {} triangles from the file", g->GetName(), td->triangleCount);
			}
		}
		if (!td->normals && norms != f.normals.end() && norms->second.size() == td->vertexCount) {
			auto* nn = static_cast<RE::NiPoint3*>(((void*(__cdecl*)(size_t))Game::kNiNew)(norms->second.size() * sizeof(RE::NiPoint3)));
			if (nn) {
				memcpy(nn, norms->second.data(), norms->second.size() * sizeof(RE::NiPoint3));
				td->normals = nn;
				logger::info("[SMP] race menu: {} given its normals from the file", g->GetName());
			}
		}
		td->keepFlags |= 0x12;
		td->dirtyFlags |= 1;
		return true;
	}

	// obj's siblings that are other, not renamed, copies of the same hair shape (the game's name, same vertex count).
	static std::vector<RE::NiAVObject*> OtherCopies(const RE::NiAVObject* obj, const char* name, uint32_t vertexCount)
	{
		std::vector<RE::NiAVObject*> out;
		auto* parent = obj->parent;
		if (!parent)
			return out;
		for (uint16_t i = 0; i < parent->ChildEnd(); ++i) {
			auto* child = parent->children[i];
			if (!child || child == obj || !child->m_name || _stricmp(child->m_name, name) != 0)
				continue;
			auto* g = hdt::castNiGeometry(child);
			if (g && g->data && g->data->vertexCount == vertexCount)
				out.push_back(child);
		}
		return out;
	}

	// The "SMP Bones" list's slot names (lower case), in slot order.
	static std::vector<std::string> SlotNames(const std::string& list)
	{
		std::vector<std::string> out;
		size_t p = 0;
		while (p < list.size()) {
			size_t end = list.find(';', p);
			if (end == std::string::npos) end = list.size();
			std::string entry = list.substr(p, end - p);
			std::string name = entry.substr(0, entry.find('|'));
			for (auto& c : name) c = (char)tolower((unsigned char)c);
			out.push_back(name);
			p = end + 1;
		}
		return out;
	}

	// A weapon's skins in the player's first-person model (the shapes whose "SMP Bones" slots are all nodes of their own
	// model, under the shape's parent): the game never draws them as it leaves them (+0x1C, the renderer's matrix count,
	// stays 0), so each is bound here on those nodes, as BindHairSkin does.
	// No physics in first person: the charm hangs as the NIF has it.
	static void BindOwnNodeSkins(RE::NiAVObject* obj, int depth = 0)
	{
		if (!obj || depth > 48)
			return;
		if (auto* g = hdt::castNiGeometry(obj)) {
			auto* sk = g->skinInstance;
			if (!sk || !sk->skinData || !sk->skinData->boneCount || !g->parent)
				return;
			const std::string list = g->SmpBonesList();
			if (list.empty())
				return;
			const std::vector<std::string> names = SlotNames(list);
			const uint32_t n = sk->skinData->boneCount;
			if (names.size() < n)
				return;
			std::vector<RE::NiAVObject*> nodes(n);
			for (uint32_t i = 0; i < n; ++i)
				if (!(nodes[i] = FindOutside(g->parent, nullptr, names[i].c_str())))
					return;
			if (sk->boneCount)
				return;
			auto** bones = static_cast<RE::NiAVObject**>(((void*(__cdecl*)(size_t))Game::kNiNew)(n * sizeof(RE::NiAVObject*)));
			if (!bones)
				return;
			std::copy(nodes.begin(), nodes.end(), bones);
			sk->bones = bones;
			sk->boneCount = n;
			if (!sk->rootParent)
				sk->rootParent = g->parent;
			logger::info("[SMP] first person: {} came with its skin unbound: bound on its own nodes ({} slots)", g->GetName(), n);
			return;
		}
		if (auto* node = obj->AsNode())
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				BindOwnNodeSkins(node->children[i], depth + 1);
	}

	// The skin slot of a hair's Bip01 Head, by its "SMP Bones" list (not always the first: a hair rigged elsewhere lists it last).
	static uint32_t HeadSlot(const std::string& list)
	{
		const auto names = SlotNames(list);
		for (uint32_t i = 0; i < names.size(); ++i)
			if (names[i] == "bip01 head") return i;
		return 0;
	}

	// The stand-in skeleton (see m_menuRoot): a node of the plugin's own for each of the player's skeleton nodes (made the
	// first time its name is seen, never released), given the real one's place; read fresh from the player's 3D each
	// frame, nothing of it held.
	static std::vector<RE::NiPointer<RE::NiNode>> g_menuProxyNodes;
	void ActorManager::refreshMenuProxies(RE::NiNode* playerRoot)
	{
		if (!playerRoot) return;
		if (!m_menuRoot) {
			m_menuRoot = hdt::createNode("SMP Race Menu Skeleton");
			if (!m_menuRoot) return;
		}
		m_menuRoot->world = playerRoot->world;
		struct Walk
		{
			ActorManager* self;
			void run(RE::NiAVObject* obj, int depth)
			{
				auto* node = obj && depth < 64 ? obj->AsNode() : nullptr;
				if (!node) return;
				if (node->m_name && *node->m_name && _strnicmp(node->m_name, "BSFaceGen", 9) != 0) {
					std::string n = node->m_name;
					for (auto& c : n) c = (char)tolower((unsigned char)c);
					auto it = self->m_menuProxies.find(n);
					if (it == self->m_menuProxies.end()) {
						RE::NiPointer<RE::NiNode> proxy(hdt::createNode(node->m_name));
						if (proxy) { g_menuProxyNodes.push_back(proxy); it = self->m_menuProxies.emplace(n, proxy.get()).first; }
					}
					if (it != self->m_menuProxies.end()) it->second->world = node->world;
				}
				for (uint16_t i = 0; i < node->ChildEnd(); ++i) run(node->children[i], depth + 1);
			}
		} walk{ this };
		walk.run(playerRoot, 0);
	}

	void ActorManager::dropMenuSystem(const char* why)
	{
		if (!m_menuSystem) return;
		hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(m_menuSystem.get());
		m_menuSystem = nullptr;
		m_menuBones.clear();
		m_menuModel.clear();
		m_menuMissing = 0;
		logger::info("[SMP] race menu: hair physics dropped ({})", why);
	}

	// The race menu's hair physics, built on a copy of the hair the game has bound to the head (the creator places its
	// bones from that), then let go of the copy at once (its skin slots put back, see releaseSlots).
	bool ActorManager::buildMenuSystem(RE::NiNode* root, RE::NiAVObject* shape, const std::string& model)
	{
		const RaceHairFile& f = RaceHair(model.c_str());
		hdt::DefaultBBP::PhysicsFile_t physicsFile{ f.physics, f.names };
		hdt::SkyrimSystemCreator creator;
		creator.m_ground = gSettings.ground;
		creator.m_boneLists = f.lists;
		creator.m_fileMeshes = f.proxies;
		// On the stand-in skeleton: the system holds only the plugin's nodes.
		refreshMenuProxies(root);
		if (!m_menuRoot) return false;
		creator.m_skeletonNodes = m_menuProxies;
		// The menu's copies are not bound (their head slot holds the hair file's own Bip01 Head, at the world origin) and
		// their skin may be off (g_menuSkins): for the build only, the skin on and its head slot on the skeleton's head,
		// which the creator places the ponytail's bones from.
		auto* g = hdt::castNiGeometry(shape);
		RE::NiSkinInstance* skin = g ? g->skinInstance : nullptr;
		bool reattached = false;
		if (g && !skin)
			if (auto it = g_menuSkins.find(g); it != g_menuSkins.end()) { skin = it->second.skin; g->skinInstance = skin; reattached = true; }
		RE::NiAVObject* head = FindOutside(root, shape->parent, "Bip01 Head");
		if (!skin || !skin->bones || !skin->boneCount || !head) {
			if (reattached) g->skinInstance = nullptr;
			return false;
		}
		std::string lower = shape->GetName();
		for (auto& c : lower) c = (char)tolower((unsigned char)c);
		const auto list = f.lists.find(lower);
		const uint32_t headSlot = list != f.lists.end() ? HeadSlot(list->second) : 0;
		if (headSlot >= skin->boneCount) {
			if (reattached) g->skinInstance = nullptr;
			return false;
		}
		RE::NiAVObject* wasThere = skin->bones[headSlot];
		skin->bones[headSlot] = head;
		auto system = creator.createOrUpdateSystem(m_menuRoot.get(), shape->parent, &physicsFile, {}, nullptr);
		skin->bones[headSlot] = wasThere;
		if (reattached) g->skinInstance = nullptr;
		if (!system || !creator.complete()) {
			if (system) system->releaseSlots();
			return false;
		}
		const int restored = system->releaseSlots();
		hdt::SkyrimPhysicsWorld::get()->addSkinnedMeshSystem(system.get());
		m_menuSystem = system;
		m_menuModel = model;
		m_menuMissing = 0;
		m_menuBones.clear();
		for (auto& b : system->getBones()) {
			std::string n = b->m_name.c_str();
			for (auto& c : n) c = (char)tolower((unsigned char)c);
			m_menuBones[n] = static_cast<hdt::SkyrimBone*>(b.get())->m_node.get();
		}
		logger::info("[SMP] race menu: hair physics built ({} bones), {} skin slot(s) given back to the hair", system->getBones().size(), restored);
		return true;
	}

	// Race hair in the race menu (see m_raceMenu), on whichever copy of the hair the scan found this frame, skinned here
	// with the face sliders and the colour: its slots by name from the hair file's "SMP Bones", the ponytail's on the
	// race menu's physics (m_menuSystem), the rest on the skeleton (its Bip01 Head: a fresh copy's slot still holds the
	// hair file's own, at the world origin). Its skin is taken off (see g_menuSkins); nothing of it is held.
	void ActorManager::skinMenuHair()
	{
		static const void* lastShape = nullptr;
		static float lastSym[50], lastAsym[30];
		static std::vector<RE::NiPoint3> morphed, out;
		bool seen = false;
		for (auto& h : m_menuHair) {
			auto* g = hdt::castNiGeometry(h.shape);
			if (!g || !g->data || !g->data->vertices || !h.npc || !g->parent) continue;
			// A copy the race menu makes from a recycled shape comes with its skin never bound (no bones, its root itself), and
			// the game never binds it: bound here as the game would, by the slot names in the hair file's "SMP Bones".
			if (auto* sk = g->skinInstance; sk && (!sk->bones || !sk->boneCount) && !BindHairSkin(g, h.root, h.model)) continue;
			// Physics for the player's hair: built on a copy the game bound to the head, and kept while the hair is shown.
			const bool player = h.root == Game::Get3D(Game::Player());
			if (player && m_menuSystem && m_menuModel != h.model) dropMenuSystem("another hair");
			static unsigned lastTry = 0;
			if (player && !m_menuSystem && (lastTry == 0 || m_frame - lastTry >= 60)) {
				lastTry = m_frame;
				if (buildMenuSystem(h.root, g, h.model)) lastTry = 0;
			}
			if (player && m_menuSystem) seen = true;

			auto* skin = g->skinInstance;
			if (skin) { g_menuSkins[g] = { skin, h.model }; g->skinInstance = nullptr; }
			else if (auto it = g_menuSkins.find(g); it != g_menuSkins.end()) skin = it->second.skin;
			if (!skin || !skin->skinData || !skin->skinData->boneData || !skin->skinData->boneCount) continue;
			const RaceHairFile& f = RaceHair(h.model.c_str());
			std::string lower = g->GetName();
			for (auto& c : lower) c = (char)tolower((unsigned char)c);
			auto base = f.base.find(lower);
			auto list = f.lists.find(lower);
			if (base == f.base.end() || list == f.lists.end() || base->second.size() != g->data->vertexCount) continue;
			RE::NiAVObject* head = FindOutside(h.root, g->parent, "Bip01 Head");
			if (!head) continue;

			auto egm = HairEgm(h.model, f, lower);
			float sym[50] = {}, asym[30] = {};
			const bool faces = egm && egm->verts == base->second.size() && FaceValues(h.npc, 0, 0, sym, egm->sym) && FaceValues(h.npc, 1, 0, asym, egm->asym);
			if (lastShape != g || memcmp(sym, lastSym, sizeof(sym)) != 0 || memcmp(asym, lastAsym, sizeof(asym)) != 0 || morphed.size() != base->second.size()) {
				if (faces) Morph(base->second, *egm, sym, asym, morphed);
				else morphed = base->second;
				lastShape = g;
				memcpy(lastSym, sym, sizeof(sym));
				memcpy(lastAsym, asym, sizeof(asym));
			}

			// Each slot's node by name: the physics' (the player's, while it runs), else the skeleton's, else the head.
			const auto* data = skin->skinData;
			const std::vector<std::string> names = SlotNames(list->second);
			const uint32_t headSlot = std::min<uint32_t>(HeadSlot(list->second), data->boneCount - 1);
			const RE::NiTransform toShape = g->world.Invert();
			out.assign(morphed.size(), RE::NiPoint3{});
			for (uint32_t b = 0; b < data->boneCount; ++b) {
				if (!data->boneData[b].vertData) continue;
				RE::NiAVObject* node = nullptr;
				const std::string name = b < names.size() ? names[b] : std::string();
				if (player && m_menuSystem)
					if (auto it = m_menuBones.find(name); it != m_menuBones.end()) node = it->second;
				if (!node && !name.empty() && name.rfind("smp ", 0) != 0) node = FindOutside(h.root, g->parent, name.c_str());
				// A ponytail bone without its physics (an NPC's hair, or before it is built): where it rests on the head.
				const RE::NiTransform m = node ? toShape * (node->world * data->boneData[b].skinToBone)
				                               : toShape * (head->world * data->boneData[headSlot].skinToBone);
				const auto* vw = data->boneData[b].vertData;
				for (uint16_t k = 0; k < data->boneData[b].vertCount; ++k)
					if (vw[k].vertex < out.size()) out[vw[k].vertex] = out[vw[k].vertex] + (m * morphed[vw[k].vertex]) * vw[k].weight;
			}
			RE::NiPoint3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (size_t v = 0; v < out.size(); ++v) {
				g->data->vertices[v] = out[v];
				lo = { std::min(lo.x, out[v].x), std::min(lo.y, out[v].y), std::min(lo.z, out[v].z) };
				hi = { std::max(hi.x, out[v].x), std::max(hi.y, out[v].y), std::max(hi.z, out[v].z) };
			}
			g->data->bound.center = (lo + hi) * 0.5f;
			g->data->bound.radius = (hi - g->data->bound.center).Length();
			g->data->dirtyFlags |= 1;
			if (uint8_t* p = ShadeProperty(g)) {
				ShaderFlags(p) &= ~2u;   // the hair shader (with the colour), as in skinOnCpu
				ShaderFlags(p) &= ~0x100u;   // and no Alpha_Texture, as the game leaves a hair it loads (see build)
				const uint32_t c = *(uint32_t*)((uint8_t*)h.npc + 0x1D8);
				float* tint = reinterpret_cast<float*>(p + 0x84);
				tint[0] = (c & 0xFF) / 255.0f;
				tint[1] = ((c >> 8) & 0xFF) / 255.0f;
				tint[2] = ((c >> 16) & 0xFF) / 255.0f;
			}
		}
		// The physics goes when its hair has not been seen for a while (the menu leaves frames with no hair as it rebuilds
		// the head: those are not a change of hair).
		if (m_menuSystem && !seen && ++m_menuMissing > 60) dropMenuSystem("its hair is gone");
		if (seen) m_menuMissing = 0;
		m_menuHair.clear();
	}

	// A race menu head part copy's vertices at rest (as the game made them, with the face sliders) and as last written here,
	// by shape (plain pointers, as g_menuHeadPartSkins): the game reshaping a copy in place shows as vertices not written here.
	struct MenuPartVerts { std::vector<RE::NiPoint3> rest, written; };
	static std::unordered_map<const RE::NiAVObject*, MenuPartVerts> g_menuHeadPartVerts;

	void ActorManager::dropMenuHeadParts(const char* why)
	{
		for (auto& [model, s] : m_menuPartSystems)
			if (s.system) hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(s.system.get());
		if (!m_menuPartSystems.empty()) logger::info("[SMP] race menu: {} head part physics dropped ({})", m_menuPartSystems.size(), why);
		m_menuPartSystems.clear();
	}

	void ActorManager::dropMenuFace(const char* why)
	{
		if (m_menuFace.system) {
			hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(m_menuFace.system.get());
			logger::info("[SMP] race menu: race head {} physics dropped ({})", m_menuFaceModel, why);
		}
		m_menuFace = MenuPartSystem();
		m_menuFaceModel.clear();
	}

	// The race menu's physics for the player's race head, as a head part's: built on the face copy the menu shows (the game
	// bound its skeleton bones; the creator finds its bone list by its name, FaceGenFace), on the stand-in skeleton, then its
	// slots let go.
	bool ActorManager::buildMenuFace(RE::NiGeometry* g, RE::NiNode* root, const std::string& model)
	{
		const RaceHairFile& f = RaceHead(model.c_str());
		refreshMenuProxies(root);
		if (!m_menuRoot || !g->parent) return false;
		hdt::DefaultBBP::PhysicsFile_t physicsFile{ f.physics, f.names };
		hdt::SkyrimSystemCreator creator;
		creator.m_ground = gSettings.ground;
		creator.m_boneLists = f.lists;
		creator.m_skeletonNodes = m_menuProxies;
		auto system = creator.createOrUpdateSystem(m_menuRoot.get(), g->parent, &physicsFile, {}, nullptr);
		if (!system || !creator.complete()) {
			if (system) system->releaseSlots();
			logger::warn("[SMP] race menu: race head {} face {:p}: physics {}", model, (void*)g, system ? "incomplete" : "not built");
			return false;
		}
		system->releaseSlots();
		hdt::SkyrimPhysicsWorld::get()->addSkinnedMeshSystem(system.get());
		m_menuFace.system = system;
		m_menuFace.bones.clear();
		for (auto& b : system->getBones()) {
			std::string n = b->m_name.c_str();
			for (auto& c : n) c = (char)tolower((unsigned char)c);
			m_menuFace.bones[n] = static_cast<hdt::SkyrimBone*>(b.get())->m_node.get();
		}
		m_menuFaceModel = model;
		logger::info("[SMP] race menu: race head {} physics built ({} bones)", model, system->getBones().size());
		return true;
	}

	// The race menu's physics for a head part, as buildMenuSystem's for race hair: built on the copy with its skin on for the
	// build only (its head slot on the skeleton's head) and its file's game name (the creator finds its bone lists and the
	// XML's shapes by it), on the stand-in skeleton; then the copy let go (its skin slots put back, the skin off, its name back).
	bool ActorManager::buildMenuHeadPart(const MenuHeadPart& h, MenuPartSystem& out)
	{
		auto* g = hdt::castNiGeometry(h.shape);
		auto skinIt = g_menuHeadPartSkins.find(h.shape);
		RE::NiAVObject* head = g && g->parent ? FindOutside(h.root, g->parent, "Bip01 Head") : nullptr;
		if (!g || skinIt == g_menuHeadPartSkins.end() || !head) {
			logger::warn("[SMP] race menu: head part {} copy {:p}: {}", h.model, (void*)h.shape, !g ? "not a shape" : !head ? "no head" : "no skin");
			return false;
		}
		RE::NiSkinInstance* skin = skinIt->second;
		const RaceHairFile& f = HeadPart(h.model.c_str());
		const uint32_t headSlot = HeadSlot(f.lists.at(h.key));
		const std::string gameName = kHeadPartPrefix + f.nifName.at(h.key);
		// A copy the menu makes from a recycled shape comes with its skin never bound (no bones): bound as race hair's are,
		// by its game name.
		if (!skin->bones || !skin->boneCount) {
			hdt::setName(h.shape, gameName.c_str());
			g->skinInstance = skin;
			const bool bound = BindHairSkin(g, h.root, h.model, true);
			g->skinInstance = nullptr;
			hdt::setName(h.shape, "FaceGenAccessory");
			if (!bound) { logger::warn("[SMP] race menu: head part {} copy {:p}: its skin could not be bound", h.model, (void*)g); return false; }
		}
		if (headSlot >= skin->boneCount) { logger::warn("[SMP] race menu: head part {}: no head slot in its skin", h.model); return false; }
		refreshMenuProxies(h.root);
		if (!m_menuRoot) return false;
		hdt::DefaultBBP::PhysicsFile_t physicsFile{ f.physics, f.names };
		hdt::SkyrimSystemCreator creator;
		creator.m_ground = gSettings.ground;
		creator.m_boneLists = f.lists;
		creator.m_fileMeshes = f.proxies;
		creator.m_skeletonNodes = m_menuProxies;
		hdt::setName(h.shape, gameName.c_str());
		g->skinInstance = skin;
		RE::NiAVObject* wasThere = skin->bones[headSlot];
		skin->bones[headSlot] = head;
		auto system = creator.createOrUpdateSystem(m_menuRoot.get(), g->parent, &physicsFile, {}, nullptr);
		skin->bones[headSlot] = wasThere;
		g->skinInstance = nullptr;
		hdt::setName(h.shape, "FaceGenAccessory");
		if (!system || !creator.complete()) {
			if (system) system->releaseSlots();
			logger::warn("[SMP] race menu: head part {} copy {:p}: physics {}", h.model, (void*)g, system ? "incomplete" : "not built");
			return false;
		}
		system->releaseSlots();
		hdt::SkyrimPhysicsWorld::get()->addSkinnedMeshSystem(system.get());
		out.system = system;
		out.bones.clear();
		for (auto& b : system->getBones()) {
			std::string n = b->m_name.c_str();
			for (auto& c : n) c = (char)tolower((unsigned char)c);
			out.bones[n] = static_cast<hdt::SkyrimBone*>(b.get())->m_node.get();
		}
		logger::info("[SMP] race menu: head part {} physics built ({} bones)", h.model, system->getBones().size());
		return true;
	}

	// The race menu's head part copies (see m_menuHeadParts), skinned here: each slot on the head part's menu physics (the
	// player's), else the skeleton's bone of its name, else where it rests on the head.
	void ActorManager::skinMenuHeadParts()
	{
		std::vector<RE::NiPoint3> out;
		std::unordered_set<std::string> seen;
		RE::NiNode* playerRoot = Game::Get3D(Game::Player());
		for (auto& h : m_menuHeadParts) {
			auto* g = hdt::castNiGeometry(h.shape);
			auto skinIt = g_menuHeadPartSkins.find(h.shape);
			if (!g || !g->data || !g->data->vertices || !g->parent || skinIt == g_menuHeadPartSkins.end()) continue;
			const auto* data = skinIt->second->skinData;
			RE::NiAVObject* head = FindOutside(h.root, g->parent, "Bip01 Head");
			if (!data || !data->boneData || !data->boneCount || !head) continue;
			auto& verts = g_menuHeadPartVerts[h.shape];
			if (verts.rest.size() != g->data->vertexCount || verts.written.size() != g->data->vertexCount ||
				memcmp(verts.written.data(), g->data->vertices, verts.written.size() * sizeof(RE::NiPoint3)) != 0)
				verts.rest.assign(g->data->vertices, g->data->vertices + g->data->vertexCount);   // new, or reshaped by the game

			MenuPartSystem* sys = nullptr;
			if (h.root == playerRoot) {
				auto& s = m_menuPartSystems[h.model];
				if (!s.system && (s.lastTry == 0 || m_frame - s.lastTry >= 60)) {
					s.lastTry = m_frame;
					if (buildMenuHeadPart(h, s)) s.lastTry = 0;
				}
				if (s.system) { sys = &s; seen.insert(h.model); }
			}

			const RaceHairFile& f = HeadPart(h.model.c_str());
			const auto& list = f.lists.at(h.key);
			const std::vector<std::string> names = SlotNames(list);
			const uint32_t headSlot = std::min<uint32_t>(HeadSlot(list), data->boneCount - 1);
			const RE::NiTransform toShape = g->world.Invert();
			out.assign(verts.rest.size(), RE::NiPoint3{});
			for (uint32_t b = 0; b < data->boneCount; ++b) {
				if (!data->boneData[b].vertData) continue;
				const std::string name = b < names.size() ? names[b] : std::string();
				RE::NiAVObject* node = nullptr;
				if (sys)
					if (auto it = sys->bones.find(name); it != sys->bones.end()) node = it->second;
				if (!node && !name.empty() && name.rfind("smp ", 0) != 0) node = FindOutside(h.root, g->parent, name.c_str());
				const RE::NiTransform m = node ? toShape * (node->world * data->boneData[b].skinToBone)
				                               : toShape * (head->world * data->boneData[headSlot].skinToBone);
				const auto* vw = data->boneData[b].vertData;
				for (uint16_t k = 0; k < data->boneData[b].vertCount; ++k)
					if (vw[k].vertex < out.size()) out[vw[k].vertex] = out[vw[k].vertex] + (m * verts.rest[vw[k].vertex]) * vw[k].weight;
			}
			RE::NiPoint3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (auto& p : out) { lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) }; hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) }; }
			std::copy(out.begin(), out.end(), g->data->vertices);
			verts.written = out;
			g->data->bound.center = (lo + hi) * 0.5f;
			g->data->bound.radius = (hi - g->data->bound.center).Length();
			g->data->dirtyFlags |= 1;
		}
		// A head part's physics goes when no copy of it has been seen for a while (the menu rebuilds the head).
		for (auto it = m_menuPartSystems.begin(); it != m_menuPartSystems.end();) {
			if (seen.count(it->first) || !it->second.system) { it->second.missing = 0; ++it; continue; }
			if (++it->second.missing <= 60) { ++it; continue; }
			hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
			logger::info("[SMP] race menu: head part {} physics dropped (it is gone)", it->first);
			it = m_menuPartSystems.erase(it);
		}
		m_menuHeadParts.clear();
	}

	void ActorManager::skinOnCpu()
	{
		skinMenuHair();
		skinMenuHeadParts();
		std::vector<RE::NiPoint3> out;
		for (auto& [key, item] : m_items) {
			if (!item.system) continue;
			for (auto& s : item.cpuSkins) {
				auto* g = hdt::castNiGeometry(s.shape.get());
				auto* skin = s.skin.get();
				if (!g || !g->data || !g->data->vertices || g->data->vertexCount != s.bind.size() || !skin->skinData || !skin->bones) continue;
				const auto* data = skin->skinData;
				// The game reshaped the hair (a face slider): its vertices are no longer the ones written last frame. They
				// are the new shape, as the game makes it (unskinned, in the hair's own space).
				if (s.written.size() == s.bind.size() && memcmp(s.written.data(), g->data->vertices, s.bind.size() * sizeof(RE::NiPoint3)) != 0) {
					s.bind.assign(g->data->vertices, g->data->vertices + s.bind.size());
					static int reported = 0;
					if (reported++ < 10) logger::info("[SMP] {}: reshaped by the game (face sliders), taken as its new shape", g->GetName());
				}
				// The face sliders (see build): the NPC's values changed, the hair is morphed again from the file's vertices.
				if (s.egm && item.npc) {
					float sym[50], asym[30];
					if (FaceValues(item.npc, 0, s.symWay, sym, s.egm->sym) && FaceValues(item.npc, 1, s.asymWay, asym, s.egm->asym) &&
						(memcmp(sym, s.sym, s.egm->sym * sizeof(float)) != 0 || memcmp(asym, s.asym, s.egm->asym * sizeof(float)) != 0)) {
						memcpy(s.sym, sym, sizeof(sym));
						memcpy(s.asym, asym, sizeof(asym));
						Morph(s.fileBase, *s.egm, sym, asym, s.bind);
						static int reported = 0;
						if (reported++ < 10) logger::info("[SMP] {}: face sliders changed, hair reshaped", g->GetName());
					}
				}
				// The hair colour follows the NPC's (see build).
				if (s.colourWay >= 0 && item.npc)
					if (uint8_t* p = ShadeProperty(g)) {
						const uint32_t c = *(uint32_t*)((uint8_t*)item.npc + 0x1D8);
						float* tint = reinterpret_cast<float*>(p + 0x84);
						const float r = (c & 0xFF) / 255.0f, gg = ((c >> 8) & 0xFF) / 255.0f, b = ((c >> 16) & 0xFF) / 255.0f;
						tint[0] = s.colourWay == 0 ? r : b;
						tint[1] = gg;
						tint[2] = s.colourWay == 0 ? b : r;
					}
				// Each bone's skin-to-world, then into the shape's own space (where the game draws its vertices).
				const RE::NiTransform toShape = g->world.Invert();
				out.assign(s.bind.size(), RE::NiPoint3{});
				for (uint32_t b = 0; b < data->boneCount; ++b) {
					RE::NiAVObject* bone = skin->bones[b];
					if (!bone || !data->boneData[b].vertData) continue;
					const RE::NiTransform m = toShape * (bone->world * data->boneData[b].skinToBone);
					const auto* vw = data->boneData[b].vertData;
					for (uint16_t k = 0; k < data->boneData[b].vertCount; ++k) {
						const uint16_t v = vw[k].vertex;
						if (v >= out.size()) continue;
						out[v] = out[v] + (m * s.bind[v]) * vw[k].weight;
					}
				}
				RE::NiPoint3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
				for (auto& p : out) { lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) }; hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) }; }
				std::copy(out.begin(), out.end(), g->data->vertices);
				s.written = out;
				const RE::NiPoint3 c = (lo + hi) * 0.5f;
				g->data->bound.center = c;
				g->data->bound.radius = (hi - c).Length();
				g->data->dirtyFlags |= 1;   // the vertices changed: packed again for the draw
			}
		}
	}

	void ActorManager::scan(RE::NiNode* root, RE::NiAVObject* obj, int depth)
	{
		if (!obj || depth > 48)
			return;
		if (obj->extraDataCount) {
			if (const char* file = obj->FindStringExtra(kPhysicsKey)) {
				// On a shape, the item is the node holding it.
				RE::NiAVObject* model = obj->AsNode() ? obj : (obj->parent ? static_cast<RE::NiAVObject*>(obj->parent) : obj);
				if (file[0])
					found(root, model, file);
				if (obj->AsNode())
					return;   // one system per item: its shapes are part of it
			}
		}
		// Race hair (see RaceHairFile): a FaceGen hair shape whose hair NIF has physics, drawn from that NIF (same vertex
		// count), is an item under the node holding it.
		if (m_scanRef && obj->m_name && !_strnicmp(obj->m_name, "FaceGenHair", 11) && obj->parent && hdt::castNiGeometry(obj)) {
			if (const char* hair = HairModel(m_scanRef); hair && *hair) {
				const RaceHairFile& f = RaceHair(hair);
				// An old copy hidden here goes by another name (see below): it is judged by the name the game gave it. An entry
				// whose object no longer carries the mark is stale (its address reused): dropped.
				auto renamed = g_renamedHair.find(obj);
				if (renamed != g_renamedHair.end() && !strstr(obj->m_name, kOldHairMark)) {
					g_renamedHair.erase(renamed);
					renamed = g_renamedHair.end();
				}
				std::string lower = renamed != g_renamedHair.end() ? renamed->second : obj->m_name;
				for (auto& c : lower) c = (char)tolower((unsigned char)c);
				auto* g = hdt::castNiGeometry(obj);
				bool current = !f.physics.empty() && f.lists.count(lower) && g->data && f.verts.count(lower) && f.verts.at(lower) == g->data->vertexCount;
				// An old copy of the hair chosen again, one known to draw: shown again (its name back) unless another copy known
				// to draw already is. The game's new copy of that hair beside it (made from a recycled shape, it draws nothing)
				// becomes the old copy instead: hidden and renamed, its skin back, not drawn here. Otherwise the old copy stays one.
				if (renamed != g_renamedHair.end()) {
					std::vector<RE::NiAVObject*> others;
					bool shownDraws = false;
					if (current && m_raceMenu && g_drawnHair.count(obj)) {
						others = OtherCopies(obj, renamed->second.c_str(), g->data->vertexCount);
						for (auto* o : others) shownDraws |= g_drawnHair.count(o) != 0;
					}
					if (current && m_raceMenu && g_drawnHair.count(obj) && !shownDraws) {
						for (auto* o : others) {
							auto* og = hdt::castNiGeometry(o);
							if (auto it = g_menuSkins.find(o); it != g_menuSkins.end()) {
								if (og && !og->skinInstance) og->skinInstance = it->second.skin;
								g_menuSkins.erase(it);
							}
							m_menuHair.erase(std::remove_if(m_menuHair.begin(), m_menuHair.end(), [o](const MenuHair& h) { return h.shape == o; }), m_menuHair.end());
							*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(o) + 0x30) |= 1;
							g_renamedHair[o] = o->m_name;
							hdt::setName(o, (std::string(o->m_name) + kOldHairMark).c_str());
							logger::info("[SMP] race menu: the game's new copy {:p} of {} set aside for the old one {:p}, which draws", (void*)o, renamed->second, (void*)obj);
						}
						hdt::setName(obj, renamed->second.c_str());
						*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + 0x30) &= ~1u;
						g_renamedHair.erase(renamed);
						renamed = g_renamedHair.end();
					} else {
						current = false;
						// Kept hidden every frame: the game sets the hidden flag of the shapes under the head's FaceGen nodes again
						// (seen clearing it once the race menu closed), and an old copy shown draws as a ghost of the old hair.
						*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + 0x30) |= 1;
					}
				}
				// A copy the race menu skinned here that is no longer the SMP hair shown (another hair chosen): its skin back,
				// and hidden (app-culled) as it was out of sight before it was placed here (the race menu keeps old copies; drawn
				// with their own slots they come out torn). The hair shown when the menu closes only gets its skin back.
				// Hidden, it is also renamed: the game never removes it (its race menu clears only the biped hair node) but finds
				// hair by name, and its hat swap (0x6062E0), finding this copy first, would show it again (torn, its shadow on
				// the body) and never swap the hair actually shown to its Hat version. It stays in the scene: the game shows
				// it again when its hair is chosen again (taken out, that hair came back invisible).
				if (!g->skinInstance && (!current || !m_raceMenu))
					if (auto it = g_menuSkins.find(obj); it != g_menuSkins.end()) {
						g->skinInstance = it->second.skin;
						g_menuSkins.erase(it);
						if (!current) {
							*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + 0x30) |= 1;
							if (renamed == g_renamedHair.end()) {
								g_renamedHair[obj] = obj->m_name;
								hdt::setName(obj, (std::string(obj->m_name) + kOldHairMark).c_str());
							}
						}
					}
				if (current) {
					// Outside the race menu, only a hair the game has bound (its head slot on the skeleton's live Bip01 Head),
					// or the one skinned here (its skin taken off): the race menu leaves fresh, unbound copies behind it.
					auto* sk = g->skinInstance;
					const uint32_t headSlot = HeadSlot(f.lists.at(lower));
					const bool bound = !sk || (sk->bones && headSlot < sk->boneCount && sk->bones[headSlot] && IsLiveBone(sk->bones[headSlot], root, obj->parent));
					if (m_raceMenu)
						m_menuHair.push_back({ obj, root, NpcOf(m_scanRef), hair });
					else if (bound)
						found(root, obj->parent, f.physics.c_str(), &f.names, &f.lists, obj);
				}
			}
		}
		// Head parts (see HeadPart): a FaceGen accessory shape whose head part's NIF has physics, told by its vertex count,
		// is renamed to its file's game name and is an item of its own under the node holding it. Not in the race menu (its
		// copies come and go there, as race hair's do). The game does not bind its skin (it draws head parts unskinned): bound
		// here as the race menu's hair copies are (its head slot on the skeleton's head, its own bones there for the build).
		// In the race menu: a copy with physics, its skin taken off (see g_menuHeadPartSkins), skinned each frame by
		// skinMenuHeadParts (drawn rigidly until then).
		if (m_scanRef && m_raceMenu && obj->m_name && !_stricmp(obj->m_name, "FaceGenAccessory") && obj->parent)
			if (auto* g = hdt::castNiGeometry(obj); g && g->data && (g->skinInstance || g_menuHeadPartSkins.count(obj))) {
				const char* models[32];
				const int n = HeadPartModelList(m_scanRef, models, 32);
				std::string game;
				if (const int i = NearestHeadPart(g->data, models, n, game); i >= 0) {
					if (g->skinInstance) {
						g_menuHeadPartSkins[g] = g->skinInstance;
						g->skinInstance = nullptr;
						g->data->dirtyFlags |= 1;
						logger::info("[SMP] race menu: head part {} copy {:p}: its skin off (skinned here)", models[i], (void*)g);
					}
					m_menuHeadParts.push_back({ obj, root, models[i], game });
				}
			}
		if (m_scanRef && !m_raceMenu && obj->m_name && !_strnicmp(obj->m_name, "FaceGenAccessory", 16) && obj->parent) {
			auto* g = hdt::castNiGeometry(obj);
			// A race menu copy that remains: its skin back, bound and built below as any.
			if (g && !g->skinInstance)
				if (auto it = g_menuHeadPartSkins.find(obj); it != g_menuHeadPartSkins.end()) {
					g->skinInstance = it->second;
					g_menuHeadPartSkins.erase(it);
					// Its vertices at rest again (skinMenuHeadParts moved them).
					if (auto v = g_menuHeadPartVerts.find(obj); v != g_menuHeadPartVerts.end() && g->data && g->data->vertices &&
						v->second.rest.size() == g->data->vertexCount) {
						std::copy(v->second.rest.begin(), v->second.rest.end(), g->data->vertices);
						g->data->dirtyFlags |= 1;
					}
				}
			const char* models[32];
			// A shape renamed here has its skin off while its physics runs (skinned on the CPU, see build): still one.
			const bool renamed = !_strnicmp(obj->m_name, kHeadPartPrefix, strlen(kHeadPartPrefix));
			const int n = g && g->data && (g->skinInstance || renamed) ? HeadPartModelList(m_scanRef, models, 32) : 0;
			// Renamed: the head part of that name (its vertices are the physics' while it runs). Else the nearest file's.
			int pick = -1;
			std::string game;
			if (renamed) {
				for (int i = 0; i < n && pick < 0; ++i) {
					const RaceHairFile& f = HeadPart(models[i]);
					if (f.physics.empty()) continue;
					for (auto& [name, nifName] : f.nifName)
						if (f.verts.count(name) && f.verts.at(name) == g->data->vertexCount && f.lists.count(name) &&
							_stricmp(obj->m_name, (kHeadPartPrefix + nifName).c_str()) == 0) { pick = i; game = name; break; }
				}
			} else if (n > 0 && !_stricmp(obj->m_name, "FaceGenAccessory"))
				pick = NearestHeadPart(g->data, models, n, game);
			if (pick >= 0) {
				const RaceHairFile& f = HeadPart(models[pick]);
				const std::string want = kHeadPartPrefix + f.nifName.at(game);
				if (_stricmp(obj->m_name, want.c_str()) != 0) {
					hdt::setName(obj, want.c_str());
					g_renamedHeadParts.push_back(RE::NiPointer<RE::NiAVObject>(obj));
					logger::info("[SMP] head part {}: its FaceGen shape {:p} ({} vertices) named '{}'", models[pick], (void*)obj, g->data->vertexCount, want);
				}
				auto* sk = g->skinInstance;
				const uint32_t headSlot = HeadSlot(f.lists.at(game));
				const bool bound = !sk || (sk->bones && headSlot < sk->boneCount && sk->bones[headSlot] && IsLiveBone(sk->bones[headSlot], root, obj->parent));
				if (bound || BindHairSkin(g, root, models[pick], true))
					found(root, obj->parent, f.physics.c_str(), &f.names, &f.lists, obj, models[pick]);
			}
		}
		// The race's head (see RaceHead): the FaceGen face whose race head NIF has physics, the same vertex count. The game skins
		// and morphs it (sliders, expressions), so it is built as any item, on its node, keyed by its shape (race hair shares the
		// node). Not in the race menu, which rebuilds the face: its bones are only held at rest there (PlaceFaceBones).
		if (m_scanRef && obj->m_name && !_stricmp(obj->m_name, "FaceGenFace") && obj->parent)
			if (auto* g = hdt::castNiGeometry(obj); g && g->data && g->skinInstance)
				if (const char* head = RaceHeadModel(m_scanRef); head && *head) {
					const RaceHairFile& f = RaceHead(head);
					auto v = f.verts.find("facegenface");
					if (!f.physics.empty() && v != f.verts.end() && v->second == g->data->vertexCount) {
						if (m_raceMenu) {
							// The player's: on its menu physics (built here, tried again each second until it is).
							const bool player = root == Game::Get3D(Game::Player());
							if (player && m_menuFace.system && m_menuFaceModel != head) dropMenuFace("another race head");
							if (player && !m_menuFace.system && (m_menuFace.lastTry == 0 || m_frame - m_menuFace.lastTry >= 60)) {
								m_menuFace.lastTry = m_frame;
								if (buildMenuFace(g, root, head)) m_menuFace.lastTry = 0;
							}
							PlaceFaceBones(g, root, SlotNames(f.lists.at("facegenface")), player && m_menuFace.system ? &m_menuFace.bones : nullptr);
						} else found(root, obj->parent, f.physics.c_str(), &f.names, &f.lists, obj, nullptr, true);
					}
				}
		if (auto* node = obj->AsNode()) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				scan(root, node->children[i], depth + 1);
		}
	}

	// Placed objects with physics (see m_worldObjects). Looked for in the loaded cells' references every kWorldObjectRescan
	// frames (an actor's own 3D is scanned with the actors); each found is seen every frame while its 3D is still in the
	// scene (attached under a parent), and goes with it.
	void ActorManager::scanWorldObjects(const RE::NiPoint3& playerPos)
	{
		if (m_frame >= m_worldObjectsAt) {
			m_worldObjectsAt = m_frame + kWorldObjectRescan;
			std::vector<WorldObject> objects;
			const float reach2 = kWorldObjectReach * kWorldObjectReach;
			Game::ForEachLoadedCell([&](void* cell) {
				Game::ForEachReference(cell, [&](void* ref, uint8_t type) {
					if (type == 0x3B || type == 0x3C) return;   // actors
					RE::NiNode* root = Game::Get3D(ref);
					if (!root || !root->parent || !root->extraDataCount) return;
					const RE::NiPoint3 d = root->world.translate - playerPos;
					if (d.x * d.x + d.y * d.y + d.z * d.z > reach2) return;
					const char* file = root->FindStringExtra(kPhysicsKey);
					if (file && file[0]) objects.push_back({ RE::NiPointer<RE::NiNode>(root), file });
				});
			});
			if (objects.size() != m_worldObjects.size())
				logger::info("[SMP] world objects with physics within {:.0f} units: {}", kWorldObjectReach, objects.size());
			m_worldObjects.swap(objects);
		}
		m_scanWorldObject = true;
		for (auto& o : m_worldObjects)
			if (o.root && o.root->parent) found(o.root.get(), o.root.get(), o.file.c_str());
		m_scanWorldObject = false;
	}

	// Stand-ins (see m_standIns): nothing at all while no world object's physics is built.
	void ActorManager::updateStandIns(const std::vector<std::tuple<float, RE::NiNode*, void*>>& actors)
	{
		// Every actor gets one, whatever SMP items it wears: earrings, a charm or hair do not cover its body (an SMP armor's
		// body shapes lie outside the capsules, so the cloth meets the armor first; animated bodies never test each other).
		std::vector<RE::NiPoint3> cloth;
		for (auto& [key, item] : m_items)
			if (item.system && item.worldObject) cloth.push_back(item.skeleton->world.translate);
		auto drop = [&](std::map<void*, StandIn>::iterator it) {
			if (it->second.system) hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
			return m_standIns.erase(it);
		};
		if (cloth.empty() || m_raceMenu) {   // the race menu rebuilds the player's 3D: nothing of it is held there
			if (!m_standIns.empty()) {
				logger::info("[SMP] stand-ins: {}, {} dropped", m_raceMenu ? "race menu" : "no world object with physics", m_standIns.size());
				for (auto it = m_standIns.begin(); it != m_standIns.end();) it = drop(it);
			}
			return;
		}
		const float reach2 = kStandInReach * kStandInReach;
		int built = 0;
		for (auto& a : actors) {
			RE::NiNode* root = std::get<1>(a);
			if (!root) continue;
			bool inReach = false;
			for (auto& c : cloth) {
				const RE::NiPoint3 d = root->world.translate - c;
				if (d.x * d.x + d.y * d.y + d.z * d.z < reach2) { inReach = true; break; }
			}
			if (!inReach) continue;
			auto it = m_standIns.find(root);
			// The model under the Weapon node (its first child), compared only.
			auto weaponOf = [](RE::NiNode* node) -> const void* {
				if (!node) return nullptr;
				for (uint16_t i = 0; i < node->ChildEnd(); ++i) if (node->children[i]) return node->children[i];
				return nullptr;
			};
			if (it != m_standIns.end() && weaponOf(it->second.weaponNode.get()) != it->second.weapon && built < kStandInBuildsPerFrame) {
				drop(it);   // another weapon in hand: built again with its capsule
				it = m_standIns.end();
			}
			if (it == m_standIns.end()) {
				if (built >= kStandInBuildsPerFrame) continue;
				++built;
				hdt::SkyrimSystemCreator creator;
				StandIn s;
				if (RE::NiAVObject* w = FindOutside(root, nullptr, "Weapon")) s.weaponNode = RE::NiPointer<RE::NiNode>(w->AsNode());
				s.weapon = weaponOf(s.weaponNode.get());
				s.system = creator.createStandIn(root);
				if (s.system) {
					hdt::SkyrimPhysicsWorld::get()->addSkinnedMeshSystem(s.system.get());
					logger::info("[SMP] stand-in for '{}': {} bone(s)", root->GetName(), s.system->getBones().size());
				}
				it = m_standIns.emplace(root, std::move(s)).first;   // a failed one is kept empty (not tried again while near)
			}
			it->second.frame = m_frame;
		}
		for (auto it = m_standIns.begin(); it != m_standIns.end();) {
			if (it->second.frame != m_frame) it = drop(it);
			else ++it;
		}
	}

	// Projectiles through world objects' cloth (see m_projectileLast): each one's path since the last frame pushes the
	// simulated bones near it along the shot. Run between the steps (the physics is not stepping here). Nothing at all
	// while no world object's physics is built.
	void ActorManager::pushByProjectiles(const std::vector<void*>& projectiles)
	{
		std::vector<Item*> cloth;
		for (auto& [key, item] : m_items)
			if (item.system && item.worldObject) cloth.push_back(&item);
		if (cloth.empty()) { m_projectileLast.clear(); return; }

		// The distance from a point to a segment.
		auto segDist = [](const btVector3& a, const btVector3& b, const btVector3& p) {
			const btVector3 ab = b - a;
			const btScalar t = std::clamp((p - a).dot(ab) / std::max(ab.length2(), btScalar(1e-6)), btScalar(0), btScalar(1));
			return (p - (a + ab * t)).length();
		};
		std::unordered_map<void*, RE::NiPoint3> now;
		for (void* p : projectiles) {
			const RE::NiPoint3 at = Game::RefPosition(p);
			now[p] = at;
			auto last = m_projectileLast.find(p);
			if (last == m_projectileLast.end()) continue;
			const btVector3 a = hdt::convertNi(last->second), b = hdt::convertNi(at);
			const btScalar len = (b - a).length();
			if (len < 1.0f || len > kProjectileMaxStep) continue;   // still, or not the same projectile (an address reused)
			const btVector3 dir = (b - a) / len;
			for (Item* item : cloth) {
				if (segDist(a, b, hdt::convertNi(item->skeleton->world.translate)) > kProjectileReach) continue;
				int pushed = 0;
				for (auto& bone : item->system->getBones()) {
					auto& rig = bone->m_rig;
					if (rig.isStaticOrKinematicObject()) continue;
					const btScalar d = segDist(a, b, rig.getWorldTransform().getOrigin() + hdt::SkyrimBone::s_origin);
					if (d >= kProjectileRadius) continue;
					rig.setLinearVelocity(rig.getLinearVelocity() + dir * (kProjectilePush * (1.0f - d / kProjectileRadius)));
					rig.activate(true);
					++pushed;
				}
				if (pushed && m_projectileHits++ < 20)
					logger::info("[SMP] projectile (form type {:#x}) through '{}': {} bone(s) pushed", (unsigned)Game::At<uint8_t>(p, 0x04), item->model->GetName(), pushed);
			}
		}
		m_projectileLast.swap(now);
	}

	// Explosions near world objects' cloth (see m_explosionsSeen): the first frame each one is in the process lists, every
	// simulated bone within kExplosionReach times its radius is pushed away from its centre. Between the steps, as
	// pushByProjectiles. Nothing at all while no world object's physics is built.
	void ActorManager::pushByExplosions(const std::vector<void*>& explosions)
	{
		std::vector<Item*> cloth;
		for (auto& [key, item] : m_items)
			if (item.system && item.worldObject) cloth.push_back(&item);
		if (cloth.empty()) { m_explosionsSeen.clear(); return; }

		std::unordered_set<void*> now(explosions.begin(), explosions.end());
		for (void* e : explosions) {
			if (m_explosionsSeen.count(e)) continue;   // pushed already
			const float radius = Game::ExplosionRadius(e);
			if (!(radius > 1.0f) || radius > 10000.0f) continue;
			const float reach = radius * kExplosionReach;
			const btVector3 centre = hdt::convertNi(Game::RefPosition(e));
			for (Item* item : cloth) {
				if ((hdt::convertNi(item->skeleton->world.translate) - centre).length() > reach + kExplosionItemSize) continue;
				int pushed = 0;
				for (auto& bone : item->system->getBones()) {
					auto& rig = bone->m_rig;
					if (rig.isStaticOrKinematicObject()) continue;
					const btVector3 away = rig.getWorldTransform().getOrigin() + hdt::SkyrimBone::s_origin - centre;
					const btScalar d = away.length();
					if (d >= reach || d < 1e-3f) continue;
					rig.setLinearVelocity(rig.getLinearVelocity() + away / d * (kExplosionPush * (1.0f - d / reach)));
					rig.activate(true);
					++pushed;
				}
				if (pushed && m_explosionHits++ < 20)
					logger::info("[SMP] explosion (radius {:.0f}) by '{}': {} bone(s) pushed", radius, item->model->GetName(), pushed);
			}
		}
		m_explosionsSeen.swap(now);
	}

	// The game's grab on SMP bones (Z by default): a press (read as the game reads it, so a rebound key works) takes the
	// simulated bone nearest the player's line of sight, and the next press lets go, as the game's own grab toggles. Held, the
	// bone is pulled toward where it was taken along the view each frame (its velocity set toward it, as the game's mouse spring
	// pulls an object). Not on a press while the game's own grab holds something (that press drops it), not on the player's own
	// items (the view starts inside them in third person), and let go in menus or a pause. Between the steps, as the pushes.
	void ActorManager::grabBones(RE::NiNode* playerRoot)
	{
		const bool running = Game::FrameTime() > 0.0f;
		const bool pressed = running && Game::GrabPressed();
		const bool gameGrabbing = Game::GameGrabbing(), gameWasGrabbing = m_gameGrabbing;
		m_gameGrabbing = gameGrabbing;

		hdt::SkyrimBone* held = nullptr;
		const Item* heldItem = nullptr;
		if (m_grabSystem)
			for (auto& [key, item] : m_items)
				if (item.system && item.system.get() == m_grabSystem && m_grabBone < item.system->getBones().size()) {
					held = static_cast<hdt::SkyrimBone*>(item.system->getBones()[m_grabBone].get());
					heldItem = &item;
				}
		if (m_grabSystem && (!held || pressed || !running)) {
			logger::info("[SMP] grab: let go{}", held ? "" : " (its physics went)");
			m_grabSystem = nullptr;
			held = nullptr;
			if (pressed) return;   // the press that let go
		}
		if (!held && (!pressed || gameGrabbing || gameWasGrabbing)) return;

		RE::NiPoint3 eyeNi, dirNi;
		if (!Game::EyeVector(eyeNi, dirNi)) return;
		const btVector3 eye = hdt::convertNi(eyeNi);
		btVector3 dir = hdt::convertNi(dirNi);
		if (dir.length2() < 1e-6f) return;
		dir.normalize();

		if (!held) {
			float best = kGrabRadius;
			for (auto& [key, item] : m_items) {
				if (!item.system || key.first == (void*)playerRoot || key.first == (void*)m_firstPersonRoot) continue;
				const auto& bones = item.system->getBones();
				for (size_t i = 0; i < bones.size(); ++i) {
					const auto& rig = bones[i]->m_rig;
					if (rig.isStaticOrKinematicObject()) continue;
					const btVector3 p = rig.getWorldTransform().getOrigin() + hdt::SkyrimBone::s_origin;
					const btScalar t = (p - eye).dot(dir);
					if (t <= 0 || t > kGrabReach) continue;
					const btScalar d = (p - (eye + dir * t)).length();
					if (d >= best) continue;
					best = d;
					m_grabSystem = item.system.get();
					m_grabBone = i;
					m_grabDistance = t;
					held = static_cast<hdt::SkyrimBone*>(bones[i].get());
					heldItem = &item;
				}
			}
			if (!held) return;
			logger::info("[SMP] grab: '{}' bone '{}', {:.0f} units away", heldItem->model->GetName(), held->m_name.c_str(), m_grabDistance);
		}

		auto& rig = held->m_rig;
		btVector3 velocity = (eye + dir * m_grabDistance - (rig.getWorldTransform().getOrigin() + hdt::SkyrimBone::s_origin)) * kGrabPull;
		if (velocity.length() > kGrabMaxSpeed) velocity *= kGrabMaxSpeed / velocity.length();
		rig.setLinearVelocity(velocity);
		rig.activate(true);
	}

	void ActorManager::playImpacts(const RE::NiPoint3& playerPos)
	{
		int played = 0;
		for (auto& i : hdt::SkyrimPhysicsWorld::get()->takeImpacts()) {
			if (i.file.empty() || (i.at - playerPos).Length() > kImpactSoundReach || played >= kImpactSoundsPerFrame) continue;   // empty: too soft to sound
			++played;
			const bool ok = Game::PlaySoundAt(i.file.c_str(), i.at, i.volume);
			if (!ok || m_impactLogs < 20) {
				++m_impactLogs;
				logger::info("[SMP] impact: '{}' on '{}' at {:.0f} units/s: {} (volume {:.2f}){}", i.boneA, i.boneB, i.speed, i.file, i.volume,
					ok ? "" : " - the game played nothing (is the file there?)");
			}
		}
	}

	void ActorManager::scanActor(RE::NiNode* root, void* ref)
	{
		m_scanRef = ref;
		if (root)
			scan(root, root, 0);
		m_scanRef = nullptr;
	}

	// How far apart each constraint's two pivots are, in the physics and as drawn (the nodes as the
	// frame left them), the widest per item until the next report.
	static void SampleJointGaps(hdt::SkyrimSystem* system, float& phys, float& drawn)
	{
		std::vector<hdt::BoneScaleConstraint*> all;
		for (auto& c : system->constraints()) all.push_back(c.get());
		for (auto& group : system->constraintGroups())
			for (auto& c : group->m_constraints) all.push_back(c.get());
		for (auto* c : all) {
			if (!c->getConstraint() || c->getConstraint()->getConstraintType() != D6_SPRING_2_CONSTRAINT_TYPE) continue;
			auto* g = static_cast<btGeneric6DofSpring2Constraint*>(c->getConstraint());
			auto* a = static_cast<hdt::SkyrimBone*>(c->m_boneA);
			auto* b = static_cast<hdt::SkyrimBone*>(c->m_boneB);
			if (&g->getRigidBodyA() != &a->m_rig) std::swap(a, b);
			const btVector3 pa = (g->getRigidBodyA().getWorldTransform() * g->getFrameOffsetA()).getOrigin();
			const btVector3 pb = (g->getRigidBodyB().getWorldTransform() * g->getFrameOffsetB()).getOrigin();
			phys = std::max(phys, (float)(pa - pb).length());
			if (!a->m_node || !b->m_node) continue;
			const btTransform ra = hdt::convertNi(a->m_node->world).asTransform() * a->m_localToRig;
			const btTransform rb = hdt::convertNi(b->m_node->world).asTransform() * b->m_localToRig;
			drawn = std::max(drawn, (float)((ra * g->getFrameOffsetA()).getOrigin() - (rb * g->getFrameOffsetB()).getOrigin()).length());
		}
	}

	void ActorManager::update()
	{
		++m_frame;
		const auto& s = gSettings;
		for (auto& [key, item] : m_items)
			if (item.system)
				SampleJointGaps(item.system.get(), item.jointGapPhys, item.jointGapDrawn);

		// The race menu: race hair lets go of everything it held (see m_raceMenu), and is placed each frame instead.
		m_menuHair.clear();
		if (Game::RaceMenuOpen() != m_raceMenu) {
			m_raceMenu = !m_raceMenu;
			if (m_raceMenu) {
				int released = 0;
				for (auto it = m_items.begin(); it != m_items.end();) {
					if (it->second.boneLists.empty()) { ++it; continue; }
					if (it->second.system) {
						ReleaseCpuSkins(it->second);
						// It holds the player's skeleton: it goes (the menu builds its own on stand-ins, see m_menuSystem).
						it->second.system->m_itemDetached = false;   // still attached: the skin slots go back to the game's
						hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
						it->second.system = nullptr;
					}
					it = m_items.erase(it);
					++released;
				}
				logger::info("[SMP] race menu: {} race hair item(s) let go; race hair drawn from the menu's own copies until it closes", released);
				for (auto& s : g_renamedHeadParts)
					if (s && s->m_name && !_strnicmp(s->m_name, kHeadPartPrefix, strlen(kHeadPartPrefix))) hdt::setName(s.get(), "FaceGenAccessory");
				if (!g_renamedHeadParts.empty()) logger::info("[SMP] race menu: {} head part shape(s) given their FaceGen name back", g_renamedHeadParts.size());
				g_renamedHeadParts.clear();
			} else {
				dropMenuSystem("race menu closed");
				dropMenuHeadParts("race menu closed");
				dropMenuFace("race menu closed");
			}
		}

		// The player, then every actor with its 3D loaded, in any process level, nearest first. None is dropped for distance
		// or count: the far ones run without collision (see the collision LOD below).
		void* player = Game::Player();
		RE::NiNode* playerRoot = player ? Game::Get3D(player) : nullptr;
		m_playerRootName = playerRoot ? playerRoot->GetName() : "(none)";
		if (playerRoot)
			KeepRadius(playerRoot, m_lastRootRadius);
		m_firstPersonRoot = nullptr;
		if (RE::NiNode* firstPerson = player ? Game::PlayerFirstPerson3D(player) : nullptr; firstPerson && firstPerson != playerRoot) {
			SetProxiesHidden(firstPerson, true);
			BindOwnNodeSkins(firstPerson);
			m_firstPersonRoot = firstPerson;
		}
		std::vector<std::tuple<float, RE::NiNode*, void*>> actors;
		if (playerRoot)
			actors.push_back({ 0.0f, playerRoot, player });
		RE::NiPoint3 playerPos = playerRoot ? playerRoot->world.translate : RE::NiPoint3{};
		// The process lists hold projectiles too (MobileObjects in the high process): kept apart (see pushByProjectiles).
		std::vector<void*> projectiles, explosions;
		Game::ForEachProcessActor([&](void* ref) {
			if (Game::IsProjectile(ref)) { projectiles.push_back(ref); return; }
			if (Game::IsExplosion(ref)) { explosions.push_back(ref); return; }
			if (auto* root = Game::Get3D(ref)) {
				actors.push_back({ (root->world.translate - playerPos).Length(), root, ref });
			}
		});
		std::sort(actors.begin(), actors.end(), [](auto& a, auto& b) { return std::get<0>(a) < std::get<0>(b); });

		for (auto& a : actors)
			scanActor(std::get<1>(a), std::get<2>(a));
		// The player's first-person model: the weapon it holds gets physics of its own (a charm), on that model. Only what
		// hangs on its Weapon node: the model also holds the worn items again, which are not seen in first person.
		if (m_firstPersonRoot)
			if (RE::NiAVObject* weapon = FindOutside(m_firstPersonRoot, nullptr, "Weapon")) {
				m_scanRef = player;
				scan(m_firstPersonRoot, weapon, 0);
				m_scanRef = nullptr;
			}
		if (!m_raceMenu) scanWorldObjects(playerPos);
		if (!m_raceMenu && !g_menuSkins.empty()) {   // the menu closed: the hair that remains got its skin back in the scan; the rest went
			logger::info("[SMP] race menu closed: {} hair skin(s) left with copies the menu threw away", g_menuSkins.size());
			g_menuSkins.clear();
		}
		if (!m_raceMenu && !g_menuHeadPartSkins.empty()) {   // the same for head parts
			logger::info("[SMP] race menu closed: {} head part skin(s) left with copies the menu threw away", g_menuHeadPartSkins.size());
			g_menuHeadPartSkins.clear();
		}
		if (!m_raceMenu) g_menuHeadPartVerts.clear();
		if (m_raceMenu != m_raceMenuWhenScanned) {
			m_raceMenuWhenScanned = m_raceMenu;
			if (!m_raceMenu && !g_renamedHair.empty())
				logger::info("[SMP] race menu closed: {} old hair copy(ies) hidden and renamed", g_renamedHair.size());
			if (!m_raceMenu) {
				m_hatSwapAt = m_frame + kHatSwapDelay;
				m_hatSwapAfterBuild = true;
			}
		}
		// The game's hat swap (0x6062E0: NoHat or Hat shown by what is worn) once more, the frame after the race menu closed (and
		// after the hair's in-game build, see build): the menu runs it while it rebuilds the head, before the SMP hair it is given
		// is settled here (old copies set aside, a new one bound), and the hair then keeps the NoHat version under a hat until
		// the hat is equipped again.
		if (m_hatSwapAt && m_frame >= m_hatSwapAt && !m_raceMenu) {
			m_hatSwapAt = 0;
			if (void* player = Game::Player())
				logger::info("[SMP] race menu closed: the game's hat swap run again for the player{}", RunHatSwap(player) ? "" : " (failed)");
		}

		// Items not seen this frame: unequipped, unloaded, or their actor left range.
		int failed = 0;
		std::vector<void*> roots;
		for (auto it = m_items.begin(); it != m_items.end();) {
			if (it->second.frame != m_frame) {
				if (it->second.system) {
					it->second.system->m_itemDetached = true;   // unequipped, or its actor's 3D went
					ReleaseCpuSkins(it->second);
					hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
				}
				SetProxiesHidden(it->second.model.get(), false);
				it = m_items.erase(it);
				continue;
			}
			failed += it->second.failed;
			// Collision LOD: past the collision distance, or beyond the nearest iMaxActors, physics without collision (the
			// player, nearest at distance 0, always collides).
			if (it->second.system) {
				float d = 0.0f;
				int rank = 0;
				for (size_t i = 0; i < actors.size(); ++i)
					if (std::get<1>(actors[i]) == it->first.first) { d = std::get<0>(actors[i]); rank = (int)i; break; }
				it->second.system->setCollisionLod(d > s.collisionDistance || rank >= std::max(1, s.maxActors));
			}
			if (std::find(roots.begin(), roots.end(), it->first.first) == roots.end())
				roots.push_back(it->first.first);
			++it;
		}
		updateStandIns(actors);
		pushByProjectiles(projectiles);
		pushByExplosions(explosions);
		grabBones(playerRoot);
		playImpacts(playerPos);
		stats.actors = (int)roots.size();
		stats.systems = (int)hdt::SkyrimPhysicsWorld::get()->systemCount();
		stats.failed = failed;

		// The race menu's hair physics reads its stand-in skeleton before the world steps: the real one's places now.
		if (m_menuSystem || !m_menuPartSystems.empty() || m_menuFace.system) refreshMenuProxies(playerRoot);
	}

	// A line in the log every couple of seconds saying whether the physics is doing anything visible:
	// how many of each system's bones are simulated, how far the simulated ones are from where they
	// would hang if rigid (0 = not moving), and whether the item's skin still draws with them.
	void ActorManager::report()
	{
		auto* world = hdt::SkyrimPhysicsWorld::get();
		logger::info("[stats] scan: player 3D '{}', {} physics item(s) seen ({} built)", m_playerRootName, m_items.size(), std::count_if(m_items.begin(), m_items.end(), [](auto& kv) { return (bool)kv.second.system; }));
		for (auto& [key, item] : m_items) {
			if (!item.system)
				continue;
			int simulated = 0;
			float maxOffset = 0, sumOffset = 0;
			std::vector<std::pair<float, std::string>> worst;
			for (auto& b : item.system->getBones()) {
				auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
				if (bone->m_rig.isStaticOrKinematicObject() || !bone->m_anchor)
					continue;
				++simulated;
				RE::NiPoint3 rigid = (bone->m_anchor->world * bone->m_anchorToNode).translate;
				float d = (bone->m_node->world.translate - rigid).Length();
				maxOffset = std::max(maxOffset, d);
				sumOffset += d;
				worst.push_back({ d, bone->m_name.c_str() });
			}
			std::sort(worst.rbegin(), worst.rend());
			std::string most;
			for (size_t k = 0; k < worst.size() && k < 5; k++)
				most += std::format("{} {:.1f}; ", worst[k].second, worst[k].first);
			int drawn = 0;
			for (auto& s : item.system->m_boundSlots)
				drawn += s.holds();
			logger::info("[stats] {}: {} bones, {} simulated, off rigid avg {:.2f} max {:.2f} units; skin slots drawn from cloth bones {}/{}; step {:.2f} ms ({:.2f} on the game thread)",
				item.model->GetName(), item.system->getBones().size(), simulated, simulated ? sumOffset / simulated : 0.0f, maxOffset,
				drawn, item.system->m_boundSlots.size(), world->m_averageStepMs, world->m_averageMainMs);
			if (!most.empty())
				logger::info("[stats]   furthest from rest: {}", most);
			// A world object's bones are its NIF's own nodes (no anchors): how far its simulated ones are from where they rested
			// at the build, in the physics and as drawn (their nodes), and the fastest.
			if (item.worldObject && item.worldRest.size() == item.system->getBones().size()) {
				int moving = 0;
				float physics = 0, drawnOff = 0, fastest = 0;
				const auto& bones = item.system->getBones();
				for (size_t i = 0; i < bones.size(); ++i) {
					auto* bone = static_cast<hdt::SkyrimBone*>(bones[i].get());
					if (bone->m_rig.isStaticOrKinematicObject() || !bone->m_node) continue;
					++moving;
					const btVector3 rest = (hdt::convertNi(item.skeleton->world * item.worldRest[i]).asTransform() * bone->m_localToRig).getOrigin();
					const btVector3 now = bone->m_rig.getWorldTransform().getOrigin() + hdt::SkyrimBone::s_origin;
					const btVector3 drawnAt = (hdt::convertNi(bone->m_node->world).asTransform() * bone->m_localToRig).getOrigin();
					physics = std::max(physics, (float)(now - rest).length());
					drawnOff = std::max(drawnOff, (float)(drawnAt - rest).length());
					fastest = std::max(fastest, (float)bone->m_rig.getLinearVelocity().length());
				}
				logger::info("[stats]   world object: {} simulated bone(s), furthest from rest {:.2f} units in the physics, {:.2f} as drawn; fastest {:.1f} units/s; "
				             "the weather's wind {:.2f} (0-1)", moving, physics, drawnOff, fastest, *(float*)Game::kWindMagnitude);
			}
			size_t joints = item.system->constraints().size();
			for (auto& group : item.system->constraintGroups()) joints += group->m_constraints.size();
			float jitter = 0, rawJitter = 0;
			for (auto& b : item.system->getBones()) {
				auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
				jitter = std::max(jitter, bone->m_jitter);
				rawJitter = std::max(rawJitter, bone->m_rawJitter);
			}
			auto* sys = item.system.get();
			logger::info("[stats]   {} person: joints apart at most {:.3f} in the physics, {:.3f} as drawn ({} joints); resets: {} turn, {} jump, {} other; "
				"jitter {:.4f} drawn, {:.4f} raw", item.firstPerson ? "first" : "third", item.jointGapPhys, item.jointGapDrawn, joints,
				sys->m_resetsTurn, sys->m_resetsJump, sys->m_resetsOther, jitter, rawJitter);
			sys->m_resetsTurn = sys->m_resetsJump = sys->m_resetsOther = 0;
			item.jointGapPhys = item.jointGapDrawn = 0;
		}
	}

	// The ground is built in (see build), so a change rebuilds every system. Not here:
	// the menu calls this from the frame's present, and dropping a system frees game nodes. The rebuild waits
	// for the next update (rebuildIfPending), where the menu's Reset physics does its work too.
	void ActorManager::applyCollision()
	{
		if ((int)gSettings.ground == m_builtGround)
			return;
		m_builtGround = gSettings.ground;
		m_rebuildPending = true;
	}

	void ActorManager::rebuildIfPending()
	{
		if (!m_rebuildPending)
			return;
		m_rebuildPending = false;
		if (m_items.empty())
			return;
		logger::info("[SMP] collision settings changed (ground {}): rebuilding", gSettings.ground ? "on" : "off");
		clear();
	}

	void ActorManager::clear()
	{
		for (auto& [key, item] : m_items)
			if (item.system)
				{ ReleaseCpuSkins(item); hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(item.system.get()); }
		m_items.clear();
		stats = {};
	}
}
