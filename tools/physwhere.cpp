// physwhere: which block of each NIF carries the physics file string ("HDT Skinned Mesh Physics Object"): the root
// node or a shape. The game copies a root's extra data onto the actor's root when it attaches armour (2026-10-31: a
// physics file on the root stayed after unequip).
//
//   physwhere file.nif [file.nif ...]
#include "nif.h"
#include <cstdio>

int main(int argc, char** argv)
{
	for (int a = 1; a < argc; a++) {
		nif::File f;
		try { f.load(argv[a]); } catch (std::exception& e) { printf("%s: %s\n", argv[a], e.what()); continue; }
		std::vector<int> physics;   // NiStringExtraData blocks named the physics key
		for (size_t i = 0; i < f.blocks.size(); i++) {
			if (f.typeOf(i) != "NiStringExtraData") continue;
			nif::Reader r(f.blocks[i].data(), f.blocks[i].size());
			const uint32_t name = r.u32();
			if (name < f.strings.size() && f.strings[name] == "HDT Skinned Mesh Physics Object") physics.push_back((int)i);
		}
		if (physics.empty()) { printf("%s: no physics string\n", argv[a]); continue; }
		for (size_t i = 0; i < f.blocks.size(); i++) {
			const std::string& t = f.typeOf(i);
			const bool node = t == "NiNode" || t == "BSFadeNode";
			if (!node && t != "NiTriShape" && t != "NiTriStrips") continue;
			nif::Reader r(f.blocks[i].data(), f.blocks[i].size());
			nif::AVObject o;
			o.read(r, node);
			for (int e : o.extraData)
				for (int p : physics)
					if (e == p) {
						const bool root = !f.roots.empty() && f.roots[0] == (int)i;
						printf("%s: on %s %s \"%s\"\n", argv[a], root ? "the ROOT" : (node ? "node" : "shape"), t.c_str(), o.name < f.strings.size() ? f.strings[o.name].c_str() : "?");
					}
		}
	}
}
