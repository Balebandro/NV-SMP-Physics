// nifcheck: what smpify changed in a NIF, and whether every skin partition is consistent.
//   nifcheck original.nif ours.nif
// Lists the blocks whose bytes differ (type and the shape they belong to) and the blocks added, then,
// for every skinned shape of ours.nif, checks each partition against its NiSkinData: palette entries
// and bone indices in range, weights summing to 1, and each vertex's partition weights matching the
// skin data's. The game skins with the partitions, so a mismatch there renders as pinched geometry.
#include "nif.h"
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <map>
using namespace nif;

static std::string strName(const File& f, int block)
{
	Reader r(f.blocks[block].data(), f.blocks[block].size());
	uint32_t s = r.u32();
	return s < f.strings.size() ? f.strings[s] : "";
}

static bool isShape(const std::string& t) { return t == "NiTriShape" || t == "NiTriStrips"; }

int main(int argc, char** argv)
{
	if (argc == 2) {
		// One file: what its blocks are (for animation files, which interpolators they use).
		try {
			File a;
			a.load(argv[1]);
			std::map<std::string, int> types;
			for (size_t i = 0; i < a.blocks.size(); i++) types[a.typeOf(i)]++;
			printf("%zu blocks\n", a.blocks.size());
			for (auto& [t, n] : types) printf("  %5d  %s\n", n, t.c_str());
			return 0;
		} catch (const std::exception& e) {
			printf("error: %s\n", e.what());
			return 1;
		}
	}
	if (argc < 3) {
		printf("usage: nifcheck original.nif ours.nif   |   nifcheck any.nif (block types)\n");
		return 1;
	}
	try {
		File a, b;
		a.load(argv[1]);
		b.load(argv[2]);

		// Which shape each data/skin/partition block belongs to, by name.
		std::map<int, std::string> owner;
		for (size_t i = 0; i < b.blocks.size(); i++) {
			if (!isShape(b.typeOf(i))) continue;
			std::string n = strName(b, (int)i);
			Geometry g;
			g.read(b.blocks[i]);
			owner[(int)i] = n;
			if (g.data >= 0) owner[g.data] = n + " (data)";
			if (g.skin >= 0) {
				owner[g.skin] = n + " (skin)";
				SkinInstance si;
				si.read(b.blocks[g.skin], b.typeOf(g.skin) == "BSDismemberSkinInstance");
				owner[si.data] = n + " (skin data)";
				owner[si.partition] = n + " (partitions)";
			}
		}

		printf("blocks: original %zu, ours %zu\n", a.blocks.size(), b.blocks.size());
		for (size_t i = 0; i < a.blocks.size() && i < b.blocks.size(); i++) {
			if (a.blocks[i] == b.blocks[i]) continue;
			size_t diff = 0, first = SIZE_MAX;
			for (size_t k = 0; k < std::min(a.blocks[i].size(), b.blocks[i].size()); k++)
				if (a.blocks[i][k] != b.blocks[i][k]) { ++diff; if (first == SIZE_MAX) first = k; }
			printf("  changed %4zu %-22s %-30s %zu -> %zu bytes, %zu differ from byte %zd\n", i, b.typeOf(i).c_str(),
				owner.count((int)i) ? owner[(int)i].c_str() : (isShape(b.typeOf(i)) || b.typeOf(i).find("Node") != std::string::npos ? strName(b, (int)i).c_str() : ""),
				a.blocks[i].size(), b.blocks[i].size(), diff, first == SIZE_MAX ? -1 : (ptrdiff_t)first);
		}
		for (size_t i = a.blocks.size(); i < b.blocks.size(); i++)
			printf("  added   %4zu %s\n", i, b.typeOf(i).c_str());

		int problems = 0;
		for (size_t i = 0; i < b.blocks.size(); i++) {
			if (!isShape(b.typeOf(i))) continue;
			Geometry g;
			g.read(b.blocks[i]);
			if (g.skin < 0) continue;
			SkinInstance si;
			si.read(b.blocks[g.skin], b.typeOf(g.skin) == "BSDismemberSkinInstance");
			SkinData sd;
			sd.read(b.blocks[si.data]);
			SkinPartition sp;
			sp.read(b.blocks[si.partition]);
			// Skin data weights per vertex.
			std::map<int, std::map<int, float>> w;
			for (size_t bone = 0; bone < sd.bones.size(); bone++)
				for (auto& vw : sd.bones[bone].weights) w[vw.index][(int)bone] += vw.weight;
			int bad = 0, mismatched = 0, verts = 0;
			for (auto& p : sp.parts) {
				for (auto x : p.bones) if (x >= si.bones.size()) ++bad;
				for (size_t v = 0; v < p.vertexMap.size(); v++) {
					++verts;
					float sum = 0;
					std::map<int, float> pw;
					for (int k = 0; k < p.numWeightsPerVertex; k++) {
						float wt = p.weights[v * p.numWeightsPerVertex + k];
						uint8_t bi = p.boneIndices[v * p.numWeightsPerVertex + k];
						sum += wt;
						if (wt <= 0) continue;
						if (bi >= p.bones.size()) { ++bad; continue; }
						pw[p.bones[bi]] += wt;
					}
					if (std::fabs(sum - 1.0f) > 0.02f) ++bad;
					auto& sw = w[p.vertexMap[v]];
					float err = 0;
					for (auto& [bone, x] : pw) err += std::fabs(x - (sw.count(bone) ? sw[bone] : 0.0f));
					for (auto& [bone, x] : sw) if (!pw.count(bone)) err += x;
					if (err > 0.05f) ++mismatched;
				}
			}
			printf("shape %-24s %zu bones, %zu partitions, %d partition vertices: %d out of range or unnormalised, %d disagree with the skin data\n",
				strName(b, (int)i).c_str(), si.bones.size(), sp.parts.size(), verts, bad, mismatched);
			if (getenv("NIFCHECK_BONES")) {
				printf("    bones:");
				for (auto bone : si.bones) printf(" [%s]", strName(b, bone).c_str());
				printf("\n");
			}
			problems += bad + mismatched;
		}
		printf(problems ? "PROBLEMS\n" : "partitions OK\n");
		return problems ? 2 : 0;
	} catch (const std::exception& e) {
		printf("error: %s\n", e.what());
		return 1;
	}
}
