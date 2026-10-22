// smpclean: takes a copy of the game's skeleton out of an SMP NIF (see nifclean.h), keeping the shapes, the item's own
// bones and the skeleton bones its skins use (flat on the root).
//
//   smpclean in.nif skeleton.nif out.nif [physics.xml]     (out may be in)
//   smpclean --check in.nif skeleton.nif [physics.xml]     (only says what it would do)
// physics.xml: the item's physics file, so the bones only it names stay too.
#include "nifclean.h"
#include <cstdio>
#include <fstream>
#include <sstream>

int main(int argc, char** argv)
{
	const bool check = argc >= 2 && std::string(argv[1]) == "--check";
	const int first = check ? 2 : 1;
	if (argc < first + (check ? 2 : 3)) {
		printf("usage: smpclean in.nif skeleton.nif out.nif [physics.xml]\n       smpclean --check in.nif skeleton.nif [physics.xml]\n");
		return 1;
	}
	const char* in = argv[first];
	const char* skel = argv[first + 1];
	const char* out = check ? nullptr : argv[first + 2];
	const int xmlArg = first + (check ? 2 : 3);
	try {
		std::set<std::string> xmlNames;
		if (argc > xmlArg) {
			std::ifstream x(argv[xmlArg], std::ios::binary);
			if (!x) { printf("error: cannot read %s\n", argv[xmlArg]); return 1; }
			std::stringstream text;
			text << x.rdbuf();
			xmlNames = nifclean::xmlBoneNames(text.str());
		}
		nif::File f;
		f.load(in);
		auto rep = nifclean::clean(f, nifclean::skeletonNames(skel), check, xmlNames);
		if (!rep.ok) { printf("error: %s\n", rep.error.c_str()); return 1; }
		printf("%zu object(s) of a copied skeleton %s", rep.removed.size(), check ? "to remove" : "removed");
		for (size_t i = 0; i < rep.removed.size() && i < 12; i++) printf("%s%s", i ? ", " : ": ", rep.removed[i].c_str());
		if (rep.removed.size() > 12) printf(", ...");
		printf("\n%zu skeleton bone(s) the skins use %s onto the root", rep.flattened.size(), check ? "to move" : "moved");
		for (size_t i = 0; i < rep.flattened.size(); i++) printf("%s%s", i ? ", " : ": ", rep.flattened[i].c_str());
		printf("\n");
		if (check) return 0;
		if (!rep.changes()) { printf("nothing to clean: %s left as it was\n", in); return 0; }
		f.save(out);
		printf("wrote %s (%zu blocks, was %zu)\n", out, rep.blocksAfter, rep.blocksBefore);
		return 0;
	} catch (const std::exception& e) {
		printf("error: %s\n", e.what());
		return 1;
	}
}
