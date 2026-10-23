// nifgraft: copies unskinned shapes from one NIF into another, under its root (a weapon charm made in its own NIF onto
// the weapon), where they sit in the source (their parents' transforms composed onto them).
//   nifgraft target.nif source.nif out.nif "Shape"...
// Each shape comes with its geometry data, properties (a shader's texture set too) and string extra data. Skinned
// shapes and shapes with controllers are refused. Run smpdress on the result to give a grafted shape physics.
#include "nif.h"
#include <cstdio>
#include <map>
using namespace nif;

static std::string strName(const File& f, int block)
{
	Reader r(f.blocks[block].data(), f.blocks[block].size());
	uint32_t s = r.u32();
	return s < f.strings.size() ? f.strings[s] : "";
}

int main(int argc, char** argv)
{
	if (argc < 5) { printf("usage: nifgraft target.nif source.nif out.nif \"Shape\"...\n"); return 1; }
	try {
		File dst, src;
		dst.load(argv[1]);
		src.load(argv[2]);
		auto str = [&](uint32_t s) { return s < src.strings.size() ? dst.addString(src.strings[s]) : 0xFFFFFFFFu; };
		// Each source node's parent, for the transforms down to a shape.
		std::map<int, int> parentOf;
		for (size_t n = 0; n < src.blocks.size(); n++) {
			if (!isNodeType(src.typeOf(n))) continue;
			Reader r(src.blocks[n].data(), src.blocks[n].size());
			AVObject node;
			node.read(r, true);
			for (auto c : node.children) if (c >= 0) parentOf[c] = (int)n;
		}
		std::map<int, int> copied;   // source block -> target block
		// A block copied as it is, with the string at the start (NiObjectNET's name) mapped when `named`.
		auto copyRaw = [&](int b, bool named) {
			if (copied.count(b)) return copied[b];
			Bytes d = src.blocks[b];
			if (named) { uint32_t s = str(*(uint32_t*)d.data()); memcpy(d.data(), &s, 4); }
			return copied[b] = dst.addBlock(src.typeOf(b), d);
		};
		AVObject root;
		Reader rr(dst.blocks[dst.roots[0]].data(), dst.blocks[dst.roots[0]].size());
		root.read(rr, true);
		Bytes rootTail(dst.blocks[dst.roots[0]].begin() + root.tailPos, dst.blocks[dst.roots[0]].end());
		for (int a = 4; a < argc; a++) {
			const std::string name = argv[a];
			int blk = -1;
			for (size_t n = 0; n < src.blocks.size() && blk < 0; n++)
				if ((src.typeOf(n) == "NiTriStrips" || src.typeOf(n) == "NiTriShape") && strName(src, (int)n) == name) blk = (int)n;
			if (blk < 0) throw std::runtime_error("shape not found in the source: " + name);
			Geometry g;
			g.read(src.blocks[blk]);
			if (g.skin >= 0) throw std::runtime_error(name + " is skinned: graft it before giving it physics");
			if (g.av.controller >= 0) throw std::runtime_error(name + " has a controller");
			if (g.rest.size() < 4 || *(uint32_t*)g.rest.data() != 0) throw std::runtime_error(name + " has material names (not handled)");
			for (int p = parentOf.count(blk) ? parentOf[blk] : -1; p >= 0; p = parentOf.count(p) ? parentOf[p] : -1) {
				Reader r(src.blocks[p].data(), src.blocks[p].size());
				AVObject node;
				node.read(r, true);
				g.av.local = node.local * g.av.local;
			}
			g.av.name = str(g.av.name);
			g.av.collision = -1;
			for (auto& e : g.av.extraData) {
				if (src.typeOf(e) != "NiStringExtraData") throw std::runtime_error(name + ": extra data of type " + src.typeOf(e) + " (not handled)");
				Bytes d = src.blocks[e];
				uint32_t k = str(*(uint32_t*)d.data()), v = str(*(uint32_t*)(d.data() + 4));
				memcpy(d.data(), &k, 4); memcpy(d.data() + 4, &v, 4);
				e = dst.addBlock("NiStringExtraData", d);
			}
			for (auto& p : g.av.properties) {
				const std::string t = src.typeOf(p);
				if (t == "BSShaderPPLightingProperty") {
					// Its texture set: after name, extra data, controller, flags, shader type, shader flags, two ints and
					// the environment map scale.
					Reader r(src.blocks[p].data(), src.blocks[p].size());
					r.u32(); uint32_t ne = r.u32(); r.skip(4 * ne);
					if (r.i32() >= 0 || ne) throw std::runtime_error(name + ": its shader has extra data or a controller (not handled)");
					r.u16(); r.u32(); r.u32(); r.u32(); r.f32(); r.u32();
					const size_t at = r.pos;
					int32_t set = r.i32();
					int np = copyRaw(p, true);
					if (set >= 0) { int32_t ns = copyRaw(set, false); memcpy(dst.blocks[np].data() + at, &ns, 4); }
					p = np;
				}
				else if (t == "NiMaterialProperty" || t == "NiAlphaProperty" || t == "NiStencilProperty") {
					Reader r(src.blocks[p].data(), src.blocks[p].size());
					r.u32();
					if (r.u32() || r.i32() >= 0) throw std::runtime_error(name + ": its " + t + " has extra data or a controller (not handled)");
					p = copyRaw(p, true);
				}
				else throw std::runtime_error(name + ": property " + t + " not handled");
			}
			g.data = copyRaw(g.data, false);
			int nb = dst.addBlock(src.typeOf(blk), g.write());
			root.children.push_back(nb);
			printf("grafted %s (%s, at %.2f %.2f %.2f)\n", name.c_str(), src.typeOf(blk).c_str(), g.av.local.trans.x, g.av.local.trans.y, g.av.local.trans.z);
		}
		Writer w;
		root.write(w);
		w.raw(rootTail.data(), rootTail.size());
		dst.blocks[dst.roots[0]] = w.b;
		dst.save(argv[3]);
		printf("wrote %s\n", argv[3]);
	} catch (std::exception& e) {
		printf("error: %s\n", e.what());
		return 2;
	}
	return 0;
}
