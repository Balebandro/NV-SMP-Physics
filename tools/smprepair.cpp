// smprepair: makes an SMP NIF work again after an editor saved it (Outfit Studio removes every skin bone no vertex
// weights, with its node: the cloth's anchor rows, a rope's top link). "SMP Bones" names each skin slot's bone in slot
// order, which the plugin needs (New Vegas drops an item's own bone nodes on attach), so the saved file's list no longer
// matches its skin and the anchors the physics hangs the cloth from are gone.
//
//   smprepair edited.nif reference.nif out.nif
//
// reference.nif: the file before the edit (smpdress or smpify output). Per shape with "SMP Bones":
//   - every SMP bone of the old list missing from the skin goes back in, unweighted, its node and skin-to-bone taken
//     from the reference (bones without a parent in the list, the body's, are not needed and are left out);
//   - "SMP Bones" is written again in the skin's slot order, each bone's parent as before.
// Everything else (weights, geometry, edits) is kept.
#include "nif.h"
#include <cstdio>
#include <map>
using namespace nif;

static std::string strName(const File& f, int block)
{
	if (block < 0 || block >= (int)f.blocks.size()) return "";
	Reader r(f.blocks[block].data(), f.blocks[block].size());
	uint32_t s = r.u32();
	return s < f.strings.size() ? f.strings[s] : "";
}

static int findBlockByName(const File& f, const std::string& name, bool nodes)
{
	for (size_t i = 0; i < f.blocks.size(); i++) {
		bool node = isNodeType(f.typeOf(i));
		if (node == nodes && strName(f, (int)i) == name) return (int)i;
	}
	return -1;
}

// A string extra data's name and value.
static std::pair<std::string, std::string> stringExtra(const File& f, int block)
{
	if (f.typeOf(block) != "NiStringExtraData") return {};
	Reader r(f.blocks[block].data(), f.blocks[block].size());
	uint32_t n = r.u32(), v = r.u32();
	return { n < f.strings.size() ? f.strings[n] : "", v < f.strings.size() ? f.strings[v] : "" };
}

static bool isSmpBonesKey(const std::string& k) { return k == "SMP Bones" || k.rfind("SMP Bones ", 0) == 0; }

static void addStringExtra(File& f, AVObject& av, const std::string& name, const std::string& value)
{
	Writer w;
	w.u32(f.addString(name));
	w.u32(f.addString(value));
	av.extraData.push_back(f.addBlock("NiStringExtraData", w.b));
}

// As smpdress: pieces under 2000 characters, split between entries (Outfit Studio cannot read 2048 or more).
static void addSmpBones(File& f, AVObject& av, const std::string& list)
{
	size_t at = 0;
	for (int piece = 1; at < list.size() || piece == 1; ++piece) {
		size_t end = list.size();
		if (end - at > 2000) end = list.rfind(';', at + 2000) + 1;
		addStringExtra(f, av, piece == 1 ? "SMP Bones" : "SMP Bones " + std::to_string(piece), list.substr(at, end - at));
		at = end;
	}
}

int main(int argc, char** argv)
{
	if (argc < 4) { printf("usage: smprepair edited.nif reference.nif out.nif\n"); return 1; }
	try {
		File f, ref;
		f.load(argv[1]);
		ref.load(argv[2]);
		AVObject root;
		{ Reader r(f.blocks[0].data(), f.blocks[0].size()); root.read(r, true); }
		const Bytes rootTail(f.blocks[0].begin() + root.tailPos, f.blocks[0].end());
		bool rootChanged = false;
		int shapes = 0;

		for (size_t si = 0; si < f.blocks.size(); si++) {
			const std::string& type = f.typeOf(si);
			if (type != "NiTriShape" && type != "NiTriStrips") continue;
			Geometry g;
			g.read(f.blocks[si]);
			// The old list, joined from its pieces.
			std::string list;
			std::map<int, std::string> pieces;
			for (int e : g.av.extraData) {
				auto [k, v] = stringExtra(f, e);
				if (k == "SMP Bones") pieces[1] = v;
				else if (k.rfind("SMP Bones ", 0) == 0) pieces[atoi(k.c_str() + 10)] = v;
			}
			if (pieces.empty()) continue;
			for (auto& [i, v] : pieces) list += v;
			std::vector<std::pair<std::string, std::string>> old;   // name, parent, in the old slot order
			for (size_t p = 0; p < list.size();) {
				size_t end = list.find(';', p);
				std::string entry = list.substr(p, end == std::string::npos ? std::string::npos : end - p);
				size_t bar = entry.find('|');
				if (!entry.empty()) old.push_back({ entry.substr(0, bar), bar == std::string::npos ? "" : entry.substr(bar + 1) });
				if (end == std::string::npos) break;
				p = end + 1;
			}
			std::map<std::string, std::string> parentOf(old.begin(), old.end());
			const std::string shapeName = strName(f, (int)si);

			bool dism = f.typeOf(g.skin) == "BSDismemberSkinInstance";
			SkinInstance skin;
			skin.read(f.blocks[g.skin], dism);
			SkinData sd;
			sd.read(f.blocks[skin.data]);
			std::vector<std::string> slots;
			for (int b : skin.bones) slots.push_back(strName(f, b));

			// The reference shape's skin, for the missing bones' skin-to-bone.
			int refShape = findBlockByName(ref, shapeName, false);
			SkinData refSd;
			std::vector<std::string> refSlots;
			if (refShape >= 0) {
				Geometry rg;
				rg.read(ref.blocks[refShape]);
				bool rdism = ref.typeOf(rg.skin) == "BSDismemberSkinInstance";
				SkinInstance rs;
				rs.read(ref.blocks[rg.skin], rdism);
				refSd.read(ref.blocks[rs.data]);
				for (int b : rs.bones) refSlots.push_back(strName(ref, b));
			}

			int restored = 0;
			for (auto& [name, parent] : old) {
				if (parent.empty()) continue;   // a body bone: not needed
				bool present = false;
				for (auto& s : slots) present |= s == name;
				if (present) continue;
				// Its node: the edited file's if still there, else the reference's, under the root.
				int node = findBlockByName(f, name, true);
				if (node < 0) {
					int refNode = findBlockByName(ref, name, true);
					if (refNode < 0) throw std::runtime_error(name + " is in neither file");
					AVObject n;
					{ Reader r(ref.blocks[refNode].data(), ref.blocks[refNode].size()); n.read(r, true); }
					n.name = f.addString(name);
					n.extraData.clear(); n.children.clear(); n.effects.clear(); n.properties.clear();
					n.controller = -1; n.collision = -1;
					Writer w;
					n.write(w);
					node = f.addBlock("NiNode", w.b);
					root.children.push_back(node);
					rootChanged = true;
				}
				SkinData::Bone bone;
				size_t k = 0;
				for (; k < refSlots.size() && refSlots[k] != name; k++) {}
				if (k == refSlots.size()) throw std::runtime_error(name + " is not in the reference's " + shapeName + " skin");
				bone.skinToBone = refSd.bones[k].skinToBone;
				skin.bones.push_back(node);
				sd.bones.push_back(bone);
				slots.push_back(name);
				restored++;
			}

			// "SMP Bones" again, in the skin's slot order.
			std::string fresh;
			for (auto& s : slots) fresh += s + "|" + (parentOf.count(s) ? parentOf[s] : "") + ";";
			std::vector<int32_t> kept;
			for (int e : g.av.extraData) if (!isSmpBonesKey(stringExtra(f, e).first)) kept.push_back(e);
			g.av.extraData = kept;   // the old pieces' blocks stay in the file, unreferenced
			addSmpBones(f, g.av, fresh);
			f.blocks[si] = g.write();
			f.blocks[g.skin] = skin.write();
			f.blocks[skin.data] = sd.write();
			printf("%s: %zu skin bones (%d restored), SMP Bones rewritten\n", shapeName.c_str(), slots.size(), restored);
			shapes++;
		}
		if (rootChanged) {
			Writer w;
			root.write(w);
			w.raw(rootTail.data(), rootTail.size());
			f.blocks[0] = w.b;
		}
		f.save(argv[3]);
		printf("wrote %s (%d shape(s))\n", argv[3], shapes);
	}
	catch (const std::exception& e) { printf("error: %s\n", e.what()); return 1; }
	return 0;
}
