// smpdress: makes several shapes of one outfit SMP (HDT-SMP / FSMP) physics items for NV SMP Physics, in one NIF and
// one physics XML. smpify's skirt grid, generalised, plus rope chains with rigid pieces hanging from them.
//
//   smpdress in.nif out.nif "Data\relative\physics.xml" out.xml job... [--collide "Shape"]...
//
//   --cloth "Shape"   a cape, poncho or skirt: a grid of cloth bones, columns around the body, rows from --top down to
//                     the hem. The top row are anchors on --anchor; the vertices above it keep their weights.
//       --anchor "Bone"     the bone the top row hangs from (default Bip01 Pelvis)
//       --top z             skin-space height of the top row (default: 80% up the shape)
//       --angles lo hi      the grid spans lo..hi degrees around the body, 0 = back, 90 = the body's right side
//                           (default: the shape's own extent below --top)
//       --axis x y          the body axis the angles are measured around (default 0 0)
//       --grid cols rows    (default 9 x 5)
//       --radial            rows by distance from the neck instead of by height: a cape draped over the shoulders hangs
//                           from the collar along its own surface (--top is then unused)
//       --collar d          with --radial, the top row's distance from the neck (default 10); closer vertices keep
//                           their weights
//       --neck x y z        with --radial, the neck point (default: the centre of the shape's top 4 units)
//       --detach "B=T"      hand bone B's weights to T (both skin bones of the shape) first, e.g. a poncho's arm
//                           weights onto the clavicle, so the arm moves under the cloth instead of dragging it (repeatable)
//       --keep "B" w0 w1     cloth pinned under armor stays static: vertices weighted to B in the source (after
//                           --detach) keep their own weights from weight w1 up, blending into the cloth from w0 (repeatable)
//   --chains "Shape"  ropes, cords, straps: every piece of the mesh that reaches the top (within 2 units of its
//                     highest point) and is longer than 4 units is a strand, a chain of --links bones from its top
//                     (an anchor on --anchor) to its bottom. Small pieces at the top (knots) keep their weights. Every
//                     other piece (a bottle, a charm, a tag) is rigid: all of it on the last link of the strand whose
//                     end is nearest, so it swings with the rope but never bends.
//       --anchor "Bone"     (default Bip01 Pelvis)
//       --links n           simulated links per strand (default 4)
//       --keep-top a b      where the strands are tied on stays static: their top a units keep their own weights,
//                           blending into the links down to b units
//   --painted "Shape" file.smppaint   chains painted on the shape in the NV SMP Maker: per chain its stroke (links laid
//                     along it from where it is tied on), the painted vertices and their strength (a soft edge blends
//                     into the shape's own weights), the bone it hangs from, links, stiffness and weight (each chain
//                     gets its own templates from those two), and any points moved off the stroke
//   --collide "Shape" a shape of the same NIF everything above collides with (repeatable)
//   --proxy cell      the colliders collide through low-poly stand-ins hidden in the NIF (vertices merged in cells of
//                     this many units, about 2-4): far cheaper for the physics to skin and test than the render meshes.
//                     A collider named "SMP Proxy:..." is rebuilt in place; a shape's existing proxy is replaced.
//   --proxy-tris n    coarsen each proxy's cell (from --proxy's, default 1) until it has at most n triangles
//   --collide-skip "Bone"   leave out of every collider the triangles weighted only to this bone (and others skipped):
//                     parts the cloth never reaches (lower legs, forearms) cost time to skin and test (repeatable)
//   --cloth-margin m, --body-margin m, --penetration d   collision margins (defaults 1, 0.5, 3); lower them for cloth
//                     modelled tight on the body, which otherwise starts inside the margins and is shoved out
//   --fixed-rows n    (per --cloth) the top rows that do not collide (default 1, the anchors)
//   --bend f, --stiffness f   scale the cloth links' swing limits and their springs back to the rest shape (default 1);
//                     a cape modelled for a T pose needs room and soft springs to drape down over a lowered arm
//   --no-weft         no links between neighbouring cloth columns: they hold the cloth's modelled width, so a flap
//                     modelled spread over a raised arm stands out like a shelf once the arm is lowered
//   --cloth-friction f   the cloth bones' friction (default 0.9); low lets a flap slide off the shoulder and drape
//   --cloth-mass m, --cloth-restitution r   the cloth bones' mass and bounce (defaults 0.02, 0.2)
//   --cloth-damping l a   the cloth bones' linear and angular damping (default 0.6 0.8); higher is heavier cloth that sways less
//
// Per shape, as smpify: new skin bones, reweighting, NiSkinData and NiSkinPartition rebuilt (16 bones per partition),
// "SMP Bones" (every skin slot's bone and parent; New Vegas drops an item's own bone nodes on attach and the plugin
// creates them from this list) and "HDT Skinned Mesh Physics Object" on the shape and the root.
#include "nif.h"
#include <algorithm>
#include <bitset>
#include <fstream>
#include <cstdio>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
using namespace nif;

static const float PI = 3.14159265f;
static const int MaxBonesPerPartition = 16;

struct Influence { int bone; float w; };
typedef std::vector<Influence> Weights;

static void normalizeTop4(Weights& w)
{
	std::map<int, float> acc;
	for (auto& i : w) acc[i.bone] += i.w;
	w.clear();
	for (auto& kv : acc) if (kv.second > 1e-4f) w.push_back({ kv.first, kv.second });
	std::sort(w.begin(), w.end(), [](const Influence& a, const Influence& b) { return a.w > b.w; });
	if (w.size() > 4) w.resize(4);
	float t = 0;
	for (auto& i : w) t += i.w;
	for (auto& i : w) i.w /= t;
}

static std::string strName(const File& f, int block)
{
	Reader r(f.blocks[block].data(), f.blocks[block].size());
	uint32_t s = r.u32();
	return s < f.strings.size() ? f.strings[s] : "";
}

static int findBlockByName(const File& f, const std::string& name)
{
	for (size_t i = 0; i < f.blocks.size(); i++) {
		const std::string& t = f.typeOf(i);
		if ((isNodeType(t) || t == "NiTriShape" || t == "NiTriStrips") && strName(f, (int)i) == name) return (int)i;
	}
	return -1;
}

static std::string xmlEscape(const std::string& s)
{
	std::string o;
	for (char c : s) {
		if (c == '&') o += "&amp;";
		else if (c == '<') o += "&lt;";
		else if (c == '>') o += "&gt;";
		else if (c == '"') o += "&quot;";
		else o += c;
	}
	return o;
}

static void addStringExtra(File& f, AVObject& av, const std::string& name, const std::string& value)
{
	Writer w;
	w.u32(f.addString(name));
	w.u32(f.addString(value));
	av.extraData.push_back(f.addBlock("NiStringExtraData", w.b));
}

// "SMP Bones" in pieces under 2000 characters, split between entries: "SMP Bones", "SMP Bones 2", ... (Outfit Studio
// cannot read a NIF string of 2048 characters or more; the plugin joins the pieces).
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

static int findRoot(std::vector<int>& p, int x) { while (p[x] != x) x = p[x] = p[p[x]]; return x; }

// A chain painted in the NV SMP Maker (--painted): its path (vertex indices in stroke order, from where it is tied on)
// and the painted vertices with their paint strength (0..1), what it hangs from and how it feels.
struct PaintChain {
	std::string anchor = "Bip01 Pelvis";
	int links = 4;
	float stiffness = 0.4f, weight = 0.4f;   // 0..1 each
	std::vector<int> path;
	std::vector<std::pair<int, float>> paint;
	std::map<int, Vec3> points;               // points moved in the Maker, by index (0 the tie-on): where that bone goes
	int global = 0;                           // its number across the outfit (template names)
};

struct Job {
	enum Kind { Cloth, Chains, Painted, Pieces } kind;
	std::string shape;
	std::string anchor = "Bip01 Pelvis";
	float top = -1e9f;
	bool angles = false;
	float angleLo = 0, angleHi = 0;
	float axisX = 0, axisY = 0;
	int cols = 9, rows = 5, links = 4;
	float keepTopA = 0, keepTopB = 0;   // --keep-top a b: the strands' top a units keep their own weights, blending to b
	std::vector<std::pair<std::string, std::string>> detach;   // --detach "Bone=Target"
	struct Keep { std::string bone; float w0, w1; };
	std::vector<Keep> keep;    // --keep "Bone" w0 w1
	int fixedRows = 1;         // --fixed-rows n: the cloth's top rows that do not collide (they sit on the body)
	bool radial = false;       // --radial: rows by distance from the neck
	float collar = 10.0f;      // --collar d: the top row's distance from it (closer vertices keep their weights)
	bool neckSet = false;      // --neck x y z (default: the centre of the shape's top 4 units)
	Vec3 neck;
	std::vector<PaintChain> painted;   // --painted "Shape" file.smppaint: the shape's chains from the file
	std::string groups;        // --groups "0,1|2|3" (--pieces): the pieces of each rigid body, top first
	Vec3 up{ 0, 0, 1 };        // --up x y z (--pieces): which way is up in the shape's space
};

// Reads the chains of one shape from a .smppaint file (written by the NV SMP Maker):
//   chain "Shape" / anchor Bone / links n / stiffness s / weight w / path v v v... / paint v:s v:s... / point k x y z
//   (a moved point, any number, in the skin's space as the vertices are read here) / end
static std::vector<PaintChain> readPaintFile(const std::string& path, const std::string& shape)
{
	std::ifstream in(path);
	if (!in) throw std::runtime_error("cannot read the paint file " + path);
	std::vector<PaintChain> out;
	PaintChain cur;
	bool inChain = false, mine = false;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		size_t sp = line.find(' ');
		std::string key = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
		if (key == "chain") { cur = PaintChain(); inChain = true; mine = rest == shape; }
		else if (!inChain) continue;
		else if (key == "end") { if (mine) out.push_back(cur); inChain = false; }
		else if (key == "anchor") cur.anchor = rest;
		else if (key == "links") cur.links = std::max(1, atoi(rest.c_str()));
		else if (key == "stiffness") cur.stiffness = std::clamp((float)atof(rest.c_str()), 0.0f, 1.0f);
		else if (key == "weight") cur.weight = std::clamp((float)atof(rest.c_str()), 0.0f, 1.0f);
		else if (key == "path") { std::istringstream s(rest); int v; while (s >> v) cur.path.push_back(v); }
		else if (key == "point") { std::istringstream s(rest); int k; Vec3 p; if (s >> k >> p.x >> p.y >> p.z) cur.points[k] = p; }
		else if (key == "paint") {
			std::istringstream s(rest);
			std::string t;
			while (s >> t) { size_t c = t.find(':'); if (c != std::string::npos) cur.paint.push_back({ atoi(t.c_str()), (float)atof(t.c_str() + c + 1) }); }
		}
	}
	if (out.empty()) throw std::runtime_error("the paint file has no chains on " + shape);
	return out;
}

// A bone the job adds: a node at its bind position (skin space), under its parent.
struct NewBone {
	std::string name, parent, role; Vec3 bind; int slot = -1;
	bool hasBox = false;   // a collision box around rigid pieces on this bone (skin space)
	Vec3 boxMin{ 1e9f, 1e9f, 1e9f }, boxMax{ -1e9f, -1e9f, -1e9f };
	std::vector<Vec3> piecePts;   // the rigid pieces' vertices (skin space), for their collision box
};

// What a job leaves for the physics XML.
struct JobResult {
	std::string shape;
	std::vector<NewBone> bones;
	std::vector<std::string> kinematic;               // the shape's own body bones
	std::vector<std::vector<std::string>> warp;       // chains of bone names, top first (anchor first)
	std::vector<std::pair<std::string, std::string>> weft;
	std::vector<std::string> topBones;                // bones whose vertices do not collide (anchors and the first link)
	std::vector<int> warpPaint;                       // per warp chain: its PaintChain::global, or -1 (cloth, rope)
};

// The connected pieces of a mesh (vertices at the same position joined: UV seams split them).
static std::vector<std::vector<int>> piecesOf(const GeometryData& d, const std::vector<Vec3>& pos)
{
	std::vector<int> p(pos.size());
	std::iota(p.begin(), p.end(), 0);
	for (auto& t : d.triangles) {
		int a = findRoot(p, t.v[0]);
		p[findRoot(p, t.v[1])] = a;
		p[findRoot(p, t.v[2])] = a;
	}
	std::map<std::tuple<int, int, int>, int> at;
	for (size_t v = 0; v < pos.size(); v++) {
		auto key = std::make_tuple((int)std::lround(pos[v].x * 100), (int)std::lround(pos[v].y * 100), (int)std::lround(pos[v].z * 100));
		auto it = at.find(key);
		if (it == at.end()) at[key] = (int)v;
		else { int a = findRoot(p, it->second), b = findRoot(p, (int)v); if (a != b) p[b] = a; }
	}
	std::map<int, std::vector<int>> m;
	for (size_t v = 0; v < pos.size(); v++) m[findRoot(p, (int)v)].push_back((int)v);
	std::vector<std::vector<int>> out;
	for (auto& kv : m) out.push_back(kv.second);
	return out;
}

// NiSkinPartition: greedy, each triangle's bones in one partition, growing by the triangle that adds the fewest new bones.
static SkinPartition buildPartition(const std::vector<Weights>& neww, const std::vector<Triangle>& tris, size_t numBones)
{
	typedef std::vector<uint64_t> Mask;
	size_t words = (numBones + 63) / 64;
	auto bits = [](const Mask& m) { int k = 0; for (auto w : m) k += (int)std::bitset<64>(w).count(); return k; };
	std::vector<Mask> triMask(tris.size(), Mask(words, 0));
	for (size_t t = 0; t < tris.size(); t++) for (int k = 0; k < 3; k++) for (auto& i : neww[tris[t].v[k]]) triMask[t][i.bone / 64] |= 1ull << (i.bone % 64);
	std::vector<bool> used(tris.size(), false);
	size_t left = tris.size();
	SkinPartition np;
	while (left) {
		Mask cur(words, 0);
		std::vector<Triangle> pt;
		for (;;) {
			int best = -1, bestNew = 1 << 30;
			for (size_t t = 0; t < tris.size(); t++) {
				if (used[t]) continue;
				Mask added(words);
				for (size_t w = 0; w < words; w++) added[w] = triMask[t][w] & ~cur[w];
				int nNew = bits(added);
				if (nNew >= bestNew || bits(cur) + nNew > MaxBonesPerPartition) continue;
				best = (int)t; bestNew = nNew;
				if (nNew == 0) break;
			}
			if (best < 0) break;
			for (size_t w = 0; w < words; w++) cur[w] |= triMask[best][w];
			for (size_t t = 0; t < tris.size(); t++) {
				if (used[t]) continue;
				bool inside = true;
				for (size_t w = 0; w < words; w++) if (triMask[t][w] & ~cur[w]) inside = false;
				if (inside) { used[t] = true; left--; pt.push_back(tris[t]); }
			}
		}
		if (pt.empty()) throw std::runtime_error("a triangle needs more bones than a partition holds");
		std::set<int> bones;
		for (size_t b = 0; b < numBones; b++) if (cur[b / 64] & (1ull << (b % 64))) bones.insert((int)b);
		SkinPartition::Part p;
		p.bones.assign(bones.begin(), bones.end());
		std::map<int, int> boneLocal;
		for (size_t i = 0; i < p.bones.size(); i++) boneLocal[p.bones[i]] = (int)i;
		std::map<int, int> vertLocal;
		for (auto& t : pt) {
			Triangle lt;
			for (int k = 0; k < 3; k++) {
				auto it = vertLocal.find(t.v[k]);
				if (it == vertLocal.end()) { it = vertLocal.emplace(t.v[k], (int)p.vertexMap.size()).first; p.vertexMap.push_back(t.v[k]); }
				lt.v[k] = (uint16_t)it->second;
			}
			p.triangles.push_back(lt);
		}
		for (auto v : p.vertexMap) {
			const Weights& w = neww[v];
			for (int k = 0; k < 4; k++) {
				p.weights.push_back(k < (int)w.size() ? w[k].w : 0.0f);
				p.boneIndices.push_back(k < (int)w.size() ? (uint8_t)boneLocal[w[k].bone] : 0);
			}
		}
		np.parts.push_back(p);
	}
	return np;
}

// --proxy: a collider's low-poly stand-in. The physics re-skins and tests every vertex and triangle of a collider each
// step, so a full render mesh (thousands of triangles) is costly; the stand-in clusters its vertices into cells of
// `cell` units, keeping surfaces that face apart (a thin shell's two sides) apart, so the side the cloth is pushed to
// stays right. Each cluster takes its members' mean place, normal and weights. It goes in the NIF as a shape skinned to
// the same bones; the physics file names it instead of the render mesh. It is not hidden in the NIF: in game a culled
// stand-in loses its collisions, and the shaders ignore an alpha test. The plugin hides it once built (generateMeshBody).
static int buildProxy(File& f, int shapeBlock, float cell, const std::string& proxyName, int& tris0, int& tris1)
{
	Geometry g;
	g.read(f.blocks[shapeBlock]);
	bool strips = f.typeOf(g.data) == "NiTriStripsData";
	GeometryData gd;
	gd.read(f.blocks[g.data], strips);
	bool dism = f.typeOf(g.skin) == "BSDismemberSkinInstance";
	SkinInstance si;
	si.read(f.blocks[g.skin], dism);
	SkinData sd;
	sd.read(f.blocks[si.data]);
	const size_t nv = gd.vertices.size();
	std::vector<Weights> w(nv);
	for (size_t b = 0; b < sd.bones.size(); b++)
		for (auto& vw : sd.bones[b].weights) w[vw.index].push_back({ (int)b, vw.weight });
	// Normals: the mesh's, else from its faces.
	std::vector<Vec3> nrm = gd.normals;
	if (nrm.size() != nv) {
		nrm.assign(nv, Vec3{});
		for (auto& t : gd.triangles) {
			Vec3 a = gd.vertices[t.v[0]], b = gd.vertices[t.v[1]], c = gd.vertices[t.v[2]];
			Vec3 e1 = b - a, e2 = c - a;
			Vec3 n{ e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x };
			for (int k = 0; k < 3; k++) nrm[t.v[k]] = nrm[t.v[k]] + n;
		}
	}
	struct Cluster { Vec3 p{}, n{}; float u = 0, v = 0; std::map<int, float> w; int count = 0; };
	std::map<std::tuple<int, int, int, int>, int> index;
	std::vector<Cluster> clusters;
	std::vector<int> clusterOf(nv);
	for (size_t v = 0; v < nv; v++) {
		const Vec3& p = gd.vertices[v];
		const Vec3& n = nrm[v];
		float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
		int dir = ax >= ay && ax >= az ? (n.x >= 0 ? 0 : 1) : ay >= az ? (n.y >= 0 ? 2 : 3) : (n.z >= 0 ? 4 : 5);
		auto key = std::make_tuple((int)std::floor(p.x / cell), (int)std::floor(p.y / cell), (int)std::floor(p.z / cell), dir);
		auto it = index.find(key);
		if (it == index.end()) { it = index.emplace(key, (int)clusters.size()).first; clusters.emplace_back(); }
		Cluster& c = clusters[it->second];
		c.p = c.p + p;
		c.n = c.n + n;
		if (gd.uvs.size() == nv * 2) { c.u += gd.uvs[v * 2]; c.v += gd.uvs[v * 2 + 1]; }
		for (auto& i : w[v]) c.w[i.bone] += i.w;
		c.count++;
		clusterOf[v] = it->second;
	}
	std::vector<Triangle> tris;
	std::set<std::tuple<int, int, int>> seen;
	for (auto& t : gd.triangles) {
		int a = clusterOf[t.v[0]], b = clusterOf[t.v[1]], c = clusterOf[t.v[2]];
		if (a == b || b == c || a == c) continue;
		int s[3] = { a, b, c };
		std::sort(s, s + 3);
		if (!seen.insert({ s[0], s[1], s[2] }).second) continue;
		tris.push_back({ { (uint16_t)a, (uint16_t)b, (uint16_t)c } });
	}
	if (clusters.size() > 65535) throw std::runtime_error("--proxy cell too small for " + proxyName);
	std::vector<Vec3> verts(clusters.size()), norms(clusters.size());
	std::vector<Weights> pw(clusters.size());
	for (size_t i = 0; i < clusters.size(); i++) {
		auto& c = clusters[i];
		verts[i] = c.p * (1.0f / c.count);
		float l = length(c.n);
		norms[i] = l > 1e-6f ? c.n * (1.0f / l) : Vec3{ 0, 0, 1 };
		for (auto& [b, x] : c.w) pw[i].push_back({ b, x });
		normalizeTop4(pw[i]);
	}
	tris0 = (int)gd.triangles.size();
	tris1 = (int)tris.size();

	// NiTriShapeData (20.2.0.7, BS 34): positions, normals, tangents and a UV set, no colours. The stand-in keeps the
	// render mesh's (normal-mapped) shader, which the game sets up from the tangents and UVs even for a culled shape:
	// without them it crashes on equip. Never drawn, so a tangent frame around the normal will do.
	Vec3 lo{ 1e9f, 1e9f, 1e9f }, hi{ -1e9f, -1e9f, -1e9f };
	for (auto& p : verts) { lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) }; hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) }; }
	Vec3 centre = (lo + hi) * 0.5f;
	float radius = 0;
	for (auto& p : verts) radius = std::max(radius, length(p - centre));
	Writer d;
	d.i32(0);                                // group id
	d.u16((uint16_t)verts.size());
	d.u8(0x01 | 0x20); d.u8(0);              // keep flags (the plugin reads positions and weights after upload), compress flags
	d.u8(1); for (auto& p : verts) d.v3(p);  // has vertices
	d.u16(0x1001);                           // BS vector flags: a UV set, tangents
	d.u8(1); for (auto& n : norms) d.v3(n);  // has normals
	std::vector<Vec3> tangents(norms.size()), bitangents(norms.size());
	for (size_t i = 0; i < norms.size(); i++) {
		Vec3 n = norms[i];
		tangents[i] = normalize(cross(std::fabs(n.z) < 0.9f ? Vec3{ 0, 0, 1 } : Vec3{ 1, 0, 0 }, n));
		bitangents[i] = cross(n, tangents[i]);
	}
	for (auto& t : tangents) d.v3(t);
	for (auto& b : bitangents) d.v3(b);
	d.v3(centre); d.f32(radius);
	d.u8(0);                                 // has vertex colours
	for (auto& c : clusters) { d.f32(c.u / c.count); d.f32(c.v / c.count); }
	d.u16(0);                                // consistency
	d.i32(-1);                               // additional data
	d.u16((uint16_t)tris.size()); d.u32((uint32_t)tris.size() * 3);
	d.u8(1); for (auto& t : tris) { d.u16(t.v[0]); d.u16(t.v[1]); d.u16(t.v[2]); }
	d.u16(0);                                // match groups
	int dataBlk = f.addBlock("NiTriShapeData", d.b);

	SkinData psd;
	psd.skinTransform = sd.skinTransform;
	psd.bones.resize(sd.bones.size());
	for (size_t b = 0; b < sd.bones.size(); b++) psd.bones[b].skinToBone = sd.bones[b].skinToBone;
	for (size_t v = 0; v < pw.size(); v++) for (auto& i : pw[v]) psd.bones[i.bone].weights.push_back({ (uint16_t)v, i.w });
	for (auto& b : psd.bones) {
		if (b.weights.empty()) continue;
		Vec3 c{};
		for (auto& vw : b.weights) c = c + b.skinToBone.apply(sd.skinTransform.apply(verts[vw.index]));
		c = c * (1.0f / b.weights.size());
		float rad = 0;
		for (auto& vw : b.weights) rad = std::max(rad, length(b.skinToBone.apply(sd.skinTransform.apply(verts[vw.index])) - c));
		b.center = c; b.radius = rad;
	}
	int sdBlk = f.addBlock("NiSkinData", psd.write());
	int spBlk = f.addBlock("NiSkinPartition", buildPartition(pw, tris, sd.bones.size()).write());
	SkinInstance psi;
	psi.dismember = false;
	psi.data = sdBlk; psi.partition = spBlk; psi.skeletonRoot = si.skeletonRoot;
	psi.bones = si.bones;
	int siBlk = f.addBlock("NiSkinInstance", psi.write());

	Geometry pg = g;
	pg.av.name = f.addString(proxyName);
	pg.av.extraData.clear();
	pg.av.flags &= ~1u;   // never app-culled: a proxy culled before the game binds its skin never collides (2026-10-19)
	{
		// NiAlphaProperty: alpha test on (bit 9), test NEVER (7 << 10), no blending; replaces the shape's own. The
		// game's shaders ignore it, but it hides the stand-in in NIF viewers. The stand-in's other properties are its
		// own copies: sharing the render mesh's shader property, the game carried the never-pass test over to the
		// render mesh and the whole armor went invisible while the stand-in was drawn (2026-10-19).
		Writer a;
		a.u32(0xFFFFFFFF); a.u32(0); a.i32(-1);   // no name, no extra data, no controller
		a.u16(0x0200 | (7 << 10)); a.u8(255);
		int alpha = f.addBlock("NiAlphaProperty", a.b);
		std::vector<int32_t> props;
		for (auto p : pg.av.properties)
			if (p >= 0 && f.typeOf(p) != "NiAlphaProperty") {
				std::string type = f.typeOf(p);
				Bytes copy = f.blocks[p];
				props.push_back(f.addBlock(type, copy));
			}
		props.push_back(alpha);
		pg.av.properties = props;
	}
	pg.data = dataBlk;
	pg.skin = siBlk;
	return f.addBlock(f.typeOf(shapeBlock) == "NiTriStrips" ? "NiTriShape" : f.typeOf(shapeBlock), pg.write());
}

// --proxy-tris: the finest cell (from `cell` up) whose stand-in has at most `maxTris` triangles. buildProxy only
// appends blocks, so each try's blocks are dropped again before the next.
static int buildProxyUnder(File& f, int shapeBlock, float cell, int maxTris, const std::string& name, int& tris0, int& tris1, float& used)
{
	const size_t firstNew = f.blocks.size();
	auto attempt = [&](float c) {
		f.blocks.resize(firstNew);
		f.blockTypeIndex.resize(firstNew);
		return buildProxy(f, shapeBlock, c, name, tris0, tris1);
	};
	used = cell;
	int blk = attempt(cell);
	if (maxTris <= 0 || tris1 <= maxTris) return blk;
	float lo = cell, hi = cell;
	do { lo = hi; hi *= 1.5f; blk = attempt(hi); } while (tris1 > maxTris && hi < 1000.0f);
	for (int k = 0; k < 10; k++) {   // lo is too fine, hi fits: the finest that fits, near enough
		float mid = 0.5f * (lo + hi);
		attempt(mid);
		if (tris1 <= maxTris) hi = mid; else lo = mid;
	}
	used = hi;
	return attempt(hi);
}

// A new stand-in over an existing one ("SMP Proxy:X" rebuilt, or the old proxy of a shape): the new blocks take the
// old ones' slots (shape, geometry data, skin instance, skin data, partition, properties of the same type), so the
// old dense geometry does not stay in the file unreferenced and the parent keeps pointing at the same block. New
// blocks with no old slot move down to close the gaps. Everything buildProxy added is at or after `firstNew`, and only
// the new shape and skin instance refer to those blocks.
static void replaceProxy(File& f, int oldShape, int newShape, int firstNew)
{
	// How many shapes use each old block: a slot another shape shares is left alone.
	std::map<int, int> users;
	for (int i = 0; i < firstNew; i++) {
		const std::string& t = f.typeOf(i);
		if (t != "NiTriShape" && t != "NiTriStrips") continue;
		Geometry g;
		g.read(f.blocks[i]);
		users[g.data]++;
		for (auto p : g.av.properties) users[p]++;
		if (g.skin < 0) continue;
		users[g.skin]++;
		SkinInstance si;
		si.read(f.blocks[g.skin], f.typeOf(g.skin) == "BSDismemberSkinInstance");
		users[si.data]++;
		users[si.partition]++;
	}
	Geometry og, ng;
	og.read(f.blocks[oldShape]);
	ng.read(f.blocks[newShape]);
	SkinInstance oi, ni;
	if (og.skin >= 0) oi.read(f.blocks[og.skin], f.typeOf(og.skin) == "BSDismemberSkinInstance");
	ni.read(f.blocks[ng.skin], false);
	const int newSkin = ng.skin;

	std::map<int, int> slot;   // new block -> the old slot it takes
	auto take = [&](int nw, int old) {
		if (nw < firstNew || old < 0 || old >= firstNew || slot.count(nw) || users[old] != 1) return false;
		for (auto& [n, o] : slot) if (o == old) return false;
		slot[nw] = old;
		return true;
	};
	slot[newShape] = oldShape;
	take(ng.data, og.data);
	if (og.skin >= 0) { take(newSkin, og.skin); take(ni.data, oi.data); take(ni.partition, oi.partition); }
	for (auto np : ng.av.properties)
		for (auto op : og.av.properties)
			if (op >= 0 && f.typeOf(op) == f.typeOf(np) && take(np, op)) break;

	std::map<int, int> where;   // every new block's final index
	int next = firstNew;
	for (int n = firstNew; n < (int)f.blocks.size(); n++) where[n] = slot.count(n) ? slot[n] : next++;
	auto remap = [&](int32_t r) { return r >= firstNew ? where[r] : r; };
	ng.data = remap(ng.data);
	ng.skin = remap(ng.skin);
	for (auto& p : ng.av.properties) p = remap(p);
	f.blocks[newShape] = ng.write();
	ni.data = remap(ni.data);
	ni.partition = remap(ni.partition);
	f.blocks[newSkin] = ni.write();

	std::vector<Bytes> blocks(f.blocks.begin() + firstNew, f.blocks.end());
	std::vector<uint16_t> types(f.blockTypeIndex.begin() + firstNew, f.blockTypeIndex.end());
	f.blocks.resize(firstNew);
	f.blockTypeIndex.resize(firstNew);
	for (int n = firstNew; n < firstNew + (int)blocks.size(); n++) {
		int to = where[n];
		if (to < firstNew) { f.blocks[to] = std::move(blocks[n - firstNew]); f.blockTypeIndex[to] = types[n - firstNew]; }
		else { f.blocks.push_back(std::move(blocks[n - firstNew])); f.blockTypeIndex.push_back(types[n - firstNew]); }
	}
}

static JobResult runJob(File& f, AVObject& root, const Job& job, const std::string& physicsPath, std::vector<int>& newNodeBlocks)
{
	JobResult res;
	res.shape = job.shape;
	int shapeBlock = findBlockByName(f, job.shape);
	if (shapeBlock < 0) throw std::runtime_error("shape not found: " + job.shape);
	Geometry geom;
	geom.read(f.blocks[shapeBlock]);
	bool strips = f.typeOf(geom.data) == "NiTriStripsData";
	GeometryData gd;
	gd.read(f.blocks[geom.data], strips);
	// An unskinned shape (a weapon charm, a rigid prop) is skinned first, all of it on the anchor: a node of that name
	// in the NIF, else a new one on the root where the root is (a weapon's "Weapon": its NIF's space is that node's).
	// The shape's own transform becomes the skin transform, and its shader is told it is skinned.
	if (geom.skin < 0) {
		int anchorBlk = findBlockByName(f, job.anchor);
		if (anchorBlk >= 0 && !isNodeType(f.typeOf(anchorBlk))) throw std::runtime_error(job.anchor + " is not a node");
		if (anchorBlk < 0) {
			AVObject node;
			node.isNode = true;
			node.name = f.addString(job.anchor);
			Writer w;
			node.write(w);
			anchorBlk = f.addBlock("NiNode", w.b);
			root.children.push_back(anchorBlk);
		}
		SkinData nsd;
		nsd.skinTransform = geom.av.local;
		nsd.bones.resize(1);
		std::vector<Weights> one(gd.vertices.size(), Weights{ { 0, 1.0f } });
		for (size_t v = 0; v < gd.vertices.size(); v++) nsd.bones[0].weights.push_back({ (uint16_t)v, 1.0f });
		SkinInstance nsi;
		nsi.data = f.addBlock("NiSkinData", nsd.write());
		nsi.partition = f.addBlock("NiSkinPartition", buildPartition(one, gd.triangles, 1).write());
		nsi.skeletonRoot = f.roots[0];
		nsi.bones = { anchorBlk };
		geom.skin = f.addBlock("NiSkinInstance", nsi.write());
		geom.av.local = Transform();
		for (auto p : geom.av.properties) {
			const std::string& t = f.typeOf(p);
			if (t.rfind("BSShader", 0) != 0 || t == "BSShaderTextureSet") continue;
			Reader r(f.blocks[p].data(), f.blocks[p].size());
			r.u32(); r.skip(4 * r.u32()); r.i32(); r.u16(); r.u32();   // name, extra data, controller, flags, shader type
			f.blocks[p][r.pos] |= 0x02;                                  // shader flags: Skinned
		}
		f.blocks[shapeBlock] = geom.write();
		printf("  %s had no skin: skinned to %s\n", job.shape.c_str(), job.anchor.c_str());
	}
	bool dism = f.typeOf(geom.skin) == "BSDismemberSkinInstance";
	SkinInstance si;
	si.read(f.blocks[geom.skin], dism);
	SkinData sd;
	sd.read(f.blocks[si.data]);
	SkinPartition sp;
	sp.read(f.blocks[si.partition]);
	size_t nv = gd.vertices.size();
	printf("\n%s '%s': %zu verts, %zu tris, %zu bones, %zu partitions\n", job.kind == Job::Cloth ? "cloth" : job.kind == Job::Painted ? "painted chains" : job.kind == Job::Pieces ? "pieces" : "chains", job.shape.c_str(),
		nv, gd.triangles.size(), si.bones.size(), sp.parts.size());
	for (auto b : si.bones) if (strName(f, b).rfind("SMP ", 0) == 0) throw std::runtime_error("shape already has SMP bones");

	std::vector<Vec3> pos(nv);
	for (size_t v = 0; v < nv; v++) pos[v] = sd.skinTransform.apply(gd.vertices[v]);
	std::vector<Weights> orig(nv);
	for (size_t b = 0; b < sd.bones.size(); b++)
		for (auto& vw : sd.bones[b].weights) orig[vw.index].push_back({ (int)b, vw.weight });
	std::vector<std::string> boneNames;
	for (auto b : si.bones) boneNames.push_back(strName(f, b));
	// --detach: the weights of a bone handed to another one of the shape's bones, before anything else (a poncho
	// over the arm: its arm weights onto the clavicle, so the cloth rests on the shoulder and the arm moves under it).
	for (auto& d : job.detach) {
		int from = -1, to = -1;
		for (size_t b = 0; b < boneNames.size(); b++) {
			if (boneNames[b] == d.first) from = (int)b;
			if (boneNames[b] == d.second) to = (int)b;
		}
		if (from < 0) { printf("  note: --detach %s: not one of the shape's bones\n", d.first.c_str()); continue; }
		if (to < 0) throw std::runtime_error("--detach target is not one of the shape's skin bones: " + d.second);
		int moved = 0;
		for (auto& w : orig) for (auto& i : w) if (i.bone == from) { i.bone = to; moved++; }
		printf("  detached %s: %d weights onto %s\n", d.first.c_str(), moved, d.second.c_str());
	}
	for (auto& w : orig) normalizeTop4(w);
	res.kinematic = boneNames;
	const int bodyBoneCount = (int)boneNames.size();
	auto addBone = [&](NewBone b) {
		AVObject node;
		node.isNode = true;
		node.name = f.addString(b.name);
		node.flags = 0xE;
		node.local.trans = b.bind;
		Writer w;
		node.write(w);
		int blk = f.addBlock("NiNode", w.b);
		newNodeBlocks.push_back(blk);
		b.slot = (int)si.bones.size();
		si.bones.push_back(blk);
		SkinData::Bone bd;
		bd.skinToBone.trans = b.bind * -1.0f;
		sd.bones.push_back(bd);
		boneNames.push_back(b.name);
		res.bones.push_back(b);
		return b.slot;
	};
	bool anchorFound = false;
	for (auto& n : boneNames) if (n == job.anchor) anchorFound = true;
	if (!anchorFound) printf("  note: %s is not one of the shape's skin bones; the anchors hang from it all the same\n", job.anchor.c_str());

	float zMax = -1e9f, zMin = 1e9f;
	for (auto& p : pos) { zMax = std::max(zMax, p.z); zMin = std::min(zMin, p.z); }
	std::vector<Weights> neww = orig;
	std::string tag = job.kind == Job::Cloth ? "Cape" : job.kind == Job::Painted ? "Paint" : job.kind == Job::Pieces ? "Piece" : "Rope";
	// Names stay unique across shapes of the outfit.
	static int shapeIndex = 0;
	char prefix[32];
	snprintf(prefix, sizeof prefix, "SMP %s%d", tag.c_str(), shapeIndex++);

	if (job.kind == Job::Cloth) {
		// Each vertex's place on the grid: an angle around the body (columns) and a distance down the cloth (rows), s.
		// By height (default): s = top - z, the rows level rings. --radial: s = the distance from the neck, the rows
		// rings around it, so a cape draped over the shoulders hangs from the collar along its own surface (across the
		// shoulder in front, straight down the back). Vertices with s below the top row's keep their weights.
		Vec3 centre{ job.axisX, job.axisY, 0 };
		float s0 = 0;
		if (job.radial) {
			if (job.neckSet) centre = job.neck;
			else {
				Vec3 c{}; int k = 0;
				for (auto& p : pos) if (p.z > zMax - 4.0f) { c = c + p; k++; }
				centre = c * (1.0f / k);
			}
			s0 = job.collar;
		}
		float zTop = job.top > -1e8f ? job.top : zMin + 0.8f * (zMax - zMin);
		auto sOf = [&](Vec3 p) { return job.radial ? length(p - centre) : zTop - p.z; };
		float lo = job.angleLo, hi = job.angleHi;
		// 0 = back, 90 = right; with --angles, measured from lo round the full circle, so a range can run past 180 (a
		// cape that wraps across the front: --angles -135 210).
		auto angleOf = [&](Vec3 p) {
			float a = std::atan2(p.x - centre.x, -(p.y - centre.y)) * 180.0f / PI;
			if (job.angles) a = lo + std::fmod(a - lo + 720.0f, 360.0f);
			return a;
		};
		float s1 = -1e9f;
		if (!job.angles) { lo = 1e9f; hi = -1e9f; }
		for (auto& p : pos) if (sOf(p) > s0) {
			s1 = std::max(s1, sOf(p));
			if (!job.angles) { float a = angleOf(p); lo = std::min(lo, a); hi = std::max(hi, a); }
		}
		if (s1 < -1e8f) throw std::runtime_error("no vertices below the top row");
		{
			int hist[12] = {};
			for (auto& p : pos) if (sOf(p) > s0) hist[std::min(11, std::max(0, (int)((angleOf(p) + 180.0f) / 30.0f)))]++;
			printf("  vertices below the top row by angle (30 deg bins from -180):");
			for (int h : hist) printf(" %d", h);
			puts("");
		}
		int cols = job.cols, rows = job.rows;
		float ds = (s1 - s0) / (rows - 1), da = (hi - lo) / (cols - 1);
		if (job.radial)
			printf("  grid %dx%d from the neck (%.1f %.1f %.1f), %.0f..%.0f deg (%.1f deg apart), %.1f..%.1f units from it (%.1f apart), anchors on %s\n",
				cols, rows, centre.x, centre.y, centre.z, lo, hi, da, s0, s1, ds, job.anchor.c_str());
		else
			printf("  grid %dx%d around (%.1f, %.1f), %.0f..%.0f deg (%.1f deg apart), z %.1f..%.1f (%.1f apart), anchors on %s\n",
				cols, rows, centre.x, centre.y, lo, hi, da, zTop - s1, zTop, ds, job.anchor.c_str());
		// A grid point's bind position: the mean of the cloth's vertices around it, so the bones lie on the cloth;
		// where none are near (past the cloth's edge), the vertex nearest on the grid.
		auto bindAt = [&](int c, int r) {
			float a = lo + c * da, s = s0 + r * ds;
			Vec3 m{}; int k = 0;
			float bestD = 1e30f; Vec3 best{};
			for (auto& p : pos) {
				float du = (angleOf(p) - a) / da, dw = (sOf(p) - s) / ds;
				if (std::fabs(du) <= 0.5f && std::fabs(dw) <= 0.5f) { m = m + p; k++; }
				float d2 = du * du + dw * dw;
				if (d2 < bestD) { bestD = d2; best = p; }
			}
			return k ? m * (1.0f / k) : best;
		};
		std::vector<int> slot(cols * rows);
		std::vector<std::string> name(cols * rows);
		for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
			NewBone b;
			char nm[64];
			snprintf(nm, sizeof nm, "%s C%02d R%d", prefix, c, r);
			b.name = nm;
			b.parent = r == 0 ? job.anchor : name[(r - 1) * cols + c];
			b.role = r == 0 ? "anchor" : "cloth";
			b.bind = bindAt(c, r);
			slot[r * cols + c] = addBone(b);
			name[r * cols + c] = nm;
			// Only the anchors keep out of collisions: the rows below must give way to an arm swinging under them.
			if (r < job.fixedRows) res.topBones.push_back(nm);
		}
		for (int c = 0; c < cols; c++) {
			std::vector<std::string> chain;
			for (int r = 0; r < rows; r++) chain.push_back(name[r * cols + c]);
			res.warp.push_back(chain);
		}
		for (int c = 0; c + 1 < cols; c++) for (int r = 1; r < rows; r++) res.weft.push_back({ name[r * cols + c], name[r * cols + c + 1] });

		int changed = 0;
		for (size_t v = 0; v < nv; v++) {
			if (sOf(pos[v]) <= s0) continue;
			float u = (angleOf(pos[v]) - lo) / da, w = (sOf(pos[v]) - s0) / ds;
			u = std::min(std::max(u, 0.0f), (float)(cols - 1));
			w = std::min(std::max(w, 0.0f), (float)(rows - 1));
			int c0 = std::min((int)u, cols - 2), r0 = std::min((int)w, rows - 2);
			float fu = u - c0, fw = w - r0;
			Weights out;
			auto add = [&](int c, int r, float k) {
				if (k <= 0) return;
				if (r == 0) { for (auto& i : orig[v]) out.push_back({ i.bone, i.w * k }); }
				else out.push_back({ slot[r * cols + c], k });
			};
			add(c0, r0, (1 - fu) * (1 - fw));
			add(c0 + 1, r0, fu * (1 - fw));
			add(c0, r0 + 1, (1 - fu) * fw);
			add(c0 + 1, r0 + 1, fu * fw);
			normalizeTop4(out);
			neww[v] = out;
			changed++;
		}
		printf("  reweighted %d vertices onto %d cloth bones\n", changed, cols * (rows - 1));

		// --keep: the cloth where it is pinned under armor keeps its own weights (static, with the body), found by
		// its weight on the given bone in the source: fully from w1 up, blending into the cloth bones from w0.
		for (auto& k : job.keep) {
			int bone = -1;
			for (size_t b = 0; b < boneNames.size(); b++) if (boneNames[b] == k.bone) bone = (int)b;
			if (bone < 0) throw std::runtime_error("--keep bone is not one of the shape's skin bones: " + k.bone);
			int full = 0, blended = 0;
			for (size_t v = 0; v < nv; v++) {
				float w = 0;
				for (auto& i : orig[v]) if (i.bone == bone) w += i.w;
				float t = std::clamp((w - k.w0) / std::max(k.w1 - k.w0, 1e-6f), 0.0f, 1.0f);
				t = t * t * (3 - 2 * t);
				if (t <= 0) continue;
				Weights mixed;
				for (auto& i : neww[v]) mixed.push_back({ i.bone, i.w * (1 - t) });
				for (auto& i : orig[v]) {
					bool merged = false;
					for (auto& m : mixed) if (m.bone == i.bone) { m.w += i.w * t; merged = true; }
					if (!merged) mixed.push_back({ i.bone, i.w * t });
				}
				normalizeTop4(mixed);
				neww[v] = mixed;
				(t >= 1 ? full : blended)++;
			}
			printf("  kept %s's own weights: %d vertices static, %d blended\n", k.bone.c_str(), full, blended);
		}
	}
	else if (job.kind == Job::Pieces) {
		// A chain of rigid bodies made of the mesh's pieces (a charm: rings, links, a pendant): the pieces listed top
		// first by their centre along --up, grouped by --groups ("0,1|2|3,4": groups split by |). The first group stays
		// on the anchor (where it is tied on); each other group is one bone, all of its vertices on it, hanging from
		// the group above at its own top, the last one the heavy end.
		auto pieces = piecesOf(gd, pos);
		auto along = [&](int v) { return dot(pos[v], job.up); };
		struct Piece { std::vector<int> verts; float centre, lo, hi; };
		std::vector<Piece> list;
		for (auto& pc : pieces) {
			Piece p{ pc, 0, 1e9f, -1e9f };
			for (int v : pc) { float a = along(v); p.centre += a; p.lo = std::min(p.lo, a); p.hi = std::max(p.hi, a); }
			p.centre /= pc.size();
			list.push_back(p);
		}
		std::sort(list.begin(), list.end(), [](const Piece& a, const Piece& b) { return a.centre > b.centre; });
		for (size_t k = 0; k < list.size(); k++) {
			Vec3 c{};
			for (int v : list[k].verts) c = c + pos[v];
			c = c * (1.0f / list[k].verts.size());
			printf("  piece %zu: %zu verts, centre (%.2f %.2f %.2f), up %.2f..%.2f\n", k, list[k].verts.size(), c.x, c.y, c.z, list[k].lo, list[k].hi);
		}
		std::vector<std::vector<int>> groups(1);
		for (size_t at = 0; at <= job.groups.size(); ) {
			size_t end = job.groups.find_first_of(",|", at);
			if (end == std::string::npos) end = job.groups.size();
			std::string tok = job.groups.substr(at, end - at);
			if (!tok.empty()) {
				int k = atoi(tok.c_str());
				if (k < 0 || k >= (int)list.size()) throw std::runtime_error("--groups: no piece " + tok);
				groups.back().push_back(k);
			}
			if (end < job.groups.size() && job.groups[end] == '|') groups.emplace_back();
			at = end + 1;
		}
		if (job.groups.empty() || groups.size() < 2) throw std::runtime_error("--pieces needs --groups: the pieces above, the fixed group first, then each body's, split by |");
		std::vector<int> used(list.size(), 0);
		for (auto& g : groups) for (int k : g) if (used[k]++) throw std::runtime_error("--groups: piece " + std::to_string(k) + " is in two groups");
		for (size_t k = 0; k < list.size(); k++) if (!used[k]) printf("  note: piece %zu is in no group: it stays on the anchor\n", k);
		// A group's top: the centre of its vertices within 0.1 units of its highest point (where it hangs).
		auto topOf = [&](const std::vector<int>& g) {
			float hi = -1e9f;
			for (int k : g) hi = std::max(hi, list[k].hi);
			Vec3 c{}; int n = 0;
			for (int k : g) for (int v : list[k].verts) if (along(v) > hi - 0.1f) { c = c + pos[v]; n++; }
			return c * (1.0f / std::max(n, 1));
		};
		std::vector<std::string> names;
		for (size_t gi = 1; gi < groups.size(); gi++) {
			if (groups[gi].empty()) throw std::runtime_error("--groups: an empty group");
			const bool last = gi + 1 == groups.size();
			if (gi == 1) {   // where the chain is tied on: an anchor bone at the first body's top
				NewBone a;
				a.name = std::string(prefix) + " L0";
				a.parent = job.anchor;
				a.role = "anchor";
				a.bind = topOf(groups[1]);
				addBone(a);
				names.push_back(a.name);
				res.topBones.push_back(a.name);
			}
			NewBone b;
			b.name = std::string(prefix) + " L" + std::to_string(gi);
			b.parent = names.back();
			b.role = last ? "end" : "rope";
			b.bind = last ? topOf(groups[gi]) : topOf(groups[gi + 1]);   // its lower end: the next body hangs there
			size_t nverts = 0;
			for (int k : groups[gi]) for (int v : list[k].verts) { b.piecePts.push_back(pos[v]); nverts++; }
			if (last) {   // the end's weight where its pieces are (a box around them, as rope ends have)
				for (auto& p : b.piecePts) {
					b.boxMin = { std::min(b.boxMin.x, p.x), std::min(b.boxMin.y, p.y), std::min(b.boxMin.z, p.z) };
					b.boxMax = { std::max(b.boxMax.x, p.x), std::max(b.boxMax.y, p.y), std::max(b.boxMax.z, p.z) };
				}
				b.hasBox = true;
			}
			const int slot = addBone(b);
			for (int k : groups[gi]) for (int v : list[k].verts) neww[v] = { { slot, 1.0f } };
			names.push_back(b.name);
			printf("  body %zu (%s): %zu verts, bone at (%.2f %.2f %.2f)\n", gi, b.role.c_str(), nverts, b.bind.x, b.bind.y, b.bind.z);
		}
		res.warp.push_back(names);
	}
	else if (job.kind == Job::Painted) {
		// Each painted chain: links laid along its stroke (from where the painting started, the end that is tied on, to
		// where it stopped), evenly by length. A painted vertex goes onto the two links it lies between (by where it
		// projects onto the stroke), blended with its own weights by its paint strength: a soft brush edge is a soft seam.
		// Near the tie-on the links hand over to the vertex's own weights (link 0 is the anchor), as a rope's do.
		// A vertex painted by several chains belongs to the one that painted it hardest.
		std::vector<int> owner(nv, -1);
		std::vector<float> strength(nv, 0.0f);
		for (size_t c = 0; c < job.painted.size(); c++)
			for (auto& [v, s] : job.painted[c].paint)
				if (v >= 0 && v < (int)nv && s > strength[v]) { strength[v] = s; owner[v] = (int)c; }
		for (size_t c = 0; c < job.painted.size(); c++) {
			const PaintChain& pc = job.painted[c];
			std::vector<Vec3> line;
			for (int v : pc.path) if (v >= 0 && v < (int)nv && (line.empty() || length(pos[v] - line.back()) > 0.05f)) line.push_back(pos[v]);
			if (line.size() < 2) throw std::runtime_error("painted chain " + std::to_string(c) + " on " + job.shape + ": its stroke is too short");
			std::vector<float> at(line.size(), 0.0f);
			for (size_t i = 1; i < line.size(); i++) at[i] = at[i - 1] + length(line[i] - line[i - 1]);
			const float total = at.back();
			auto pointAt = [&](float s) {
				size_t i = 1;
				while (i + 1 < line.size() && at[i] < s) i++;
				float t = std::clamp((s - at[i - 1]) / std::max(at[i] - at[i - 1], 1e-6f), 0.0f, 1.0f);
				return line[i - 1] + (line[i] - line[i - 1]) * t;
			};
			auto project = [&](Vec3 p) {   // the stroke length at the point of the stroke nearest p
				float best = 1e30f, s = 0;
				for (size_t i = 1; i < line.size(); i++) {
					Vec3 d = line[i] - line[i - 1];
					float dd = std::max(d.x * d.x + d.y * d.y + d.z * d.z, 1e-9f);
					Vec3 q = p - line[i - 1];
					float t = std::clamp((q.x * d.x + q.y * d.y + q.z * d.z) / dd, 0.0f, 1.0f);
					Vec3 r = q - d * t;
					float e = r.x * r.x + r.y * r.y + r.z * r.z;
					if (e < best) { best = e; s = at[i - 1] + t * (at[i] - at[i - 1]); }
				}
				return s;
			};
			const int n = pc.links;
			const float step = total / n;
			// The bones' places: evenly along the stroke, or where the Maker moved them. With a point moved, a vertex goes
			// onto the links by where it projects onto the line through the bones (the stroke no longer says where they are).
			std::vector<Vec3> at3(n + 1);
			for (int k = 0; k <= n; k++) { auto it = pc.points.find(k); at3[k] = it != pc.points.end() ? it->second : pointAt(k * step); }
			auto projectBones = [&](Vec3 p) {   // in links (0..n) along the line through the bones, nearest p
				float best = 1e30f, u = 0;
				for (int i = 1; i <= n; i++) {
					Vec3 d = at3[i] - at3[i - 1];
					float dd = std::max(d.x * d.x + d.y * d.y + d.z * d.z, 1e-9f);
					Vec3 q = p - at3[i - 1];
					float t = std::clamp((q.x * d.x + q.y * d.y + q.z * d.z) / dd, 0.0f, 1.0f);
					Vec3 r = q - d * t;
					float e = r.x * r.x + r.y * r.y + r.z * r.z;
					if (e < best) { best = e; u = (i - 1) + t; }
				}
				return u;
			};
			std::vector<int> slots;
			std::vector<std::string> names;
			for (int k = 0; k <= n; k++) {
				NewBone b;
				char nm[64];
				snprintf(nm, sizeof nm, "%s P%zu L%d", prefix, c, k);
				b.name = nm;
				b.parent = k == 0 ? pc.anchor : names.back();
				char role[32];
				snprintf(role, sizeof role, "paint%d", pc.global);
				b.role = k == 0 ? "anchor" : role;
				b.bind = at3[k];
				slots.push_back(addBone(b));
				names.push_back(nm);
				// The anchor and the first link sit where the chain is tied on: they stay out of collisions.
				if (k < 2) res.topBones.push_back(nm);
			}
			res.warp.push_back(names);
			res.warpPaint.push_back(pc.global);
			int moved = 0;
			for (size_t v = 0; v < nv; v++) {
				if (owner[v] != (int)c) continue;
				float u = std::clamp(pc.points.empty() ? project(pos[v]) / std::max(step, 1e-6f) : projectBones(pos[v]), 0.0f, (float)n);
				int k0 = std::min((int)u, n - 1);
				float fk = u - k0, s = std::clamp(strength[v], 0.0f, 1.0f);
				Weights out;
				auto add = [&](int k, float w) {
					if (w <= 0) return;
					if (k == 0) { for (auto& i : orig[v]) out.push_back({ i.bone, i.w * w }); }
					else out.push_back({ slots[k], w });
				};
				add(k0, (1 - fk) * s);
				add(k0 + 1, fk * s);
				for (auto& i : orig[v]) {
					bool merged = false;
					for (auto& m : out) if (m.bone == i.bone) { m.w += i.w * (1 - s); merged = true; }
					if (!merged && 1 - s > 0) out.push_back({ i.bone, i.w * (1 - s) });
				}
				normalizeTop4(out);
				neww[v] = out;
				moved++;
			}
			printf("  painted chain %zu: hangs from %s, %d links of %.1f units, %d vertices (stiffness %.2f, weight %.2f)\n",
				c, pc.anchor.c_str(), n, step, moved, pc.stiffness, pc.weight);
		}
	}
	else {
		auto pieces = piecesOf(gd, pos);
		struct Strand { std::vector<int> verts; float top, bottom; std::vector<int> slots; std::vector<std::string> names; Vec3 end; };
		std::vector<Strand> strands;
		std::vector<std::vector<int>> rigid;
		int knots = 0;
		for (auto& pc : pieces) {
			float a = 1e9f, b = -1e9f;
			for (int v : pc) { a = std::min(a, pos[v].z); b = std::max(b, pos[v].z); }
			bool reachesTop = b > zMax - 2.0f;
			if (reachesTop && b - a > 4.0f) strands.push_back({ pc, b, a });
			else if (reachesTop) knots++;
			else rigid.push_back(pc);
		}
		if (strands.empty()) throw std::runtime_error("no strands: no piece reaches the top and is longer than 4 units");
		printf("  %zu strand(s), %d knot(s) kept as they are, %zu rigid piece(s)\n", strands.size(), knots, rigid.size());
		// The centre of a strand's vertices within a slab around a height.
		auto centreAt = [&](const Strand& s, float z, float half) {
			Vec3 c{}; int k = 0;
			for (int v : s.verts) if (std::fabs(pos[v].z - z) <= half) { c = c + pos[v]; k++; }
			if (!k) for (int v : s.verts) { c = c + pos[v]; k++; }
			c = c * (1.0f / k);
			c.z = z;
			return c;
		};
		for (size_t si_ = 0; si_ < strands.size(); si_++) {
			Strand& s = strands[si_];
			int n = job.links;
			float len = s.top - s.bottom, step = len / n;
			for (int k = 0; k <= n; k++) {
				float z = s.top - k * step;
				NewBone b;
				char nm[64];
				snprintf(nm, sizeof nm, "%s S%zu L%d", prefix, si_, k);
				b.name = nm;
				b.parent = k == 0 ? job.anchor : s.names.back();
				b.role = k == 0 ? "anchor" : (k == n ? "end" : "rope");
				b.bind = centreAt(s, z, std::max(step * 0.5f, 0.5f));
				s.slots.push_back(addBone(b));
				s.names.push_back(nm);
				// Only the last link (the rigid pieces) collides: the thin rope above it would catch on the hip and thigh
				// every stride and shake; it just hangs.
				if (k < n) res.topBones.push_back(nm);
			}
			s.end = res.bones.back().bind;
			res.warp.push_back(s.names);
			printf("  strand %zu: %zu verts, z %.1f..%.1f, %d links of %.1f units\n", si_, s.verts.size(), s.bottom, s.top, n, step);
			for (int v : s.verts) {
				float t = std::min(std::max((s.top - pos[v].z) / step, 0.0f), (float)n);
				int k0 = std::min((int)t, n - 1);
				float fk = t - k0;
				Weights out;
				auto add = [&](int k, float w) {
					if (w <= 0) return;
					if (k == 0) { for (auto& i : orig[v]) out.push_back({ i.bone, i.w * w }); }
					else out.push_back({ s.slots[k], w });
				};
				add(k0, 1 - fk);
				add(k0 + 1, fk);
				// --keep-top a b: where it is tied on (the top a units of the strand) the rope keeps its own
				// weights, blending into the links down to b units.
				if (job.keepTopB > 0) {
					float t = std::clamp((s.top - pos[v].z - job.keepTopA) / std::max(job.keepTopB - job.keepTopA, 1e-6f), 0.0f, 1.0f);
					t = 1 - t * t * (3 - 2 * t);
					if (t > 0) {
						for (auto& i : out) i.w *= 1 - t;
						for (auto& i : orig[v]) {
							bool merged = false;
							for (auto& m : out) if (m.bone == i.bone) { m.w += i.w * t; merged = true; }
							if (!merged) out.push_back({ i.bone, i.w * t });
						}
					}
				}
				normalizeTop4(out);
				neww[v] = out;
			}
		}
		std::vector<int> rigidStrand;
		for (auto& pc : rigid) {
			Vec3 c{}; float zt = -1e9f;
			for (int v : pc) { c = c + pos[v]; zt = std::max(zt, pos[v].z); }
			c = c * (1.0f / pc.size());
			int best = 0; float bestD = 1e30f;
			for (size_t k = 0; k < strands.size(); k++) {
				Vec3 d = strands[k].end - Vec3{ c.x, c.y, zt };
				float dd = d.x * d.x + d.y * d.y + d.z * d.z;
				if (dd < bestD) { bestD = dd; best = (int)k; }
			}
			for (int v : pc) neww[v] = { { strands[best].slots.back(), 1.0f } };
			// The end bone gets a box around its rigid pieces, so they knock against each other (a skinned shape does
			// not collide with itself): the bone-to-bone collisions of the physics world.
			for (auto& b : res.bones) if (b.name == strands[best].names.back()) {
				for (int v : pc) {
					b.boxMin = { std::min(b.boxMin.x, pos[v].x), std::min(b.boxMin.y, pos[v].y), std::min(b.boxMin.z, pos[v].z) };
					b.boxMax = { std::max(b.boxMax.x, pos[v].x), std::max(b.boxMax.y, pos[v].y), std::max(b.boxMax.z, pos[v].z) };
				}
				for (int v : pc) b.piecePts.push_back(pos[v]);
				b.hasBox = true;
			}
			printf("  rigid piece: %zu verts, centre (%.1f %.1f %.1f), on the end of strand %d (%.1f units away)\n", pc.size(), c.x, c.y, c.z, best, std::sqrt(bestD));
			rigidStrand.push_back(best);
		}
		// How close the rigid pieces of different strands are modelled (they collide vertex against vertex: closer
		// than twice the margin, they would start in contact).
		for (size_t i = 0; i < rigid.size(); i++) for (size_t j = i + 1; j < rigid.size(); j++) {
			if (rigidStrand[i] == rigidStrand[j]) continue;
			float g = 1e30f;
			for (int a : rigid[i]) for (int b : rigid[j]) g = std::min(g, length(pos[a] - pos[b]));
			printf("  rigid pieces %zu and %zu (strands %d, %d): %.2f units apart\n", i, j, rigidStrand[i], rigidStrand[j], g);
		}
	}

	// NiSkinData: weights and bounds for every bone.
	for (auto& b : sd.bones) b.weights.clear();
	for (size_t v = 0; v < nv; v++) for (auto& i : neww[v]) sd.bones[i.bone].weights.push_back({ (uint16_t)v, i.w });
	for (auto& b : sd.bones) {
		if (b.weights.empty()) { b.center = {}; b.radius = 0; continue; }
		Vec3 c{};
		for (auto& vw : b.weights) c = c + b.skinToBone.apply(pos[vw.index]);
		c = c * (1.0f / b.weights.size());
		float rad = 0;
		for (auto& vw : b.weights) rad = std::max(rad, length(b.skinToBone.apply(pos[vw.index]) - c));
		b.center = c; b.radius = rad;
	}

	SkinPartition np = buildPartition(neww, gd.triangles, sd.bones.size());
	printf("  partitions: %zu (", np.parts.size());
	for (auto& p : np.parts) printf(" %zuv/%zut/%zub", p.vertexMap.size(), p.triangles.size(), p.bones.size());
	printf(" )\n");
	if (dism) {
		SkinInstance::Part proto = si.parts.empty() ? SkinInstance::Part{ 257, 0 } : si.parts[0];
		si.parts.assign(np.parts.size(), proto);
	}

	std::string smpBones;
	for (size_t b = 0; b < boneNames.size(); b++) {
		std::string parent = (int)b >= bodyBoneCount ? res.bones[b - bodyBoneCount].parent : "";
		smpBones += boneNames[b] + "|" + parent + ";";
	}
	addSmpBones(f, geom.av, smpBones);
	addStringExtra(f, geom.av, "HDT Skinned Mesh Physics Object", physicsPath);
	if (f.blocks[geom.data].size() > 6) f.blocks[geom.data][6] |= 0x01 | 0x20;   // keep XYZ and bone data after upload
	f.blocks[shapeBlock] = geom.write();
	f.blocks[geom.skin] = si.write();
	f.blocks[si.data] = sd.write();
	f.blocks[si.partition] = np.write();
	return res;
}

int main(int argc, char** argv)
{
	if (argc < 6) {
		printf("usage: smpdress in.nif out.nif \"Data\\relative\\physics.xml\" out.xml --cloth \"Shape\" [options] --chains \"Shape\" [options] [--collide \"Shape\"]...\n");
		return 1;
	}
	std::vector<Job> jobs;
	std::vector<std::string> colliders;
	std::vector<std::string> collideSkip;   // --collide-skip "Bone": the colliders' triangles weighted only to these bones are left out
	float proxyCell = 0;                    // --proxy cell: the colliders' low-poly stand-ins (0: collide with the render meshes)
	int proxyMaxTris = 0;                   // --proxy-tris n: coarsen each stand-in's cell until it has at most n triangles
	float clothMargin = 1.0f, bodyMargin = 0.5f, penetration = 3.0f;   // --cloth-margin, --body-margin, --penetration
	float bend = 1.0f, stiffness = 1.0f;   // --bend, --stiffness: the cloth links' swing limits and springs, scaled
	bool weft = true;                      // --no-weft: no links between neighbouring columns
	float clothFriction = 0.9f;            // --cloth-friction
	float clothMass = 0.02f, clothRestitution = 0.2f;   // --cloth-mass, --cloth-restitution
	float clothDamping[2] = { 0.6f, 0.8f };  // --cloth-damping linear angular
	int paintCount = 0;                      // painted chains over every --painted shape
	auto fmtNum = [](float v) { char b[32]; snprintf(b, sizeof b, "%g", v); return std::string(b); };
	// The cloth links' angular limits (x, y swing; z twist), scaled by --bend and kept under 1.5 rad (the twist under
	// 0.5: more lets the top row turn and the cloth slide sideways off the shoulder), and their springs back toward the
	// rest shape, scaled by --stiffness.
	auto limits = [&](float x, float y, float z) {
		float b = bend;
		auto c = [&](float v, float cap) { return fmtNum(std::min(v * b, cap)); };
		std::string cx = c(x, 1.5f), cy = c(y, 1.5f), cz = c(z, 0.5f);
		return "\t\t<angularLowerLimit x=\"-" + cx + "\" y=\"-" + cy + "\" z=\"-" + cz + "\"/>\n\t\t<angularUpperLimit x=\"" + cx + "\" y=\"" + cy + "\" z=\"" + cz + "\"/>";
	};
	auto spring = [&](float k) { std::string s = fmtNum(k * stiffness); return "\t\t<angularStiffness x=\"" + s + "\" y=\"" + s + "\" z=\"" + s + "\"/>"; };
	for (int i = 5; i < argc; i++) {
		std::string a = argv[i];
		auto need = [&](int n) { if (i + n >= argc) { printf("%s needs %d value(s)\n", a.c_str(), n); exit(1); } };
		auto cur = [&]() -> Job& { if (jobs.empty()) { printf("%s before --cloth or --chains\n", a.c_str()); exit(1); } return jobs.back(); };
		if (a == "--cloth" || a == "--chains") { need(1); Job j; j.kind = a == "--cloth" ? Job::Cloth : Job::Chains; j.shape = argv[++i]; jobs.push_back(j); }
		else if (a == "--painted") {
			need(2);
			Job j; j.kind = Job::Painted; j.shape = argv[++i];
			try { j.painted = readPaintFile(argv[++i], j.shape); }
			catch (std::exception& e) { printf("error: %s\n", e.what()); return 2; }
			for (auto& pc : j.painted) pc.global = paintCount++;
			jobs.push_back(j);
		}
		else if (a == "--pieces") { need(1); Job j; j.kind = Job::Pieces; j.shape = argv[++i]; jobs.push_back(j); }
		else if (a == "--groups") { need(1); cur().groups = argv[++i]; }
		else if (a == "--up") { need(3); cur().up = normalize({ (float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3]) }); i += 3; }
		else if (a == "--collide") { need(1); colliders.push_back(argv[++i]); }
		else if (a == "--collide-skip") { need(1); collideSkip.push_back(argv[++i]); }
		else if (a == "--proxy") { need(1); proxyCell = (float)atof(argv[++i]); }
		else if (a == "--proxy-tris") { need(1); proxyMaxTris = atoi(argv[++i]); if (proxyCell <= 0) proxyCell = 1.0f; }
		else if (a == "--anchor") { need(1); cur().anchor = argv[++i]; }
		else if (a == "--top") { need(1); cur().top = (float)atof(argv[++i]); }
		else if (a == "--angles") { need(2); cur().angles = true; cur().angleLo = (float)atof(argv[++i]); cur().angleHi = (float)atof(argv[++i]); }
		else if (a == "--axis") { need(2); cur().axisX = (float)atof(argv[++i]); cur().axisY = (float)atof(argv[++i]); }
		else if (a == "--grid") { need(2); cur().cols = atoi(argv[++i]); cur().rows = atoi(argv[++i]); }
		else if (a == "--links") { need(1); cur().links = atoi(argv[++i]); }
		else if (a == "--keep-top") { need(2); cur().keepTopA = (float)atof(argv[++i]); cur().keepTopB = (float)atof(argv[++i]); }
		else if (a == "--radial") cur().radial = true;
		else if (a == "--fixed-rows") { need(1); cur().fixedRows = atoi(argv[++i]); }
		else if (a == "--cloth-margin") { need(1); clothMargin = (float)atof(argv[++i]); }
		else if (a == "--body-margin") { need(1); bodyMargin = (float)atof(argv[++i]); }
		else if (a == "--penetration") { need(1); penetration = (float)atof(argv[++i]); }
		else if (a == "--bend") { need(1); bend = (float)atof(argv[++i]); }
		else if (a == "--stiffness") { need(1); stiffness = (float)atof(argv[++i]); }
		else if (a == "--no-weft") weft = false;
		else if (a == "--cloth-friction") { need(1); clothFriction = (float)atof(argv[++i]); }
		else if (a == "--cloth-mass") { need(1); clothMass = (float)atof(argv[++i]); }
		else if (a == "--cloth-restitution") { need(1); clothRestitution = (float)atof(argv[++i]); }
		else if (a == "--cloth-damping") { need(2); clothDamping[0] = (float)atof(argv[++i]); clothDamping[1] = (float)atof(argv[++i]); }
		else if (a == "--collar") { need(1); cur().collar = (float)atof(argv[++i]); }
		else if (a == "--neck") { need(3); cur().neckSet = true; cur().neck = { (float)atof(argv[i + 1]), (float)atof(argv[i + 2]), (float)atof(argv[i + 3]) }; i += 3; }
		else if (a == "--keep") { need(3); Job::Keep k; k.bone = argv[++i]; k.w0 = (float)atof(argv[++i]); k.w1 = (float)atof(argv[++i]); cur().keep.push_back(k); }
		else if (a == "--detach") {
			need(1);
			std::string s = argv[++i];
			size_t eq = s.find('=');
			if (eq == std::string::npos) { printf("--detach wants \"Bone=Target\"\n"); return 1; }
			cur().detach.push_back({ s.substr(0, eq), s.substr(eq + 1) });
		}
		else { printf("unknown option %s\n", a.c_str()); return 1; }
	}
	// Only --proxy and --collide: add the stand-ins to a NIF that already has its physics (its XML is left as it is).
	const bool proxyOnly = jobs.empty() && proxyCell > 0 && !colliders.empty();
	if (jobs.empty() && !proxyOnly) { printf("nothing to do: give --cloth, --chains and/or --painted (or --proxy with --collide)\n"); return 1; }
	const std::string physicsPath = argv[3], xmlOut = argv[4];
	try {
		File f;
		f.load(argv[1]);
		AVObject root;
		{
			Reader rr(f.blocks[f.roots[0]].data(), f.blocks[f.roots[0]].size());
			root.read(rr, true);
		}
		Bytes rootTail(f.blocks[f.roots[0]].begin() + root.tailPos, f.blocks[f.roots[0]].end());
		std::vector<int> newNodeBlocks;
		std::vector<JobResult> results;
		for (auto& j : jobs) results.push_back(runJob(f, root, j, physicsPath, newNodeBlocks));

		// --proxy: each collider's stand-in goes beside it (same parent); the physics file names the stand-in.
		std::vector<std::string> colliderShapes = colliders;
		if (proxyCell > 0) {
			for (size_t c = 0; c < colliders.size(); c++) {
				int blk = findBlockByName(f, colliders[c]);
				if (blk < 0) throw std::runtime_error("collision shape not found: " + colliders[c]);
				// A proxy given as the shape is rebuilt in place (same name: the physics file needs no change); a shape
				// that has a proxy already gets it replaced, not a second one.
				const bool rebuild = colliders[c].rfind("SMP Proxy:", 0) == 0;
				std::string name = rebuild ? colliders[c] : "SMP Proxy:" + colliders[c];
				const int existing = rebuild ? blk : findBlockByName(f, name);
				const int firstNew = (int)f.blocks.size();
				int t0, t1;
				float cellUsed = proxyCell;
				int pblk = buildProxyUnder(f, blk, proxyCell, proxyMaxTris, name, t0, t1, cellUsed);
				if (cellUsed != proxyCell) printf("proxy %s: cell %.2f to stay under %d triangles\n", colliders[c].c_str(), cellUsed, proxyMaxTris);
				if (existing >= 0) {
					replaceProxy(f, existing, pblk, firstNew);
					printf("proxy %s: %d -> %d triangles (%s)\n", colliders[c].c_str(), t0, t1, rebuild ? "rebuilt in place" : "replaces the old proxy");
					colliderShapes[c] = name;
					continue;
				}
				bool placed = std::find(root.children.begin(), root.children.end(), blk) != root.children.end();
				if (placed) root.children.push_back(pblk);
				for (size_t n = 0; n < f.blocks.size() && !placed; n++) {
					if (!isNodeType(f.typeOf(n)) || (int)n == f.roots[0]) continue;
					Reader nr(f.blocks[n].data(), f.blocks[n].size());
					AVObject node;
					node.read(nr, true);
					if (std::find(node.children.begin(), node.children.end(), blk) == node.children.end()) continue;
					Bytes tail(f.blocks[n].begin() + node.tailPos, f.blocks[n].end());
					node.children.push_back(pblk);
					Writer w;
					node.write(w);
					w.raw(tail.data(), tail.size());
					f.blocks[n] = w.b;
					placed = true;
				}
				if (!placed) throw std::runtime_error("collision shape has no parent node: " + colliders[c]);
				printf("proxy %s: %d -> %d triangles\n", colliders[c].c_str(), t0, t1);
				colliderShapes[c] = name;
			}
		}

		// The bone nodes under the root (a valid tree for NifSkope; the game drops them, the plugin rebuilds them).
		for (int b : newNodeBlocks) root.children.push_back(b);
		if (!proxyOnly) addStringExtra(f, root, "HDT Skinned Mesh Physics Object", physicsPath);
		{
			Writer w;
			root.write(w);
			w.raw(rootTail.data(), rootTail.size());
			f.blocks[f.roots[0]] = w.b;
		}
		std::set<std::string> kinematic;
		for (auto& r : results) for (auto& b : r.kinematic) kinematic.insert(b);
		for (auto& c : colliderShapes) {
			int blk = findBlockByName(f, c);
			if (blk < 0) throw std::runtime_error("collision shape not found: " + c);
			Geometry og;
			og.read(f.blocks[blk]);
			if (og.skin < 0) throw std::runtime_error("collision shape has no skin: " + c);
			if (og.data >= 0 && f.blocks[og.data].size() > 6) f.blocks[og.data][6] |= 0x01 | 0x20;
			SkinInstance os;
			os.read(f.blocks[og.skin], f.typeOf(og.skin) == "BSDismemberSkinInstance");
			for (auto b : os.bones) kinematic.insert(strName(f, b));
		}
		f.save(argv[2]);
		if (proxyOnly) {
			printf("\nwrote %s (%zu collision stand-ins; name them in the physics file's per-triangle-shapes)\n", argv[2], colliders.size());
			return 0;
		}

		std::string x;
		auto line = [&](const std::string& s) { x += s; x += "\n"; };
		line("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
		line("<!-- Generated by smpdress (NV SMP Physics): cloth grids in the style of FSMP's cloak pattern, rope chains with rigid pieces. -->");
		line("<system>");
		line("");
		line("\t<!-- Body bones: kinematic (no template, mass 0), animation driven. -->");
		for (auto& b : kinematic) line("\t<bone name=\"" + xmlEscape(b) + "\"/>");
		line("");
		line("\t<bone-default name=\"anchor\">");
		line("\t\t<restitution>1</restitution>");
		line("\t</bone-default>");
		line("\t<bone-default name=\"cloth\">");
		line("\t\t<mass>" + fmtNum(clothMass) + "</mass>");
		line("\t\t<inertia x=\"1\" y=\"1\" z=\"1\"/>");
		line("\t\t<linearDamping>" + fmtNum(clothDamping[0]) + "</linearDamping>");
		line("\t\t<angularDamping>" + fmtNum(clothDamping[1]) + "</angularDamping>");
		line("\t\t<friction>" + fmtNum(clothFriction) + "</friction>");
		line("\t\t<restitution>" + fmtNum(clothRestitution) + "</restitution>");
		line("\t\t<wind-factor>1</wind-factor>");
		line("\t</bone-default>");
		line("\t<!-- Rope links: light and damped; the mass steps up 2:1 at most to the end (a bigger jump makes the solver jitter). -->");
		line("\t<bone-default name=\"rope\">");
		line("\t\t<mass>0.05</mass>");
		line("\t\t<inertia x=\"1\" y=\"1\" z=\"1\"/>");
		line("\t\t<linearDamping>0.5</linearDamping>");
		line("\t\t<angularDamping>0.7</angularDamping>");
		line("\t\t<friction>0.5</friction>");
		line("\t\t<restitution>0.3</restitution>");
		line("\t\t<wind-factor>0.3</wind-factor>");
		line("\t</bone-default>");
		line("\t<!-- The last link carries the rigid pieces (a bottle): heavier, so it hangs and swings like a weight. -->");
		line("\t<bone-default name=\"end\">");
		line("\t\t<mass>0.1</mass>");
		line("\t\t<inertia x=\"1\" y=\"1\" z=\"1\"/>");
		line("\t\t<linearDamping>0.5</linearDamping>");
		line("\t\t<angularDamping>0.8</angularDamping>");
		line("\t\t<friction>0.4</friction>");
		line("\t\t<restitution>0.3</restitution>");
		line("\t\t<wind-factor>0</wind-factor>");
		line("\t</bone-default>");
		// Painted chains (the NV SMP Maker): each its own bone template and link template, from its stiffness and weight
		// (0..1). Stiffness narrows the swing (1.2 rad floppy .. 0.2 stiff, twist 0.3 .. 0.05) and strengthens the
		// springs back to the modelled shape (0.3 .. 12, squared: most of the range stays soft); weight is the links'
		// mass (0.02 .. 0.12) and damping (light and lively .. heavy and calm), and how much the wind moves them. At
		// 0.4 / 0.4 the links sit between smpdress's rope (free) and cloth (held) templates.
		struct PaintFeel { float stiffness, weight; };
		std::map<int, PaintFeel> paintFeel;
		for (auto& j : jobs) for (auto& pc : j.painted) paintFeel[pc.global] = { pc.stiffness, pc.weight };
		if (!paintFeel.empty()) line("\t<!-- Painted chains (NV SMP Maker): each with its own feel, from its stiffness and weight. -->");
		for (auto& [g, feel] : paintFeel) {
			const float w = feel.weight;
			line("\t<bone-default name=\"paint" + std::to_string(g) + "\">");
			line("\t\t<mass>" + fmtNum(0.02f + 0.1f * w) + "</mass>");
			line("\t\t<inertia x=\"1\" y=\"1\" z=\"1\"/>");
			line("\t\t<linearDamping>" + fmtNum(0.4f + 0.55f * w) + "</linearDamping>");
			line("\t\t<angularDamping>" + fmtNum(0.6f + 0.37f * w) + "</angularDamping>");
			line("\t\t<friction>0.5</friction>");
			line("\t\t<restitution>0.2</restitution>");
			line("\t\t<wind-factor>" + fmtNum(1.0f - 0.7f * w) + "</wind-factor>");
			line("\t</bone-default>");
		}
		for (auto& r : results) for (auto& b : r.bones) {
			if (!b.hasBox) { line("\t<bone name=\"" + xmlEscape(b.name) + "\" template=\"" + b.role + "\"/>"); continue; }
			// The body centred on its rigid pieces (their weight where they are), with a box the size of their body:
			// across, their mean distance from the upright axis through their centre (a broken bottle's jagged rim
			// stays out, so pieces modelled touching do not start in contact); upright, 90% of their height. A
			// collision-filter makes the box collide with the other ends' boxes (rigid contacts: they knock).
			Vec3 c{};
			for (auto& p : b.piecePts) c = c + p;
			c = c * (1.0f / b.piecePts.size());
			float rs = 0;
			for (auto& p : b.piecePts) rs += std::sqrt((p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y));
			float half = rs / b.piecePts.size(), halfZ = 0.45f * (b.boxMax.z - b.boxMin.z);
			c.z = (b.boxMin.z + b.boxMax.z) * 0.5f;
			Vec3 off = c - b.bind;
			char buf[512];
			line("\t<bone name=\"" + xmlEscape(b.name) + "\" template=\"" + b.role + "\">");
			snprintf(buf, sizeof buf, "\t\t<centerOfMassTransform><origin x=\"%.3f\" y=\"%.3f\" z=\"%.3f\"/></centerOfMassTransform>", off.x, off.y, off.z);
			line(buf);
			snprintf(buf, sizeof buf, "\t\t<shape type=\"box\"><halfExtend x=\"%.3f\" y=\"%.3f\" z=\"%.3f\"/><margin>0.05</margin></shape>", half, half, halfZ);
			line(buf);
			line("\t\t<collision-filter>1</collision-filter>");
			line("\t</bone>");
		}
		line("");
		line("\t<!-- Cloth warp: each bone hangs from the one above at a fixed distance, swinging within limits; soft angular");
		line("\t     springs bring it back toward its rest shape. Weft: soft springs between neighbouring columns. -->");
		line("\t<generic-constraint-default name=\"warpTop\">");
		line("\t\t<frameInB/>");
		line("\t\t<linearLowerLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line("\t\t<linearUpperLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line(limits(0.6f, 0.5f, 0.15f));
		line(spring(3.0f));
		line("\t\t<angularDamping x=\"0.15\" y=\"0.15\" z=\"0.15\"/>");
		line("\t</generic-constraint-default>");
		line("\t<generic-constraint-default name=\"warp\">");
		line("\t\t<frameInB/>");
		line("\t\t<linearLowerLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line("\t\t<linearUpperLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line(limits(0.35f, 0.3f, 0.1f));
		line(spring(2.0f));
		line("\t\t<angularDamping x=\"0.1\" y=\"0.1\" z=\"0.1\"/>");
		line("\t</generic-constraint-default>");
		line("\t<generic-constraint-default name=\"weft\">");
		line("\t\t<frameInA/>");
		line("\t\t<linearLowerLimit x=\"-1.5\" y=\"-1.5\" z=\"-1.5\"/>");
		line("\t\t<linearUpperLimit x=\"1.5\" y=\"1.5\" z=\"1.5\"/>");
		line("\t\t<angularLowerLimit x=\"-0.5\" y=\"-0.5\" z=\"-0.5\"/>");
		line("\t\t<angularUpperLimit x=\"0.5\" y=\"0.5\" z=\"0.5\"/>");
		line("\t\t<linearStiffness x=\"5\" y=\"5\" z=\"5\"/>");
		line("\t\t<linearDamping x=\"0.2\" y=\"0.2\" z=\"0.2\"/>");
		line("\t\t<linearNonHookeanStiffness x=\"0.8\" y=\"0.8\" z=\"0.8\"/>");
		line("\t\t<linearNonHookeanDamping x=\"0.8\" y=\"0.8\" z=\"0.8\"/>");
		line("\t</generic-constraint-default>");
		line("\t<!-- Rope links: a fixed length, free to swing wide, barely springy; no twist to speak of. -->");
		line("\t<generic-constraint-default name=\"link\">");
		line("\t\t<frameInB/>");
		line("\t\t<linearLowerLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line("\t\t<linearUpperLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line("\t\t<angularLowerLimit x=\"-0.9\" y=\"-0.9\" z=\"-0.2\"/>");
		line("\t\t<angularUpperLimit x=\"0.9\" y=\"0.9\" z=\"0.2\"/>");
		line("\t\t<angularStiffness x=\"0.6\" y=\"0.6\" z=\"0.6\"/>");
		line("\t\t<angularDamping x=\"0.08\" y=\"0.08\" z=\"0.08\"/>");
		line("\t</generic-constraint-default>");
		for (auto& [g, feel] : paintFeel) {
			const float s = feel.stiffness;
			line("\t<generic-constraint-default name=\"paintLink" + std::to_string(g) + "\">");
			line("\t\t<frameInB/>");
			line("\t\t<linearLowerLimit x=\"0\" y=\"0\" z=\"0\"/>");
			line("\t\t<linearUpperLimit x=\"0\" y=\"0\" z=\"0\"/>");
			const std::string sw = fmtNum(1.2f - 1.0f * s), tw = fmtNum(0.3f - 0.25f * s);
			line("\t\t<angularLowerLimit x=\"-" + sw + "\" y=\"-" + sw + "\" z=\"-" + tw + "\"/>");
			line("\t\t<angularUpperLimit x=\"" + sw + "\" y=\"" + sw + "\" z=\"" + tw + "\"/>");
			const std::string k = fmtNum(0.3f + 12.0f * s * s);
			line("\t\t<angularStiffness x=\"" + k + "\" y=\"" + k + "\" z=\"" + k + "\"/>");
			const std::string d = fmtNum(0.05f + 0.25f * s);
			line("\t\t<angularDamping x=\"" + d + "\" y=\"" + d + "\" z=\"" + d + "\"/>");
			line("\t</generic-constraint-default>");
		}
		for (size_t ri = 0; ri < results.size(); ri++) {
			const JobResult& r = results[ri];
			bool cloth = jobs[ri].kind == Job::Cloth;
			for (size_t ci = 0; ci < r.warp.size(); ci++) {
				auto& chain = r.warp[ci];
				int paint = ci < r.warpPaint.size() ? r.warpPaint[ci] : -1;
				line("\t<constraint-group>");
				for (size_t k = 1; k < chain.size(); k++)
					line("\t\t<generic-constraint bodyA=\"" + xmlEscape(chain[k]) + "\" bodyB=\"" + xmlEscape(chain[k - 1]) + "\" template=\"" +
						(paint >= 0 ? "paintLink" + std::to_string(paint) : cloth ? (k == 1 ? "warpTop" : "warp") : "link") + "\"/>");
				line("\t</constraint-group>");
			}
			if (weft && !r.weft.empty()) {
				line("\t<constraint-group>");
				for (auto& w : r.weft) line("\t\t<generic-constraint bodyA=\"" + xmlEscape(w.first) + "\" bodyB=\"" + xmlEscape(w.second) + "\" template=\"weft\"/>");
				line("\t</constraint-group>");
			}
		}
		line("");
		line("\t<!-- Collision: the dangling shapes (vertices) against the body (triangles). The anchors and the first links sit");
		line("\t     on the body and do not collide (threshold 1), or the contacts would shove them out into a lump. Rigid");
		line("\t     pieces on rope ends knock against each other through their bones' boxes (above), not here. -->");
		for (auto& r : results) {
			line("\t<per-vertex-shape name=\"" + xmlEscape(r.shape) + "\">");
			line("\t\t<margin>" + fmtNum(clothMargin) + "</margin>");
			line("\t\t<tag>cloth</tag>");
			line("\t\t<no-collide-with-tag>cloth</no-collide-with-tag>");
			for (auto& b : r.topBones) line("\t\t<weight-threshold bone=\"" + xmlEscape(b) + "\">1</weight-threshold>");
			// Vertices weighted only to the body's bones (static: kept, collar, knot) move with the body and cannot be
			// pushed: testing them against it only costs time (measured: about 40% of the Sunset's physics).
			for (auto& b : r.kinematic) line("\t\t<weight-threshold bone=\"" + xmlEscape(b) + "\">1</weight-threshold>");
			line("\t</per-vertex-shape>");
		}
		for (auto& c : colliderShapes) {
			line("\t<per-triangle-shape name=\"" + xmlEscape(c) + "\">");
			line("\t\t<margin>" + fmtNum(bodyMargin) + "</margin>");
			line("\t\t<penetration>" + fmtNum(penetration) + "</penetration>");
			line("\t\t<tag>body</tag>");
			line("\t\t<no-collide-with-tag>body</no-collide-with-tag>");
			for (auto& b : collideSkip) line("\t\t<weight-threshold bone=\"" + xmlEscape(b) + "\">1</weight-threshold>");
			line("\t</per-triangle-shape>");
		}
		line("</system>");
		std::ofstream xo(xmlOut, std::ios::binary);
		if (!xo) throw std::runtime_error("cannot write " + xmlOut);
		xo << x;
		size_t nb = 0;
		for (auto& r : results) nb += r.bones.size();
		printf("\nwrote %s and %s (%zu physics bones, %zu collision shapes)\n", argv[2], xmlOut.c_str(), nb, colliders.size());
	}
	catch (std::exception& e) {
		printf("error: %s\n", e.what());
		return 2;
	}
	return 0;
}
