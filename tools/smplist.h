#pragma once
// Writing what NV SMP Physics needs into an SMP NIF made outside these tools (a Skyrim body or outfit rigged with
// extra bones): shared by smprepair --list and NV SMP Prepare.
//
//   "SMP Bones" on every skinned shape with bones the game's skeleton lacks (New Vegas drops them on attach; the
//     plugin rebuilds them from this list): "name|parent;" per skin slot, in slot order. A bone the skeleton has is
//     the body's ("name|"); one it lacks gets its parent node in the NIF, or, sitting on the NIF's root (as Skyrim
//     bodies have them), the bone the physics XML's constraints hang it from (a hair chain: the bone above), else the
//     skeleton bone it shares most weight with (a breast bone: the chest).
//   "HDT Skinned Mesh Physics Object" on the root: the physics XML, relative to Data.
//   The keep flags on the geometry data the physics reads (XYZ and bone data kept after upload), else the game drops
//     the vertices and the plugin has nothing to collide with.
#include "nif.h"
#include <map>
#include <queue>
#include <regex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace smplist
{
	using namespace nif;

	inline std::string strName(const File& f, int block)
	{
		if (block < 0 || block >= (int)f.blocks.size()) return "";
		Reader r(f.blocks[block].data(), f.blocks[block].size());
		uint32_t s = r.u32();
		return s < f.strings.size() ? f.strings[s] : "";
	}

	inline std::pair<std::string, std::string> stringExtra(const File& f, int block)
	{
		if (f.typeOf(block) != "NiStringExtraData") return {};
		Reader r(f.blocks[block].data(), f.blocks[block].size());
		uint32_t n = r.u32(), v = r.u32();
		return { n < f.strings.size() ? f.strings[n] : "", v < f.strings.size() ? f.strings[v] : "" };
	}

	inline bool isSmpBonesKey(const std::string& k) { return k == "SMP Bones" || k.rfind("SMP Bones ", 0) == 0; }

	inline void addStringExtra(File& f, AVObject& av, const std::string& name, const std::string& value)
	{
		Writer w;
		w.u32(f.addString(name));
		w.u32(f.addString(value));
		av.extraData.push_back(f.addBlock("NiStringExtraData", w.b));
	}

	// In pieces under 2000 characters, split between entries (Outfit Studio cannot read 2048 or more).
	inline void addSmpBones(File& f, AVObject& av, const std::string& list)
	{
		size_t at = 0;
		for (int piece = 1; at < list.size() || piece == 1; ++piece) {
			size_t end = list.size();
			if (end - at > 2000) end = list.rfind(';', at + 2000) + 1;
			addStringExtra(f, av, piece == 1 ? "SMP Bones" : "SMP Bones " + std::to_string(piece), list.substr(at, end - at));
			at = end;
		}
	}

	inline std::set<std::string> skeletonBones(const std::string& skeletonPath)
	{
		File skel;
		skel.load(skeletonPath);
		std::set<std::string> out;
		for (size_t i = 0; i < skel.blocks.size(); i++) if (isNodeType(skel.typeOf(i))) out.insert(strName(skel, (int)i));
		return out;
	}

	struct ShapeReport
	{
		std::string shape;
		std::vector<std::string> own;   // "bone on parent"
		bool hadList = false;           // already listed: left as it was
	};

	// The skinned shapes and their bones the skeleton lacks (listed or not).
	inline std::vector<ShapeReport> analyse(const File& f, const std::set<std::string>& skeleton)
	{
		std::vector<ShapeReport> out;
		for (size_t si = 0; si < f.blocks.size(); si++) {
			const std::string& type = f.typeOf(si);
			if (type != "NiTriShape" && type != "NiTriStrips") continue;
			Geometry g;
			g.read(f.blocks[si]);
			if (g.skin < 0) continue;
			SkinInstance skin;
			skin.read(f.blocks[g.skin], f.typeOf(g.skin) == "BSDismemberSkinInstance");
			ShapeReport r;
			r.shape = strName(f, (int)si);
			for (int e : g.av.extraData) r.hadList |= isSmpBonesKey(stringExtra(f, e).first);
			for (int b : skin.bones) if (!skeleton.count(strName(f, b))) r.own.push_back(strName(f, b));
			if (!r.own.empty()) out.push_back(r);
		}
		return out;
	}

	// Each bone's parent by the physics XML's constraints (any element with bodyA and bodyB): from the skeleton bones
	// outward, a bone joined to a placed one hangs from it. Which of bodyA/bodyB is the parent varies between XMLs, so
	// the direction comes from the skeleton, not the attribute order. Bones no constraint reaches are left out.
	inline std::map<std::string, std::string> constraintParents(const std::string& xml, const std::set<std::string>& skeleton)
	{
		std::map<std::string, std::vector<std::string>> joined;
		std::regex tag("<[A-Za-z-]*constraint\\b[^>]*>"), a("\\bbodyA\\s*=\\s*\"([^\"]+)\""), b("\\bbodyB\\s*=\\s*\"([^\"]+)\"");
		for (std::sregex_iterator it(xml.begin(), xml.end(), tag), end; it != end; ++it) {
			std::string t = it->str();
			std::smatch ma, mb;
			if (!std::regex_search(t, ma, a) || !std::regex_search(t, mb, b)) continue;
			joined[ma[1].str()].push_back(mb[1].str());
			joined[mb[1].str()].push_back(ma[1].str());
		}
		std::map<std::string, std::string> parents;
		std::queue<std::string> open;
		for (auto& [name, others] : joined) if (skeleton.count(name)) open.push(name);
		while (!open.empty()) {
			std::string at = open.front();
			open.pop();
			for (auto& other : joined[at])
				if (!skeleton.count(other) && !parents.count(other)) { parents[other] = at; open.push(other); }
		}
		return parents;
	}

	// Writes "SMP Bones" on every skinned shape with bones the skeleton lacks and no list yet. Returns what it did.
	// xmlParents: constraintParents of the item's physics XML, for bones sitting on the NIF's root.
	inline std::vector<ShapeReport> addLists(File& f, const std::set<std::string>& skeleton, const std::map<std::string, std::string>& xmlParents = {})
	{
		std::map<int, int> parent;
		for (size_t i = 0; i < f.blocks.size(); i++) {
			if (!isNodeType(f.typeOf(i))) continue;
			AVObject n;
			Reader r(f.blocks[i].data(), f.blocks[i].size());
			n.read(r, true);
			for (int c : n.children) parent[c] = (int)i;
		}
		std::vector<ShapeReport> done;
		for (size_t si = 0; si < f.blocks.size(); si++) {
			const std::string& type = f.typeOf(si);
			if (type != "NiTriShape" && type != "NiTriStrips") continue;
			Geometry g;
			g.read(f.blocks[si]);
			if (g.skin < 0) continue;
			bool has = false;
			for (int e : g.av.extraData) has |= isSmpBonesKey(stringExtra(f, e).first);
			if (has) continue;
			SkinInstance skin;
			skin.read(f.blocks[g.skin], f.typeOf(g.skin) == "BSDismemberSkinInstance");
			SkinData sd;
			sd.read(f.blocks[skin.data]);
			std::map<int, std::map<int, float>> byVertex;
			for (size_t b = 0; b < sd.bones.size(); b++) for (auto& vw : sd.bones[b].weights) byVertex[vw.index][(int)b] = vw.weight;
			auto sharedParent = [&](int slot) {
				std::map<int, float> shared;
				for (auto& [v, ws] : byVertex) {
					auto me = ws.find(slot);
					if (me == ws.end()) continue;
					for (auto& [other, w] : ws) if (other != slot && skeleton.count(strName(f, skin.bones[other]))) shared[other] += std::min(w, me->second);
				}
				int best = -1; float bw = 0;
				for (auto& [o, w] : shared) if (w > bw) { bw = w; best = o; }
				return best >= 0 ? strName(f, skin.bones[best]) : std::string();
			};
			std::string list;
			ShapeReport rep;
			rep.shape = strName(f, (int)si);
			for (size_t slot = 0; slot < skin.bones.size(); slot++) {
				int b = skin.bones[slot];
				std::string name = strName(f, b), par;
				if (!skeleton.count(name)) {
					auto it = parent.find(b);
					par = it != parent.end() && it->second != f.roots[0] ? strName(f, it->second) : "";
					if (par.empty()) if (auto x = xmlParents.find(name); x != xmlParents.end()) par = x->second;
					if (par.empty()) par = sharedParent((int)slot);
					if (par.empty()) par = "Bip01";   // weighted alone: it stays with the skeleton's root
					rep.own.push_back(name + " on " + par);
				}
				list += name + "|" + par + ";";
			}
			if (rep.own.empty()) continue;   // only the body's bones: nothing for the plugin to rebuild
			addSmpBones(f, g.av, list);
			f.blocks[si] = g.write();
			done.push_back(rep);
		}
		return done;
	}

	// The root's physics file ("HDT Skinned Mesh Physics Object"), or empty.
	inline std::string physicsPath(const File& f)
	{
		AVObject root;
		Reader r(f.blocks[f.roots[0]].data(), f.blocks[f.roots[0]].size());
		root.read(r, true);
		for (int e : root.extraData) { auto [k, v] = stringExtra(f, e); if (k == "HDT Skinned Mesh Physics Object") return v; }
		return "";
	}

	inline void setPhysicsPath(File& f, const std::string& path)
	{
		AVObject root;
		Reader r(f.blocks[f.roots[0]].data(), f.blocks[f.roots[0]].size());
		root.read(r, true);
		Bytes tail(f.blocks[f.roots[0]].begin() + root.tailPos, f.blocks[f.roots[0]].end());
		addStringExtra(f, root, "HDT Skinned Mesh Physics Object", path);
		Writer w;
		root.write(w);
		w.raw(tail.data(), tail.size());
		f.blocks[f.roots[0]] = w.b;
	}

	// The keep flags (XYZ and bone data) on these shapes' geometry data. Returns how many it set.
	inline int keepVertexData(File& f, const std::set<std::string>& shapes)
	{
		int n = 0;
		for (size_t si = 0; si < f.blocks.size(); si++) {
			const std::string& type = f.typeOf(si);
			if (type != "NiTriShape" && type != "NiTriStrips") continue;
			if (!shapes.count(strName(f, (int)si))) continue;
			Geometry g;
			g.read(f.blocks[si]);
			if (g.data < 0 || f.blocks[g.data].size() <= 6) continue;
			if ((f.blocks[g.data][6] & 0x21) != 0x21) { f.blocks[g.data][6] |= 0x01 | 0x20; n++; }
		}
		return n;
	}
}
