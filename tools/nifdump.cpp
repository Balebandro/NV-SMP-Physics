// nifdump: the shapes of a NIF, for planning SMP content: name, vertices, bounds, skin bones (with how many vertices
// each weights) and the separate pieces (connected parts) the mesh is made of, with their bounds.
//
//   nifdump file.nif
#include "nif.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <numeric>
using namespace nif;

static std::string nameOf(const File& f, int block)
{
	if (block < 0 || block >= (int)f.blocks.size()) return "(none)";
	Reader r(f.blocks[block].data(), f.blocks[block].size());
	uint32_t s = r.u32();
	return s < f.strings.size() ? f.strings[s] : "";
}

static int findRoot(std::vector<int>& p, int x) { while (p[x] != x) x = p[x] = p[p[x]]; return x; }

int main(int argc, char** argv)
{
	if (argc < 2) { printf("usage: nifdump file.nif\n"); return 1; }
	try {
		File f;
		f.load(argv[1]);
		for (size_t i = 0; i < f.blocks.size(); i++) {
			const std::string& t = f.typeOf(i);
			if (t != "NiTriShape" && t != "NiTriStrips") continue;
			Geometry g; g.read(f.blocks[i]);
			GeometryData d;
			if (g.data >= 0) d.read(f.blocks[g.data], t == "NiTriStrips");
			Vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
			for (auto& v : d.vertices) {
				lo.x = std::min(lo.x, v.x); lo.y = std::min(lo.y, v.y); lo.z = std::min(lo.z, v.z);
				hi.x = std::max(hi.x, v.x); hi.y = std::max(hi.y, v.y); hi.z = std::max(hi.z, v.z);
			}
			printf("\n[%zu] %s \"%s\": %zu vertices, %zu triangles, bounds (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)\n", i, t.c_str(),
				nameOf(f, (int)i).c_str(), d.vertices.size(), d.triangles.size(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);

			if (g.skin >= 0) {
				const std::string& st = f.typeOf(g.skin);
				SkinInstance s; s.read(f.blocks[g.skin], st == "BSDismemberSkinInstance");
				SkinData sd;
				if (s.data >= 0) sd.read(f.blocks[s.data]);
				printf("  skin %s, %zu bones:", st.c_str(), s.bones.size());
				for (size_t b = 0; b < s.bones.size(); b++)
					printf(" %s(%zu)", nameOf(f, s.bones[b]).c_str(), b < sd.bones.size() ? sd.bones[b].weights.size() : 0);
				printf("\n");
			}

			// Connected pieces.
			std::vector<int> p(d.vertices.size());
			std::iota(p.begin(), p.end(), 0);
			for (auto& tr : d.triangles) {
				int a = findRoot(p, tr.v[0]), b = findRoot(p, tr.v[1]), c = findRoot(p, tr.v[2]);
				p[b] = a; p[findRoot(p, c)] = a;
			}
			// Vertices at the same position count as one (UV seams split them).
			std::map<std::tuple<int, int, int>, int> at;
			for (size_t v = 0; v < d.vertices.size(); v++) {
				auto key = std::make_tuple((int)std::lround(d.vertices[v].x * 100), (int)std::lround(d.vertices[v].y * 100), (int)std::lround(d.vertices[v].z * 100));
				auto it = at.find(key);
				if (it == at.end()) at[key] = (int)v;
				else { int a = findRoot(p, it->second), b = findRoot(p, (int)v); if (a != b) p[b] = a; }
			}
			std::map<int, std::vector<int>> pieces;
			for (size_t v = 0; v < d.vertices.size(); v++) pieces[findRoot(p, (int)v)].push_back((int)v);
			printf("  %zu piece(s)\n", pieces.size());
			int shown = 0;
			for (auto& [root, verts] : pieces) {
				if (shown++ >= 40) { printf("  ...\n"); break; }
				Vec3 a{ 1e9f, 1e9f, 1e9f }, b{ -1e9f, -1e9f, -1e9f };
				for (int v : verts) {
					const Vec3& q = d.vertices[v];
					a.x = std::min(a.x, q.x); a.y = std::min(a.y, q.y); a.z = std::min(a.z, q.z);
					b.x = std::max(b.x, q.x); b.y = std::max(b.y, q.y); b.z = std::max(b.z, q.z);
				}
				printf("    %5zu verts  (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)\n", verts.size(), a.x, a.y, a.z, b.x, b.y, b.z);
			}
		}
	}
	catch (const std::exception& e) { printf("error: %s\n", e.what()); return 1; }
	return 0;
}
