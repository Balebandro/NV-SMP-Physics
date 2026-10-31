// niftop: a NIF's root and its direct children (type, name, child count), to compare how outfits are laid out.
//
//   niftop file.nif [file.nif ...]
#include "nif.h"
#include <cstdio>

int main(int argc, char** argv)
{
	for (int a = 1; a < argc; a++) {
		nif::File f;
		try { f.load(argv[a]); } catch (std::exception& e) { printf("%s: %s\n", argv[a], e.what()); continue; }
		auto name = [&](int i) -> std::string {
			const std::string& t = f.typeOf(i);
			const bool node = t == "NiNode" || t == "BSFadeNode";
			if (!node && t != "NiTriShape" && t != "NiTriStrips") return t;
			nif::Reader r(f.blocks[i].data(), f.blocks[i].size());
			nif::AVObject o;
			o.read(r, node);
			std::string s = t + " \"" + (o.name < f.strings.size() ? f.strings[o.name] : "?") + "\"";
			if (node) s += " (" + std::to_string(o.children.size()) + " children)";
			return s;
		};
		const int root = f.roots.empty() ? -1 : f.roots[0];
		printf("%s\n  root: %s\n", argv[a], root >= 0 ? name(root).c_str() : "?");
		if (root < 0) continue;
		nif::Reader r(f.blocks[root].data(), f.blocks[root].size());
		nif::AVObject o;
		o.read(r, true);
		for (int c : o.children) printf("    %s\n", c >= 0 ? name(c).c_str() : "(none)");
	}
}
