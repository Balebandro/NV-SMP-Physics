// niforder: puts the named children of a NIF's root first, in the order given, the rest after as they were. The game
// attaches an outfit under a node of its own only when the root's second child is a shape (BipedAnim::AttachSkinnedObject
// 0x4AD4A0); otherwise its shapes go loose under the actor and only the first is taken off again (2026-10-31: the White
// Glove attire's dress stayed on after unequip).
//
//   niforder in.nif out.nif "First" "Second" ...
//   niforder in.nif out.nif --shapes-first       (every shape before the root's other children, in their order)
#include "nif.h"
#include <cstdio>

int main(int argc, char** argv)
{
	if (argc < 4) { printf("niforder in.nif out.nif name...\n"); return 1; }
	nif::File f;
	f.load(argv[1]);
	const int root = f.roots.empty() ? -1 : f.roots[0];
	if (root < 0) { printf("no root\n"); return 1; }
	nif::Reader r(f.blocks[root].data(), f.blocks[root].size());
	nif::AVObject o;
	o.read(r, true);
	auto nameOf = [&](int b) -> std::string {
		if (b < 0) return "";
		const std::string& t = f.typeOf(b);
		const bool node = t == "NiNode" || t == "BSFadeNode";
		if (!node && t != "NiTriShape" && t != "NiTriStrips") return "";
		nif::Reader rr(f.blocks[b].data(), f.blocks[b].size());
		nif::AVObject c;
		c.read(rr, node);
		return c.name < f.strings.size() ? f.strings[c.name] : "";
	};
	std::vector<int32_t> first, rest = o.children;
	if (argc == 4 && std::string(argv[3]) == "--shapes-first") {   // every shape first, in its order, then the rest
		for (auto it = rest.begin(); it != rest.end();)
			if (*it >= 0 && (f.typeOf(*it) == "NiTriShape" || f.typeOf(*it) == "NiTriStrips")) { first.push_back(*it); it = rest.erase(it); }
			else ++it;
		argc = 3;
	}
	for (int a = 3; a < argc; a++) {
		auto it = std::find_if(rest.begin(), rest.end(), [&](int32_t c) { return nameOf(c) == argv[a]; });
		if (it == rest.end()) { printf("no root child named \"%s\"\n", argv[a]); return 1; }
		first.push_back(*it);
		rest.erase(it);
	}
	o.children = first;
	o.children.insert(o.children.end(), rest.begin(), rest.end());
	nif::Writer w;
	o.write(w);
	w.raw(f.blocks[root].data() + o.tailPos, f.blocks[root].size() - o.tailPos);
	f.blocks[root] = w.b;
	f.save(argv[2]);
	for (size_t i = 0; i < o.children.size() && i < 8; i++) printf("  %zu: %s\n", i, nameOf(o.children[i]).c_str());
	return 0;
}
