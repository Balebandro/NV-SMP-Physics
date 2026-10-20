// smpify: makes a skinned coat or skirt an SMP (HDT-SMP / FSMP) physics item for NV SMP Physics.
//
//   smpify in.nif out.nif "Coat Shape" "Data\relative\path.xml" out.xml [options]
//     --collide "Shape"       a shape of the same NIF the coat collides with (repeatable; e.g. "Legs")
//     --virtual-legs "Shape"  collide with smooth leg capsules fitted to that shape instead (e.g. "Legs")
//     --grid cols rows        cloth bone grid (default 11 x 5)
//     --top z                 skin-space height of the waist row (default 66)
//     --angle degrees         grid spans -angle..+angle around the body, 0 = back (default 140: front open)
//
// What it does (the grid, the reweighting and the partitioning are NV Cloth Physics' clothify):
//   1. Lays a grid of cloth bones over the skirt in skin space: columns around the body, rows from
//      the waist down to the hem. The waist row are fixed anchors carried by the pelvis; the rows
//      below are simulated.
//   2. Reweights the skirt onto the cloth bones (bilinear over the grid; the waist row keeps the
//      vertex's original weights, so the seam does not move), rebuilds NiSkinData and
//      NiSkinPartition (16 bones per partition: the skinning shaders hold 18).
//   3. Writes, for the plugin:
//        "SMP Bones" on the shape: every skin slot's bone and its parent ("name|parent;..."). New
//          Vegas drops an item's own bone nodes when it attaches it to an actor, so the plugin
//          creates them from this list;
//        "HDT Skinned Mesh Physics Object" on the root and the shape: the physics file, as SMP has it;
//        the physics XML: bones, chained columns (warp) and soft springs across them (weft), in the
//          style of FSMP's cloak pattern, and the collision shapes.
#include "nif.h"
#include <algorithm>
#include <bitset>
#include <cstdio>
#include <map>
#include <set>
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

// A string extra data block, added to an object's extra data list.
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

int main(int argc, char** argv)
{
	if (argc < 6) {
		printf("usage: smpify in.nif out.nif \"Coat Shape\" \"Data\\relative\\physics.xml\" out.xml [--collide \"Shape\"]... [--virtual-legs \"Shape\"] [--grid cols rows] [--top z] [--angle deg]\n");
		return 1;
	}
	int cols = 11, rows = 5;
	float zTop = 66.0f, angleMax = 140.0f;
	std::vector<std::string> colliders;
	std::string virtualLegs;   // --virtual-legs "Shape": smooth leg capsules fitted to that shape, not its triangles
	for (int i = 6; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--collide" && i + 1 < argc) colliders.push_back(argv[++i]);
		else if (a == "--virtual-legs" && i + 1 < argc) virtualLegs = argv[++i];
		else if (a == "--grid" && i + 2 < argc) { cols = atoi(argv[++i]); rows = atoi(argv[++i]); }
		else if (a == "--top" && i + 1 < argc) zTop = (float)atof(argv[++i]);
		else if (a == "--angle" && i + 1 < argc) angleMax = (float)atof(argv[++i]);
		else { printf("unknown option %s\n", a.c_str()); return 1; }
	}
	const std::string shapeName = argv[3], physicsPath = argv[4], xmlOut = argv[5];
	try {
		File f;
		f.load(argv[1]);
		int shapeBlock = findBlockByName(f, shapeName);
		if (shapeBlock < 0) throw std::runtime_error("shape not found: " + shapeName);
		Geometry geom;
		geom.read(f.blocks[shapeBlock]);
		bool strips = f.typeOf(geom.data) == "NiTriStripsData";
		GeometryData gd;
		gd.read(f.blocks[geom.data], strips);
		bool dism = f.typeOf(geom.skin) == "BSDismemberSkinInstance";
		SkinInstance si;
		si.read(f.blocks[geom.skin], dism);
		SkinData sd;
		sd.read(f.blocks[si.data]);
		SkinPartition sp;
		sp.read(f.blocks[si.partition]);
		size_t nv = gd.vertices.size();
		printf("shape %d '%s': %zu verts, %zu tris, %zu bones, %zu partitions\n", shapeBlock, shapeName.c_str(), nv, gd.triangles.size(), si.bones.size(), sp.parts.size());
		for (size_t b = 0; b < si.bones.size(); b++)
			if (strName(f, si.bones[b]).rfind("SMP ", 0) == 0) throw std::runtime_error("shape already has SMP bones");

		// Skin-space positions and original weights.
		std::vector<Vec3> pos(nv);
		for (size_t v = 0; v < nv; v++) pos[v] = sd.skinTransform.apply(gd.vertices[v]);
		std::vector<Weights> orig(nv);
		for (size_t b = 0; b < sd.bones.size(); b++)
			for (auto& vw : sd.bones[b].weights) orig[vw.index].push_back({ (int)b, vw.weight });
		for (auto& w : orig) normalizeTop4(w);

		std::vector<std::string> boneNames;
		for (auto b : si.bones) boneNames.push_back(strName(f, b));
		const int bodyBoneCount = (int)boneNames.size();
		auto skinBone = [&](const std::string& n) { for (size_t i = 0; i < boneNames.size(); i++) if (boneNames[i] == n) return (int)i; return -1; };
		int pelvis = skinBone("Bip01 Pelvis");
		if (pelvis < 0) throw std::runtime_error("shape is not skinned to Bip01 Pelvis");

		// Skirt axis and radius per height.
		float zMin = 1e9f;
		double cx = 0, cy = 0;
		int n = 0;
		for (auto& p : pos) if (p.z < zTop) { zMin = std::min(zMin, p.z); cx += p.x; cy += p.y; n++; }
		if (!n) throw std::runtime_error("no vertices below the waist height");
		cx /= n; cy /= n;
		auto angleOf = [&](Vec3 p) { return std::atan2(p.x - (float)cx, -(p.y - (float)cy)) * 180.0f / PI; };   // 0 = back
		float zBottom = zMin, dz = (zTop - zBottom) / (rows - 1), da = 2 * angleMax / (cols - 1);
		std::vector<float> rowRadius(rows, 0);
		for (int r = 0; r < rows; r++) {
			float z = zTop - r * dz;
			double s = 0;
			int k = 0;
			for (auto& p : pos) if (std::fabs(p.z - z) < dz * 0.5f) { s += std::sqrt((p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy)); k++; }
			rowRadius[r] = k ? (float)(s / k) : 14.0f;
		}
		printf("skirt axis (%.2f, %.2f), z %.1f..%.1f, grid %dx%d, %.1f deg x %.1f units\n", cx, cy, zBottom, zTop, cols, rows, da, dz);

		// The grid: one bone per point. Bind rotation identity (the skin's frame), at the point.
		struct Point { int c, r; Vec3 bind; int bone = -1; std::string name, parent; };
		std::vector<Point> grid;
		auto pid = [&](int c, int r) { return r * cols + c; };
		std::vector<int> newNodeBlocks;
		for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) {
			Point p;
			p.c = c; p.r = r;
			float a = (-angleMax + c * da) * PI / 180.0f, z = zTop - r * dz;
			p.bind = { (float)cx + rowRadius[r] * std::sin(a), (float)cy - rowRadius[r] * std::cos(a), z };
			char nm[64];
			snprintf(nm, sizeof nm, "SMP Coat C%02d R%d", c, r);
			p.name = nm;
			if (r == 0) p.parent = "Bip01 Pelvis";
			else { char pn[64]; snprintf(pn, sizeof pn, "SMP Coat C%02d R%d", c, r - 1); p.parent = pn; }
			AVObject node;
			node.isNode = true;
			node.name = f.addString(nm);
			node.flags = 0xE;
			node.local.trans = p.bind;
			Writer w;
			node.write(w);
			int blk = f.addBlock("NiNode", w.b);
			newNodeBlocks.push_back(blk);
			p.bone = (int)si.bones.size();
			si.bones.push_back(blk);
			SkinData::Bone bd;
			bd.skinToBone.trans = p.bind * -1.0f;
			sd.bones.push_back(bd);
			boneNames.push_back(nm);
			grid.push_back(p);
		}
		// The bone nodes go under the root so the file stays a valid tree for NifSkope. The game drops
		// them on attach; the plugin builds them back from "SMP Bones".
		AVObject root;
		Reader rr(f.blocks[f.roots[0]].data(), f.blocks[f.roots[0]].size());
		root.read(rr, true);
		Bytes rootTail(f.blocks[f.roots[0]].begin() + root.tailPos, f.blocks[f.roots[0]].end());
		for (int b : newNodeBlocks) root.children.push_back(b);

		// Reweight the skirt: bilinear over the grid; the waist row hands over to the original weights.
		std::vector<Weights> neww = orig;
		int changed = 0;
		for (size_t v = 0; v < nv; v++) {
			if (pos[v].z >= zTop) continue;
			float u = (angleOf(pos[v]) + angleMax) / da, w = (zTop - pos[v].z) / dz;
			u = std::min(std::max(u, 0.0f), (float)(cols - 1));
			w = std::min(std::max(w, 0.0f), (float)(rows - 1));
			int c0 = std::min((int)u, cols - 2), r0 = std::min((int)w, rows - 2);
			float fu = u - c0, fw = w - r0;
			Weights out;
			auto add = [&](int c, int r, float k) {
				if (k <= 0) return;
				if (r == 0) { for (auto& i : orig[v]) out.push_back({ i.bone, i.w * k }); }
				else out.push_back({ grid[pid(c, r)].bone, k });
			};
			add(c0, r0, (1 - fu) * (1 - fw));
			add(c0 + 1, r0, fu * (1 - fw));
			add(c0, r0 + 1, (1 - fu) * fw);
			add(c0 + 1, r0 + 1, fu * fw);
			normalizeTop4(out);
			neww[v] = out;
			changed++;
		}
		printf("reweighted %d skirt vertices onto %d cloth bones\n", changed, cols * (rows - 1));

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

		// NiSkinPartition: greedy, each triangle's bones in one partition, growing by the triangle that
		// adds the fewest new bones, so partitions follow regions of the skirt.
		typedef std::vector<uint64_t> Mask;
		size_t words = (sd.bones.size() + 63) / 64;
		auto bits = [](const Mask& m) { int k = 0; for (auto w : m) k += (int)std::bitset<64>(w).count(); return k; };
		const std::vector<Triangle>& tris = gd.triangles;
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
			for (size_t b = 0; b < sd.bones.size(); b++) if (cur[b / 64] & (1ull << (b % 64))) bones.insert((int)b);
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
		printf("partitions: %zu (", np.parts.size());
		for (auto& p : np.parts) printf(" %zuv/%zut/%zub", p.vertexMap.size(), p.triangles.size(), p.bones.size());
		printf(" )\n");
		if (dism) {
			SkinInstance::Part proto = si.parts.empty() ? SkinInstance::Part{ 257, 0 } : si.parts[0];
			si.parts.assign(np.parts.size(), proto);
		}

		// "SMP Bones": every skin slot's bone and parent, in skin-instance order.
		std::string smpBones;
		for (size_t b = 0; b < boneNames.size(); b++) {
			std::string parent;
			if ((int)b >= bodyBoneCount) parent = grid[b - bodyBoneCount].parent;
			smpBones += boneNames[b] + "|" + parent + ";";
		}
		addSmpBones(f, geom.av, smpBones);
		addStringExtra(f, geom.av, "HDT Skinned Mesh Physics Object", physicsPath);
		addStringExtra(f, root, "HDT Skinned Mesh Physics Object", physicsPath);

		{
			Writer w;
			root.write(w);
			w.raw(rootTail.data(), rootTail.size());
			f.blocks[f.roots[0]] = w.b;
		}

		// The plugin reads the physics shapes' vertices and weights from memory: ask the game to keep
		// them after it uploads the mesh (NiGeometryData keep flags KEEP_XYZ | KEEP_BONEDATA, the byte
		// after the group id and the vertex count).
		auto keepData = [&](int shape) {
			Geometry g;
			g.read(f.blocks[shape]);
			if (g.data >= 0 && f.blocks[g.data].size() > 6) f.blocks[g.data][6] |= 0x01 | 0x20;
		};
		keepData(shapeBlock);
		// The shapes the physics reads: the colliders, and the one the virtual legs are fitted to.
		std::vector<std::string> bodyShapes = colliders;
		if (!virtualLegs.empty()) bodyShapes.push_back(virtualLegs);
		for (auto& c : bodyShapes) {
			int blk = findBlockByName(f, c);
			if (blk >= 0) keepData(blk);
		}
		f.blocks[shapeBlock] = geom.write();
		f.blocks[geom.skin] = si.write();
		f.blocks[si.data] = sd.write();
		f.blocks[si.partition] = np.write();
		f.save(argv[2]);

		// The physics XML.
		std::set<std::string> kinematic(boneNames.begin(), boneNames.begin() + bodyBoneCount);
		for (auto& c : bodyShapes) {
			int blk = findBlockByName(f, c);
			if (blk < 0) throw std::runtime_error("collision shape not found: " + c);
			Geometry og;
			og.read(f.blocks[blk]);
			if (og.skin < 0) throw std::runtime_error("collision shape has no skin: " + c);
			SkinInstance os;
			os.read(f.blocks[og.skin], f.typeOf(og.skin) == "BSDismemberSkinInstance");
			for (auto b : os.bones) kinematic.insert(strName(f, b));
		}

		std::string x;
		auto line = [&](const std::string& s) { x += s; x += "\n"; };
		char buf[512];
		line("<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
		line("<!-- Generated by smpify (NV SMP Physics) for " + xmlEscape(shapeName) + ": a " + std::to_string(cols) + " x " + std::to_string(rows) +
			" grid of cloth bones over the skirt, in the style of FSMP's cloak pattern. -->");
		line("<system>");
		line("");
		line("\t<!-- Body bones: kinematic (no template, mass 0), animation driven. The collision shapes are skinned to them. -->");
		for (auto& b : kinematic) line("\t<bone name=\"" + xmlEscape(b) + "\"/>");
		line("");
		line("\t<!-- The waist row: fixed anchors on the pelvis. The rows below: simulated. -->");
		line("\t<bone-default name=\"anchor\">");
		line("\t\t<restitution>1</restitution>");
		line("\t</bone-default>");
		line("\t<bone-default name=\"cloth\">");
		line("\t\t<mass>0.02</mass>");
		line("\t\t<inertia x=\"1\" y=\"1\" z=\"1\"/>");
		line("\t\t<linearDamping>0.6</linearDamping>");
		line("\t\t<angularDamping>0.8</angularDamping>");
		line("\t\t<friction>0.9</friction>");
		line("\t\t<restitution>0.2</restitution>");
		line("\t\t<wind-factor>1</wind-factor>");
		line("\t</bone-default>");
		for (auto& p : grid) line("\t<bone name=\"" + p.name + "\" template=\"" + (p.r == 0 ? "anchor" : "cloth") + "\"/>");
		line("");
		line("\t<!-- Warp: each bone hangs from the one above it at a fixed distance, swinging within limits;");
		line("\t     soft angular springs bring it back toward its rest shape. The first link swings the most. -->");
		line("\t<generic-constraint-default name=\"warpTop\">");
		line("\t\t<frameInB/>");
		line("\t\t<linearLowerLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line("\t\t<linearUpperLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line("\t\t<angularLowerLimit x=\"-0.6\" y=\"-0.5\" z=\"-0.15\"/>");
		line("\t\t<angularUpperLimit x=\"0.6\" y=\"0.5\" z=\"0.15\"/>");
		line("\t\t<angularStiffness x=\"3\" y=\"3\" z=\"3\"/>");
		line("\t\t<angularDamping x=\"0.15\" y=\"0.15\" z=\"0.15\"/>");
		line("\t</generic-constraint-default>");
		line("\t<generic-constraint-default name=\"warp\">");
		line("\t\t<frameInB/>");
		line("\t\t<linearLowerLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line("\t\t<linearUpperLimit x=\"0\" y=\"0\" z=\"0\"/>");
		line("\t\t<angularLowerLimit x=\"-0.35\" y=\"-0.3\" z=\"-0.1\"/>");
		line("\t\t<angularUpperLimit x=\"0.35\" y=\"0.3\" z=\"0.1\"/>");
		line("\t\t<angularStiffness x=\"2\" y=\"2\" z=\"2\"/>");
		line("\t\t<angularDamping x=\"0.1\" y=\"0.1\" z=\"0.1\"/>");
		line("\t</generic-constraint-default>");
		line("\t<!-- Weft: neighbouring columns at the same row, a soft spring that loosens as it stretches");
		line("\t     (non-Hookean), with room to separate and rotate: lateral cohesion, not a rigid sheet. -->");
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
		for (int c = 0; c < cols; c++) {
			line("\t<constraint-group>");
			for (int r = 1; r < rows; r++)
				line("\t\t<generic-constraint bodyA=\"" + grid[pid(c, r)].name + "\" bodyB=\"" + grid[pid(c, r - 1)].name + "\" template=\"" + (r == 1 ? "warpTop" : "warp") + "\"/>");
			line("\t</constraint-group>");
		}
		for (int c = 0; c + 1 < cols; c++) {
			line("\t<constraint-group>");
			for (int r = 1; r < rows; r++)
				line("\t\t<generic-constraint bodyA=\"" + grid[pid(c, r)].name + "\" bodyB=\"" + grid[pid(c + 1, r)].name + "\" template=\"weft\"/>");
			line("\t</constraint-group>");
		}
		line("");
		line("\t<!-- Collision: the coat (vertices) against what is under it (triangles). -->");
		// SMP's usual arrangement: what is underneath as triangles (outward normals: a vertex up to
		// `penetration` behind the surface is inside it and is pushed out), the cloth as vertices.
		// The other way round, the legs sit behind the coat's own surface and push it outward all the time.
		line("\t<per-vertex-shape name=\"" + xmlEscape(shapeName) + "\">");
		line("\t\t<margin>1</margin>");
		line("\t\t<tag>cloth</tag>");
		line("\t\t<no-collide-with-tag>cloth</no-collide-with-tag>");
		// The top rows sit tight over the hips. Against the leg mesh, posed, the body is always within the margins
		// there and the contacts shove those bones out into a lump: only the hanging part of the coat collides.
		// Against the slimmer, smooth virtual legs, the vertices mostly on those rows collide too (threshold 0.5):
		// measured on the game's run, that ends the coat clipping into the thighs at the hips, and sneaking stays
		// as calm as before (fully on, the crouched thighs fight the waist anchors).
		const char* topThreshold = virtualLegs.empty() ? "1" : "0.5";
		for (auto& p : grid)
			if (p.r < 2) line("\t\t<weight-threshold bone=\"" + p.name + "\">" + topThreshold + "</weight-threshold>");
		line("\t</per-vertex-shape>");
		if (!virtualLegs.empty()) {
			line("\t<!-- The legs as smooth tapered capsules fitted to " + xmlEscape(virtualLegs) + " (the plugin builds them). -->");
			line("\t<nv-virtual-legs fit=\"" + xmlEscape(virtualLegs) + "\">");
			line("\t\t<margin>0.5</margin>");
			line("\t\t<penetration>3</penetration>");
			line("\t\t<tag>body</tag>");
			line("\t\t<no-collide-with-tag>body</no-collide-with-tag>");
			line("\t</nv-virtual-legs>");
		}
		for (auto& c : colliders) {
			line("\t<per-triangle-shape name=\"" + xmlEscape(c) + "\">");
			line("\t\t<margin>0.5</margin>");
			line("\t\t<penetration>3</penetration>");
			line("\t\t<tag>body</tag>");
			line("\t\t<no-collide-with-tag>body</no-collide-with-tag>");
			line("\t</per-triangle-shape>");
		}
		line("</system>");
		std::ofstream xo(xmlOut, std::ios::binary);
		if (!xo) throw std::runtime_error("cannot write " + xmlOut);
		xo << x;
		snprintf(buf, sizeof buf, "wrote %s and %s (%d cloth bones, %zu collision shapes)\n", argv[2], xmlOut.c_str(), cols * rows, colliders.size() + 1 + (virtualLegs.empty() ? 0 : 1));
		printf("%s", buf);
	} catch (std::exception& e) {
		printf("error: %s\n", e.what());
		return 2;
	}
	return 0;
}
