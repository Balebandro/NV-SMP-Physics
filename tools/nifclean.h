#pragma once
// Removing a copy of the game's skeleton from an SMP NIF (shared by smpclean and the previewer).
//
// NIFs made in other tools often carry a whole skeleton (Bip01, the spine, Camera3rd, HeadAnims...) with the item's own
// bones nested in it. New Vegas hangs a hair's or an item's NIF under the actor, so those copies end up in the actor's
// scene beside the real skeleton: a skin slot bound by name can land on a copy (deep in the copied skeleton, off the
// head and never animated) and the item draws away from the body. The working layout is the one smpdress writes: the
// skeleton bones a skin uses sit flat on the NIF's root at their place, the item's own bones under them (or on the root).
//
// What stays:
//   - every shape whose name is not the skeleton's (a copied skeleton's own shapes, HeadAnims, go), with all it uses;
//   - every other node that is used: a kept skin is bound to it, an "SMP Bones" list or the physics XML names it, or it
//     holds something that stays (a custom body skeleton's extra bones, face bones and the like go when nothing uses them),
//     with its place in the tree among the nodes that stay;
//   - a skeleton node a kept skin is bound to, or that holds a node or shape that stays: moved onto the root, at its place.
// Everything else of the copied skeleton goes, and every block only it used. The world place of everything kept is the
// same as before. Blocks are renumbered; a block type whose references are not known here stops the clean (nothing
// written) rather than guessing.
#include "nif.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace nifclean
{
	using namespace nif;

	inline std::string lower(std::string s)
	{
		for (auto& c : s) c = (char)tolower((unsigned char)c);
		return s;
	}

	inline std::string nameOf(const File& f, int block)
	{
		if (block < 0 || block >= (int)f.blocks.size() || f.blocks[block].size() < 4) return "";
		Reader r(f.blocks[block].data(), f.blocks[block].size());
		uint32_t s = r.u32();
		return s < f.strings.size() ? f.strings[s] : "";
	}

	// The names in the game's skeleton (nodes and shapes, lower case; "HeadAnims:0" also as "headanims").
	inline std::set<std::string> skeletonNames(const std::string& skeletonPath)
	{
		File skel;
		skel.load(skeletonPath);
		std::set<std::string> out;
		for (size_t i = 0; i < skel.blocks.size(); i++) {
			const std::string& t = skel.typeOf(i);
			if (!isNodeType(t) && t != "NiTriShape" && t != "NiTriStrips") continue;
			std::string n = lower(nameOf(skel, (int)i));
			if (n.empty()) continue;
			out.insert(n);
			if (auto colon = n.find(':'); colon != std::string::npos) out.insert(n.substr(0, colon));
		}
		return out;
	}

	// The bone names a physics XML uses (lower case): bone and shape names, constraint bodies, the bones its elements name.
	inline std::set<std::string> xmlBoneNames(const std::string& xml)
	{
		std::set<std::string> out;
		std::regex attr("\\b(?:name|bodyA|bodyB|bone)\\s*=\\s*\"([^\"]+)\""), text("<([A-Za-z-]*bone[A-Za-z-]*)>\\s*([^<]+?)\\s*</\\1>");
		for (std::sregex_iterator it(xml.begin(), xml.end(), attr), end; it != end; ++it) out.insert(lower((*it)[1].str()));
		for (std::sregex_iterator it(xml.begin(), xml.end(), text), end; it != end; ++it) out.insert(lower((*it)[2].str()));
		return out;
	}

	// Where each block's references sit, for the types SMP NIFs use. false: a type not known here.
	struct Refs
	{
		std::vector<size_t> at;   // byte offsets of 32-bit block references
	};

	inline bool netRefs(const Bytes& b, Refs& out, size_t& end)   // NiObjectNET: name, extra data, controller
	{
		Reader r(b.data(), b.size());
		r.u32();
		uint32_t n = r.u32();
		for (uint32_t i = 0; i < n; i++) { out.at.push_back(r.pos); r.i32(); }
		out.at.push_back(r.pos); r.i32();
		end = r.pos;
		return true;
	}

	inline bool geomDataRefs(const Bytes& b, bool strips, Refs& out)
	{
		Reader r(b.data(), b.size());
		r.i32();
		uint16_t nv = r.u16();
		r.u8(); r.u8();
		if (r.u8()) r.skip(nv * 12);
		uint16_t bsFlags = r.u16();
		if (r.u8()) { r.skip(nv * 12); if (bsFlags & 4096) r.skip(nv * 24); }
		r.skip(16);
		if (r.u8()) r.skip(nv * 16);
		if (bsFlags & 1) r.skip((size_t)nv * 8);   // the uv set (as GeometryData reads it)
		r.u16();
		out.at.push_back(r.pos);   // additional data
		(void)strips;
		return true;
	}

	inline bool refsOf(const File& f, int block, Refs& out)
	{
		const std::string& t = f.typeOf(block);
		const Bytes& b = f.blocks[block];
		if (t == "NiNode" || t == "BSFadeNode") {
			AVObject av;
			Reader r(b.data(), b.size());
			av.read(r, true);
			if (av.tailPos != b.size()) return false;
			Reader q(b.data(), b.size());
			q.u32();
			uint32_t n = q.u32(); for (uint32_t i = 0; i < n; i++) { out.at.push_back(q.pos); q.i32(); }
			out.at.push_back(q.pos); q.i32();          // controller
			q.u32(); q.skip(12 + 36 + 4);              // flags, transform
			n = q.u32(); for (uint32_t i = 0; i < n; i++) { out.at.push_back(q.pos); q.i32(); }
			out.at.push_back(q.pos); q.i32();          // collision
			n = q.u32(); for (uint32_t i = 0; i < n; i++) { out.at.push_back(q.pos); q.i32(); }
			n = q.u32(); for (uint32_t i = 0; i < n; i++) { out.at.push_back(q.pos); q.i32(); }
			return true;
		}
		if (t == "NiTriShape" || t == "NiTriStrips") {
			Geometry g;
			g.read(b);
			Reader q(b.data(), b.size());
			q.u32();
			uint32_t n = q.u32(); for (uint32_t i = 0; i < n; i++) { out.at.push_back(q.pos); q.i32(); }
			out.at.push_back(q.pos); q.i32();
			q.u32(); q.skip(12 + 36 + 4);
			n = q.u32(); for (uint32_t i = 0; i < n; i++) { out.at.push_back(q.pos); q.i32(); }
			out.at.push_back(q.pos); q.i32();          // collision
			out.at.push_back(q.pos); q.i32();          // data
			out.at.push_back(q.pos); q.i32();          // skin instance
			return true;
		}
		if (t == "NiTriShapeData") return geomDataRefs(b, false, out);
		if (t == "NiTriStripsData") return geomDataRefs(b, true, out);
		if (t == "NiSkinInstance" || t == "BSDismemberSkinInstance") {
			Reader q(b.data(), b.size());
			out.at.push_back(q.pos); q.i32();
			out.at.push_back(q.pos); q.i32();
			out.at.push_back(q.pos); q.i32();
			uint32_t n = q.u32(); for (uint32_t i = 0; i < n; i++) { out.at.push_back(q.pos); q.i32(); }
			return true;
		}
		if (t == "NiSkinData" || t == "NiSkinPartition" || t == "BSShaderTextureSet" || t == "NiStringExtraData" || t == "NiIntegerExtraData" ||
			t == "NiFloatExtraData" || t == "NiBinaryExtraData" || t == "BSXFlags" || t == "BSBound")
			return true;   // no references
		if (t == "NiMaterialProperty" || t == "NiAlphaProperty" || t == "NiStencilProperty" || t == "NiSpecularProperty" ||
			t == "NiVertexColorProperty" || t == "NiZBufferProperty" || t == "NiWireframeProperty" || t == "NiDitherProperty" ||
			t == "NiShadeProperty" || t == "BSShaderNoLightingProperty") {
			size_t end;
			return netRefs(b, out, end);
		}
		if (t == "BSShaderPPLightingProperty") {
			size_t end;
			netRefs(b, out, end);
			out.at.push_back(end + 2 + 4 + 4 + 4 + 4 + 4);   // flags, type, flags 1 and 2, env map scale, clamp: the texture set
			return true;
		}
		return false;
	}

	inline int32_t refAt(const Bytes& b, size_t p) { int32_t v; memcpy(&v, b.data() + p, 4); return v; }
	inline void setRef(Bytes& b, size_t p, int32_t v) { memcpy(b.data() + p, &v, 4); }

	struct Report
	{
		bool ok = false;
		std::string error;
		std::vector<std::string> removed;    // node and shape names taken out
		std::vector<std::string> flattened;  // skeleton nodes kept, moved onto the root
		size_t blocksBefore = 0, blocksAfter = 0;
		bool changes() const { return !removed.empty() || !flattened.empty(); }
	};

	inline bool compact(File& f, Report& rep, const std::vector<int>& kept);

	// Cleans f in place (dryRun: only says what it would do). xmlNames: the bone names the physics XML uses (lower case), if
	// it is at hand; the "SMP Bones" lists in the file are read here.
	inline Report clean(File& f, const std::set<std::string>& skeleton, bool dryRun, const std::set<std::string>& xmlNames = {})
	{
		Report rep;
		rep.blocksBefore = f.blocks.size();
		if (f.roots.empty() || f.roots[0] < 0 || !isNodeType(f.typeOf(f.roots[0]))) { rep.error = "no root node"; return rep; }
		const int root = f.roots[0];
		auto isGeom = [&](int b) { const std::string& t = f.typeOf(b); return t == "NiTriShape" || t == "NiTriStrips"; };
		auto isNode = [&](int b) { return isNodeType(f.typeOf(b)); };

		// The tree: each object's parent, world place, and whether its name is the skeleton's.
		std::map<int, AVObject> av;
		std::map<int, int> parent;
		std::map<int, Transform> world;
		std::vector<int> order;   // depth first, as the file has it
		std::function<void(int, int, const Transform&)> walk = [&](int b, int from, const Transform& up) {
			if (b < 0 || b >= (int)f.blocks.size() || av.count(b)) return;
			AVObject o;
			Reader r(f.blocks[b].data(), f.blocks[b].size());
			o.read(r, isNode(b));
			av[b] = o;
			parent[b] = from;
			world[b] = up * o.local;
			order.push_back(b);
			if (o.isNode) for (int c : o.children) if (c >= 0 && (isNode(c) || isGeom(c))) walk(c, b, world[b]);
		};
		walk(root, -1, Transform{});
		auto skelName = [&](int b) {
			std::string n = lower(nameOf(f, b));
			if (skeleton.count(n)) return true;
			auto colon = n.find(':');
			return colon != std::string::npos && skeleton.count(n.substr(0, colon));
		};

		// The names the item uses: its "SMP Bones" lists' and the physics XML's.
		std::set<std::string> named = xmlNames;
		for (size_t b = 0; b < f.blocks.size(); b++) {
			if (f.typeOf(b) != "NiStringExtraData" || f.blocks[b].size() < 8) continue;
			Reader r(f.blocks[b].data(), f.blocks[b].size());
			uint32_t k = r.u32(), v = r.u32();
			if (k >= f.strings.size() || v >= f.strings.size() || f.strings[k].rfind("SMP Bones", 0) != 0) continue;
			const std::string& list = f.strings[v];
			for (size_t p = 0; p < list.size();) {
				size_t end = list.find(';', p);
				if (end == std::string::npos) end = list.size();
				std::string entry = list.substr(p, end - p);
				if (!entry.empty()) named.insert(lower(entry.substr(0, entry.find('|'))));
				p = end + 1;
			}
		}

		// What stays: shapes that are not the skeleton's, and the bones their skins use; then, children first, every node
		// that is used or holds something that stays.
		std::set<int> keep{ root };
		std::set<int> boundBones;
		for (int b : order) {
			if (b == root || !isGeom(b) || skelName(b)) continue;
			keep.insert(b);
			Geometry g;
			g.read(f.blocks[b]);
			if (g.skin >= 0 && g.skin < (int)f.blocks.size()) {
				SkinInstance s;
				s.read(f.blocks[g.skin], f.typeOf(g.skin) == "BSDismemberSkinInstance");
				for (int x : s.bones) boundBones.insert(x);
			}
		}
		std::set<int> flat;   // skeleton nodes kept, onto the root
		for (auto it = order.rbegin(); it != order.rend(); ++it) {
			int b = *it;
			if (b == root || !isNode(b)) continue;
			bool holds = false;   // a kept node or shape directly under it (one moved onto the root does not count)
			for (int c : av[b].children) holds |= keep.count(c) > 0 && !flat.count(c);
			if (skelName(b)) {
				// A skeleton bone: kept (onto the root) when a skin uses it or it holds an item node or shape.
				bool holdsItem = false;
				for (int c : av[b].children) holdsItem |= keep.count(c) > 0 && !skelName(c);
				if (boundBones.count(b) || holdsItem) { keep.insert(b); flat.insert(b); }
			} else if (boundBones.count(b) || named.count(lower(nameOf(f, b))) || holds)
				keep.insert(b);
		}
		for (int b : order)
			if (b != root && !keep.count(b)) rep.removed.push_back(nameOf(f, b));
		for (int b : flat)
			if (parent[b] != root) rep.flattened.push_back(nameOf(f, b));
		if (dryRun || !rep.changes()) { rep.ok = true; rep.blocksAfter = rep.blocksBefore; return rep; }

		// The new tree: each kept object under its nearest kept ancestor (a flattened skeleton node: the root), at the same
		// world place.
		std::map<int, int> newParent;
		for (int b : order) {
			if (b == root || !keep.count(b)) continue;
			int p = parent[b];
			if (flat.count(b)) p = root;
			else while (p != root && !keep.count(p)) p = parent[p];
			newParent[b] = p;
		}
		std::map<int, std::vector<int>> kids;
		for (int b : order)
			if (newParent.count(b)) kids[newParent[b]].push_back(b);
		for (int b : order) {
			if (!keep.count(b)) continue;
			AVObject o = av[b];
			if (b != root && newParent[b] != parent[b])
				o.local = world[newParent[b]].inverse() * world[b];
			if (o.isNode) {
				std::vector<int32_t> others;   // children that are not tree objects (none expected) stay
				for (int c : av[b].children) if (c >= 0 && !(isNode(c) || isGeom(c))) others.push_back(c);
				o.children.assign(kids[b].begin(), kids[b].end());
				o.children.insert(o.children.end(), others.begin(), others.end());
			}
			Writer w;
			o.write(w);
			const Bytes& old = f.blocks[b];
			w.raw(old.data() + av[b].tailPos, old.size() - av[b].tailPos);
			f.blocks[b] = w.b;
		}
		for (int b : order)   // what goes is unlinked from what it held (its blocks may still be reached by a kept one)
			if (!keep.count(b) && isNode(b)) {
				AVObject o = av[b];
				o.children.clear();
				Writer w;
				o.write(w);
				w.raw(f.blocks[b].data() + av[b].tailPos, f.blocks[b].size() - av[b].tailPos);
				f.blocks[b] = w.b;
			}

		std::vector<int> kept;
		for (int b : order) if (keep.count(b)) kept.push_back(b);
		if (!compact(f, rep, kept)) return rep;
		rep.ok = true;
		return rep;
	}

	// Every block reached from the root (and the footer's other roots) through known references stays, renumbered; the rest
	// goes. `kept`: blocks that must still be reached (a kept object that is not means something is wrong: nothing changes).
	// False (rep.error set, f untouched) on a block type whose references are not known here.
	inline bool compact(File& f, Report& rep, const std::vector<int>& kept)
	{
		std::vector<Refs> refs(f.blocks.size());
		std::vector<char> reached(f.blocks.size(), 0);
		std::vector<int> stack;
		for (int r : f.roots) if (r >= 0) stack.push_back(r);
		while (!stack.empty()) {
			int b = stack.back();
			stack.pop_back();
			if (b < 0 || b >= (int)f.blocks.size() || reached[b]) continue;
			if (!refsOf(f, b, refs[b])) { rep.error = "block " + std::to_string(b) + " is a " + f.typeOf(b) + ", which this cleaner does not know: nothing changed"; return false; }
			reached[b] = 1;
			for (size_t p : refs[b].at) {
				int32_t x = refAt(f.blocks[b], p);
				if (x >= 0 && x < (int)f.blocks.size()) stack.push_back(x);
			}
		}
		for (int b : kept) if (!reached[b]) { rep.error = "a kept object was not reached: nothing changed"; return false; }
		for (size_t b = 0; b < f.blocks.size(); b++)
			if (reached[b] && (f.typeOf(b) == "NiTriShapeData" || f.typeOf(b) == "NiTriStripsData") && refAt(f.blocks[b], refs[b].at.back()) >= 0) {
				rep.error = "a shape has additional geometry data, which this cleaner does not handle: nothing changed";
				return false;
			}

		std::vector<int32_t> to(f.blocks.size(), -1);
		File out = f;
		out.blocks.clear();
		out.blockTypeIndex.clear();
		for (size_t b = 0; b < f.blocks.size(); b++)
			if (reached[b]) { to[b] = (int32_t)out.blocks.size(); out.blocks.push_back(f.blocks[b]); out.blockTypeIndex.push_back(f.blockTypeIndex[b]); }
		for (size_t b = 0; b < f.blocks.size(); b++) {
			if (!reached[b]) continue;
			Bytes& nb = out.blocks[to[b]];
			for (size_t p : refs[b].at) {
				int32_t x = refAt(nb, p);
				if (x >= 0 && x < (int)f.blocks.size()) setRef(nb, p, to[x]);
			}
		}
		out.roots.clear();
		for (int r : f.roots) if (r >= 0 && reached[r]) out.roots.push_back(to[r]);
		f = std::move(out);
		rep.blocksAfter = f.blocks.size();
		return true;
	}

	// The shapes (NiTriShape / NiTriStrips) in tree order from the root, as the previewer lists them, with their parents.
	inline std::vector<std::pair<int, int>> shapesInTree(const File& f)
	{
		std::vector<std::pair<int, int>> out;   // shape block, parent node block
		std::set<int> seen;
		std::function<void(int)> walk = [&](int b) {
			if (b < 0 || b >= (int)f.blocks.size() || !seen.insert(b).second || !isNodeType(f.typeOf(b))) return;
			AVObject o;
			Reader r(f.blocks[b].data(), f.blocks[b].size());
			o.read(r, true);
			for (int c : o.children) {
				if (c < 0 || c >= (int)f.blocks.size()) continue;
				const std::string& t = f.typeOf(c);
				if (t == "NiTriShape" || t == "NiTriStrips") out.push_back({ c, b });
				else walk(c);
			}
		};
		if (!f.roots.empty()) walk(f.roots[0]);
		return out;
	}

	// The shape block named `name` (its `occurrence`-th in tree order when several share the name), or -1.
	inline int findShape(const File& f, const std::string& name, int occurrence, int* parent = nullptr)
	{
		int seen = 0;
		for (auto& [b, p] : shapesInTree(f))
			if (lower(nameOf(f, b)) == lower(name) && seen++ == occurrence) { if (parent) *parent = p; return b; }
		return -1;
	}

	// Takes shapes out of the file: unlinked from their parents, then every block only they used goes (compact).
	// targets: name and occurrence (see findShape).
	inline Report removeShapes(File& f, const std::vector<std::pair<std::string, int>>& targets)
	{
		Report rep;
		rep.blocksBefore = f.blocks.size();
		std::map<int, std::set<int>> drop;   // parent node -> its shapes that go
		for (auto& [name, occurrence] : targets) {
			int parent = -1, b = findShape(f, name, occurrence, &parent);
			if (b < 0) { rep.error = "no shape " + name + " in the NIF: nothing changed"; return rep; }
			drop[parent].insert(b);
			rep.removed.push_back(name);
		}
		File edited = f;
		for (auto& [parent, shapes] : drop) {
			AVObject o;
			Reader r(edited.blocks[parent].data(), edited.blocks[parent].size());
			o.read(r, true);
			std::vector<int32_t> children;
			for (int c : o.children) if (!shapes.count(c)) children.push_back(c);
			o.children = children;
			Writer w;
			o.write(w);
			const Bytes& old = edited.blocks[parent];
			w.raw(old.data() + o.tailPos, old.size() - o.tailPos);
			edited.blocks[parent] = w.b;
		}
		std::vector<int> kept;
		for (auto& [b, p] : shapesInTree(edited)) kept.push_back(b);
		if (!compact(edited, rep, kept)) return rep;
		f = std::move(edited);
		rep.ok = true;
		return rep;
	}

	// The game hangs an outfit under a node of its own only when its root's second child is a shape
	// (BipedAnim::AttachSkinnedObject 0x4AD4A0); otherwise the shapes go loose under the actor and taking the item off
	// removes only the first, the rest staying on the body. Puts the root's shapes before its other children (each kept
	// in its order). Returns whether that was needed: false when the layout already works or the root holds fewer than
	// two shapes (no order fixes that). dryRun only checks.
	inline bool shapesFirst(File& f, bool dryRun)
	{
		if (f.roots.empty() || f.roots[0] < 0 || !isNodeType(f.typeOf(f.roots[0]))) return false;
		const int root = f.roots[0];
		Reader r(f.blocks[root].data(), f.blocks[root].size());
		AVObject o;
		o.read(r, true);
		auto isShape = [&](int32_t c) { return c >= 0 && c < (int)f.blocks.size() && (f.typeOf(c) == "NiTriShape" || f.typeOf(c) == "NiTriStrips"); };
		std::vector<int32_t> shapes, rest;
		for (int32_t c : o.children) (isShape(c) ? shapes : rest).push_back(c);
		if (shapes.size() < 2 || isShape(o.children[1])) return false;
		if (dryRun) return true;
		o.children = shapes;
		o.children.insert(o.children.end(), rest.begin(), rest.end());
		Writer w;
		o.write(w);
		w.raw(f.blocks[root].data() + o.tailPos, f.blocks[root].size() - o.tailPos);
		f.blocks[root] = w.b;
		return true;
	}
}
