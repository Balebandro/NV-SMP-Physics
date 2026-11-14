#pragma once
// The previewer's "Make SMP" window: smpdress, smpify and smprepair with a form instead of a command line. It reads the
// shapes and bones of the NIF open in the previewer, runs the tool (beside the previewer's exe, no console window) and
// opens what it made. Included by previewer.cpp after its helpers (Help, PickFile, PickFolder, LoadItem, AddLog, gItem).
//
// The output goes into a mod folder laid out as Data: <output folder>\<physics file's folder>\<the NIF's file name> and
// <output folder>\<physics file>, so the folder can be an MO2 mod. Files it would replace are kept as *.bak. Each build
// writes its settings as a recipe (<xml>.smprecipe) beside the XML; "Load recipe..." brings them back.
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <deque>
#include <functional>
#include <thread>
#include <tuple>

#include "backup.h"

namespace maker
{
	enum Mode { Outfit, Coat, Repair };
	enum Role { None, Cloth, Chains, Collider, VirtualLegs };   // Cloth is smpify's coat in Coat mode
	static const char* kOutfitRoles[] = { "Leave as is", "Cloth", "Chains", "Collider" };

	static std::string Lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
	static std::string Num(float v) { char b[32]; snprintf(b, sizeof b, "%g", v); return b; }
	static void Copy(char* dst, size_t n, const std::string& s) { strncpy_s(dst, n, s.c_str(), _TRUNCATE); }
	static std::string ToolDir() { char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH); return fs::path(exe).parent_path().string(); }

	struct KeepRow { std::string bone; float w0 = 0.05f, w1 = 0.2f; };

	struct ShapeRow
	{
		std::string name;
		int verts = 0;
		float zMin = 0, zMax = 0;
		std::vector<std::string> skinBones;
		int role = None;
		// --cloth
		std::string anchor = "Bip01 Pelvis";
		int cols = 9, rows = 5;
		bool autoTop = true;
		float top = 0;
		bool autoAngles = true;
		float angleLo = -140, angleHi = 140;
		bool radial = false;
		float collar = 10;
		int fixedRows = 1;
		std::vector<std::pair<std::string, std::string>> detach;
		std::vector<KeepRow> keep;
		// --chains
		int links = 4;
		bool keepTop = false;
		float keepTopA = 1, keepTopB = 2;
	};

	// A chain painted with the brush: on one shape, its stroke (vertex indices from where it is tied on) and the painted
	// vertices with their strength; built by smpdress --painted.
	struct PaintChain
	{
		std::string shape;
		std::vector<int> path;
		std::map<int, float> paint;
		std::map<int, V3> points;           // points moved off the stroke (Move points), by index: their place in the skin's space (smpdress's)
		std::string anchor;
		int links = 4;
		float stiffness = 0.4f, weight = 0.4f;
		bool hidden = false;                // (2026-11-14) its square in the list clicked: lines, points and paint not drawn (view only, not saved)
	};

	struct State
	{
		bool open = false;
		// painting
		bool paintTab = true, paintOn = true;
		bool nifPose = true;                // painting shows the NIF's own pose, not the animation's frame
		int tool = 0;                       // 0 new chain, 1 add to the selected one, 2 erase from it, 3 smooth it, 4 mask, 5 unmask
		std::map<std::string, std::set<int>> masks;   // per shape, the vertices no brush, smoothing or grow / fade changes
		std::string maskShape;              // the shape a mask stroke is on
		float radius = 6, strength = 1;
		std::vector<PaintChain> chains;
		int selected = -1;
		int point = -1;                     // the selected chain's selected point (0 where it is tied on), -1 none
		float reach = 8, amount = 0.25f;    // grow / fade around the point: how far, how much per click
		V3 dragPlane, dragNormal;           // Move points: the plane the dragged point moves in (facing the camera)
		// (2026-11-14 user: move several points at once) Move points: the points picked together (chain, point; Ctrl+click
		// adds or takes one out), dragged as one; where each was when the drag began (world), and the grabbed one's.
		std::vector<std::pair<int, int>> group;
		std::vector<V3> groupStart;
		struct Snapshot { std::vector<PaintChain> chains; int selected, point; std::map<std::string, std::set<int>> masks; };
		std::deque<Snapshot> undo;          // the chains before each edit, newest last (Ctrl+Z), at most kUndo
		std::deque<Snapshot> redo;          // what Ctrl+Z took back, newest last (Ctrl+Y); a new edit clears it
		bool stroking = false;
		bool hit = false;
		V3 hitPoint, hitNormal;
		int hitShape = -1;
		int mode = Outfit;
		std::string source;                 // the NIF the shapes were read from
		bool sourceHasPhysics = false;
		std::vector<ShapeRow> shapes;
		std::vector<std::string> skeleton;  // anchors to pick from
		char physics[512] = "";
		char outDir[512] = "";
		// collision
		bool proxy = true;
		float proxyCell = 3;
		std::set<std::string> collideSkip;
		float clothMargin = 1, bodyMargin = 0.5f, penetration = 3;
		// cloth feel
		float bend = 1, stiffness = 1, friction = 0.9f, mass = 0.02f, restitution = 0.2f, dampL = 0.6f, dampA = 0.8f;
		bool weft = true;
		// smpify
		int coatCols = 11, coatRows = 5;
		float coatTop = 66, coatAngle = 140;
		// smprepair
		char edited[512] = "", reference[512] = "", repaired[512] = "";
		char listNif[512] = "", listSkeleton[512] = "";   // the missing SMP Bones list (smprepair --list)
		// the tool running
		std::thread worker;
		std::mutex lock;
		std::vector<std::string> pending;
		std::atomic<bool> running{ false }, finished{ false };
		int exitCode = 0;
		std::string resultNif, recipeToSave, lastCommand, lastResult;
		std::function<void()> afterSuccess;   // runs once the tool succeeded, before the result is opened
		bool buildOnOpen = false;           // --maker-build (testing): build once the window has a source
		// (2026-11-14 user: a rebuild lost the proxy and shape edits made on the output.) Edits made in the previewer on the
		// build's output ("kind|arg|..."; RecordEdit), kept in the recipe and done again, in order, after every build.
		std::vector<std::string> edits;
		std::deque<std::string> replay;     // the edits still to redo after this build
		bool replaying = false, replayAfterBuild = false;
		bool valuesAfterBuild = false;      // once the kept edits are done again: the kept Bone values put in (ApplyKeptValues)
	};
	static State gM;

	// Undo: a copy of the chains before each edit (a brush stroke, smoothing, grow / fade, deleting, loading a recipe).
	// Redo: what an undo took back; a new edit clears it.
	constexpr size_t kUndo = 50;
	static State::Snapshot Current() { return { gM.chains, gM.selected, gM.point, gM.masks }; }
	static void Restore(State::Snapshot& s)
	{
		gM.chains = std::move(s.chains);
		gM.selected = s.selected < (int)gM.chains.size() ? s.selected : -1;
		gM.point = s.point;
		gM.masks = std::move(s.masks);
	}
	static void PushUndo()
	{
		gM.undo.push_back(Current());
		if (gM.undo.size() > kUndo) gM.undo.pop_front();
		gM.redo.clear();
	}
	static void Undo()
	{
		if (gM.undo.empty()) { AddLog(3, "nothing to undo"); return; }
		gM.redo.push_back(Current());
		Restore(gM.undo.back());
		gM.undo.pop_back();
		AddLog(2, "undone (" + std::to_string(gM.undo.size()) + " more to undo, " + std::to_string(gM.redo.size()) + " to redo)");
	}
	static void Redo()
	{
		if (gM.redo.empty()) { AddLog(3, "nothing to redo"); return; }
		gM.undo.push_back(Current());
		if (gM.undo.size() > kUndo) gM.undo.pop_front();
		Restore(gM.redo.back());
		gM.redo.pop_back();
		AddLog(2, "redone (" + std::to_string(gM.redo.size()) + " more to redo)");
	}

	static const D3DCOLOR kChainColors[] = { D3DCOLOR_ARGB(255, 255, 120, 40), D3DCOLOR_ARGB(255, 60, 200, 255), D3DCOLOR_ARGB(255, 120, 230, 90),
		D3DCOLOR_ARGB(255, 240, 80, 200), D3DCOLOR_ARGB(255, 250, 220, 60), D3DCOLOR_ARGB(255, 150, 120, 255), D3DCOLOR_ARGB(255, 80, 240, 200), D3DCOLOR_ARGB(255, 255, 80, 80) };
	static D3DCOLOR ChainColor(size_t i) { return kChainColors[i % IM_ARRAYSIZE(kChainColors)]; }

	// The paint file smpdress --painted reads (one for every shape; each chain names its shape).
	static std::string PaintText()
	{
		std::ostringstream o;
		o << "; NV SMP Maker painted chains (smpdress --painted)\n";
		for (auto& c : gM.chains) {
			o << "chain " << c.shape << "\nanchor " << c.anchor << "\nlinks " << c.links << "\nstiffness " << Num(c.stiffness) << "\nweight " << Num(c.weight) << "\npath";
			for (int v : c.path) o << " " << v;
			o << "\npaint";
			for (auto& [v, s] : c.paint) o << " " << v << ":" << Num(s);
			for (auto& [k, p] : c.points) o << "\npoint " << k << " " << Num(p.x) << " " << Num(p.y) << " " << Num(p.z);
			o << "\nend\n";
		}
		return o.str();
	}

	static void ReadPaint(const std::string& path)
	{
		gM.chains.clear();
		gM.selected = -1;
		std::ifstream in(path);
		std::string line;
		PaintChain cur;
		bool inChain = false;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			size_t sp = line.find(' ');
			std::string key = line.substr(0, sp), rest = sp == std::string::npos ? "" : line.substr(sp + 1);
			if (key == "chain") { cur = PaintChain(); cur.shape = rest; inChain = true; }
			else if (!inChain) continue;
			else if (key == "end") { gM.chains.push_back(cur); inChain = false; }
			else if (key == "anchor") cur.anchor = rest;
			else if (key == "links") cur.links = atoi(rest.c_str());
			else if (key == "stiffness") cur.stiffness = (float)atof(rest.c_str());
			else if (key == "weight") cur.weight = (float)atof(rest.c_str());
			else if (key == "point") { std::istringstream s(rest); int k; V3 p; if (s >> k >> p.x >> p.y >> p.z) cur.points[k] = p; }
			else if (key == "path") { std::istringstream s(rest); int v; while (s >> v) cur.path.push_back(v); }
			else if (key == "paint") { std::istringstream s(rest); std::string t; while (s >> t) { size_t c = t.find(':'); if (c != std::string::npos) cur.paint[atoi(t.c_str())] = (float)atof(t.c_str() + c + 1); } }
		}
		if (!gM.chains.empty()) gM.selected = 0;
	}


	// The physics file's path relative to Data, from where the NIF sits ("...\Meshes\armor\x\coat.nif" ->
	// "Meshes\armor\x\coat_smp.xml"); a NIF outside a Meshes folder gets "Meshes\<name>_smp.xml".
	static std::string DefaultPhysicsPath(const std::string& nif)
	{
		fs::path p(nif);
		std::string xml = p.stem().string() + "_smp.xml";
		std::vector<std::string> parts;
		for (auto& c : p.parent_path()) parts.push_back(c.string());
		for (int i = (int)parts.size() - 1; i >= 0; i--) {
			if (Lower(parts[i]) != "meshes") continue;
			std::string r = "Meshes";
			for (size_t j = i + 1; j < parts.size(); j++) r += "\\" + parts[j];
			return r + "\\" + xml;
		}
		return "Meshes\\" + xml;
	}

	// Where the build writes: the NIF beside the physics file, both under the output folder as under Data.
	static std::string OutXml() { return (fs::path(gM.outDir) / gM.physics).string(); }
	static std::string OutNif() { return (fs::path(OutXml()).parent_path() / fs::path(gM.source).filename()).string(); }

	// The shapes and bones of the NIF open in the previewer.
	static void Capture()
	{
		std::vector<ShapeRow> old = std::move(gM.shapes);
		gM.shapes.clear();
		if (gItem.nifPath != gM.source) { gM.chains.clear(); gM.selected = -1; gM.point = -1; gM.masks.clear(); gM.undo.clear(); gM.redo.clear(); gM.edits.clear(); }   // another NIF: its chains, masks, history and kept edits go
		gM.source = gItem.nifPath;
		gM.sourceHasPhysics = !gItem.scene.physicsFile.empty();
		for (auto& s : gItem.shapes) {
			auto* g = s.info.geometry;
			ShapeRow r;
			r.name = g->m_name ? g->m_name : "";
			if (r.name.empty() || Lower(r.name).rfind("smp proxy:", 0) == 0) continue;
			auto* d = g->data;
			r.verts = d ? d->vertexCount : 0;
			r.zMin = 1e9f; r.zMax = -1e9f;
			for (int v = 0; d && d->vertices && v < d->vertexCount; v++) { r.zMin = std::min(r.zMin, d->vertices[v].z); r.zMax = std::max(r.zMax, d->vertices[v].z); }
			if (r.zMin > r.zMax) r.zMin = r.zMax = 0;
			r.top = r.zMin + (r.zMax - r.zMin) * 0.8f;
			std::set<std::string> bones;
			if (auto* skin = g->skinInstance)
				for (uint32_t b = 0; b < skin->boneCount; b++) if (const char* n = SkinBoneName(s, (int)b)) bones.insert(n);
			r.skinBones.assign(bones.begin(), bones.end());
			for (auto& o : old) if (o.name == r.name) { auto keepBones = r.skinBones; auto v = r.verts; auto z0 = r.zMin, z1 = r.zMax; r = o; r.skinBones = keepBones; r.verts = v; r.zMin = z0; r.zMax = z1; }
			gM.shapes.push_back(r);
		}
		gM.skeleton.clear();
		for (auto& [name, node] : gItem.scene.skeleton.bones) gM.skeleton.push_back(name);
		if (!gM.physics[0] || old.empty()) Copy(gM.physics, sizeof gM.physics, DefaultPhysicsPath(gM.source));
		if (!gM.outDir[0] || old.empty()) Copy(gM.outDir, sizeof gM.outDir, (fs::path(gM.source).parent_path() / "SMP output").string());
	}

	// --- recipes -------------------------------------------------------------------------------------------------

	static std::string RecipeText()
	{
		std::ostringstream o;
		o << "; NV SMP Previewer recipe: the Make SMP window's settings. Load it there with \"Load recipe...\".\n[recipe]\n";
		o << "mode=" << gM.mode << "\nsource=" << gM.source << "\nphysics=" << gM.physics << "\noutdir=" << gM.outDir << "\n";
		o << "proxy=" << gM.proxy << "\nproxycell=" << Num(gM.proxyCell) << "\n";
		for (auto& b : gM.collideSkip) o << "collideskip=" << b << "\n";
		o << "clothmargin=" << Num(gM.clothMargin) << "\nbodymargin=" << Num(gM.bodyMargin) << "\npenetration=" << Num(gM.penetration) << "\n";
		o << "bend=" << Num(gM.bend) << "\nstiffness=" << Num(gM.stiffness) << "\nweft=" << gM.weft << "\nfriction=" << Num(gM.friction) << "\n";
		o << "mass=" << Num(gM.mass) << "\nrestitution=" << Num(gM.restitution) << "\ndamping=" << Num(gM.dampL) << " " << Num(gM.dampA) << "\n";
		o << "coatgrid=" << gM.coatCols << " " << gM.coatRows << "\ncoattop=" << Num(gM.coatTop) << "\ncoatangle=" << Num(gM.coatAngle) << "\n";
		if (!gM.chains.empty()) o << "paint=" << OutXml() << ".smppaint\n";
		for (auto& e : gM.edits) o << "edit=" << e << "\n";   // done again on the output after every build
		for (auto& s : gM.shapes) {
			if (s.role == None) continue;
			o << "\n[shape]\nname=" << s.name << "\nrole=" << s.role << "\nanchor=" << s.anchor << "\ngrid=" << s.cols << " " << s.rows << "\n";
			o << "top=" << (s.autoTop ? "auto" : Num(s.top)) << "\nangles=" << (s.autoAngles ? "auto" : Num(s.angleLo) + " " + Num(s.angleHi)) << "\n";
			o << "radial=" << s.radial << "\ncollar=" << Num(s.collar) << "\nfixedrows=" << s.fixedRows << "\n";
			for (auto& [b, t] : s.detach) o << "detach=" << b << "=" << t << "\n";
			for (auto& k : s.keep) o << "keep=" << k.bone << "|" << Num(k.w0) << "|" << Num(k.w1) << "\n";
			o << "links=" << s.links << "\nkeeptop=" << (s.keepTop ? Num(s.keepTopA) + " " + Num(s.keepTopB) : std::string("off")) << "\n";
		}
		return o.str();
	}

	// The recipe a Maker build left beside its physics XML (<xml>.smprecipe), or empty.
	static std::string RecipeOf(const std::string& xml)
	{
		if (xml.empty()) return "";
		std::error_code ec;
		std::string r = xml + ".smprecipe";
		return fs::exists(r, ec) ? r : "";
	}

	// Loads a recipe: opens its source NIF in the previewer and puts the settings back on the shapes of the same names.
	static bool LoadRecipe(const std::string& path)
	{
		std::ifstream in(path);
		if (!in) { AddLog(4, "cannot read the recipe " + path); return false; }
		std::vector<std::pair<std::string, std::string>> top;
		std::vector<std::vector<std::pair<std::string, std::string>>> shapes;
		std::string line;
		bool inShape = false;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			if (line.empty() || line[0] == ';') continue;
			if (line == "[shape]") { shapes.emplace_back(); inShape = true; continue; }
			if (line[0] == '[') continue;
			size_t eq = line.find('=');
			if (eq == std::string::npos) continue;
			auto kv = std::make_pair(line.substr(0, eq), line.substr(eq + 1));
			(inShape && kv.first != "edit" ? shapes.back() : top).push_back(kv);   // (edits appended after a build land after the shapes)
		}
		std::string source;
		for (auto& [k, v] : top) if (k == "source") source = v;
		if (source.empty() || !fs::exists(source)) { AddLog(4, "the recipe's source NIF is missing: " + source); return false; }
		LoadItem(source);
		gM.shapes.clear();
		gM.physics[0] = gM.outDir[0] = 0;
		Capture();
		gM.collideSkip.clear();
		gM.edits.clear();
		for (auto& [k, v] : top) {
			float a = 0, b = 0;
			if (k == "mode") gM.mode = atoi(v.c_str());
			else if (k == "edit") gM.edits.push_back(v);
			else if (k == "physics") Copy(gM.physics, sizeof gM.physics, v);
			else if (k == "outdir") Copy(gM.outDir, sizeof gM.outDir, v);
			else if (k == "proxy") gM.proxy = v == "1";
			else if (k == "proxycell") gM.proxyCell = (float)atof(v.c_str());
			else if (k == "collideskip") gM.collideSkip.insert(v);
			else if (k == "clothmargin") gM.clothMargin = (float)atof(v.c_str());
			else if (k == "bodymargin") gM.bodyMargin = (float)atof(v.c_str());
			else if (k == "penetration") gM.penetration = (float)atof(v.c_str());
			else if (k == "bend") gM.bend = (float)atof(v.c_str());
			else if (k == "stiffness") gM.stiffness = (float)atof(v.c_str());
			else if (k == "weft") gM.weft = v == "1";
			else if (k == "friction") gM.friction = (float)atof(v.c_str());
			else if (k == "mass") gM.mass = (float)atof(v.c_str());
			else if (k == "restitution") gM.restitution = (float)atof(v.c_str());
			else if (k == "damping" && sscanf_s(v.c_str(), "%f %f", &a, &b) == 2) { gM.dampL = a; gM.dampA = b; }
			else if (k == "coatgrid") sscanf_s(v.c_str(), "%d %d", &gM.coatCols, &gM.coatRows);
			else if (k == "coattop") gM.coatTop = (float)atof(v.c_str());
			else if (k == "coatangle") gM.coatAngle = (float)atof(v.c_str());
		}
		PushUndo();   // the chains before the recipe replaced them
		gM.chains.clear();
		gM.selected = -1;
		for (auto& [k, v] : top) if (k == "paint") ReadPaint(v);
		for (auto& kvs : shapes) {
			std::string name;
			for (auto& [k, v] : kvs) if (k == "name") name = v;
			ShapeRow* r = nullptr;
			for (auto& s : gM.shapes) if (s.name == name) r = &s;
			if (!r) { AddLog(3, "recipe: the NIF has no shape " + name); continue; }
			r->detach.clear(); r->keep.clear();
			for (auto& [k, v] : kvs) {
				float a = 0, b = 0;
				if (k == "role") r->role = atoi(v.c_str());
				else if (k == "anchor") r->anchor = v;
				else if (k == "grid") sscanf_s(v.c_str(), "%d %d", &r->cols, &r->rows);
				else if (k == "top") { r->autoTop = v == "auto"; if (!r->autoTop) r->top = (float)atof(v.c_str()); }
				else if (k == "angles") { r->autoAngles = v == "auto"; if (!r->autoAngles && sscanf_s(v.c_str(), "%f %f", &a, &b) == 2) { r->angleLo = a; r->angleHi = b; } }
				else if (k == "radial") r->radial = v == "1";
				else if (k == "collar") r->collar = (float)atof(v.c_str());
				else if (k == "fixedrows") r->fixedRows = atoi(v.c_str());
				else if (k == "detach") { size_t e = v.find('='); if (e != std::string::npos) r->detach.push_back({ v.substr(0, e), v.substr(e + 1) }); }
				else if (k == "keep") { size_t p1 = v.find('|'), p2 = v.rfind('|'); if (p1 != std::string::npos && p2 > p1) r->keep.push_back({ v.substr(0, p1), (float)atof(v.substr(p1 + 1).c_str()), (float)atof(v.substr(p2 + 1).c_str()) }); }
				else if (k == "links") r->links = atoi(v.c_str());
				else if (k == "keeptop") { r->keepTop = v != "off"; if (r->keepTop && sscanf_s(v.c_str(), "%f %f", &a, &b) == 2) { r->keepTopA = a; r->keepTopB = b; } }
			}
		}
		AddLog(2, "recipe loaded: " + path);
		return true;
	}

	// --- running a tool --------------------------------------------------------------------------------------------

	// One command-line argument, quoted as the C runtime reads it back (backslashes before a quote doubled).
	static std::string Quote(const std::string& a)
	{
		std::string o = "\"";
		size_t slashes = 0;
		for (char c : a) {
			if (c == '\\') { slashes++; continue; }
			if (c == '"') o.append(slashes * 2 + 1, '\\');
			else o.append(slashes, '\\');
			slashes = 0;
			o += c;
		}
		o.append(slashes * 2, '\\');
		return o + "\"";
	}

	// A file about to be replaced is kept as <file>.bak (the previous .bak goes).
	static void Backup(const std::string& path)
	{
		std::string err;
		std::string kept = backup::keep(path, &err);
		if (!err.empty()) AddLog(3, "could not back up " + path + ": " + err);
		else if (!kept.empty()) AddLog(2, "kept the previous " + fs::path(path).filename().string() + " as " + fs::path(kept).filename().string());
	}

	static void Run(const char* tool, const std::vector<std::string>& args, const std::string& resultNif, const std::string& recipePath)
	{
		// The tools are built into this exe: it runs itself with the tool's name first (RunTool in previewer.cpp).
		char self[MAX_PATH];
		GetModuleFileNameA(nullptr, self, MAX_PATH);
		std::string cmd = Quote(self) + " " + fs::path(tool).stem().string();
		for (auto& a : args) cmd += " " + Quote(a);
		gM.lastCommand = cmd;
		gM.afterSuccess = nullptr;
		gM.resultNif = resultNif;
		gM.recipeToSave = recipePath;
		gM.running = true;
		gM.finished = false;
		gM.lastResult.clear();
		AddLog(2, std::string("running ") + tool);
		gM.worker = std::thread([cmd]() {
			int code = -1;
			SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
			HANDLE rd = nullptr, wr = nullptr;
			if (CreatePipe(&rd, &wr, &sa, 0)) {
				SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
				STARTUPINFOA si{ sizeof si };
				si.dwFlags = STARTF_USESTDHANDLES;
				si.hStdOutput = si.hStdError = wr;
				PROCESS_INFORMATION pi{};
				std::vector<char> line(cmd.begin(), cmd.end());
				line.push_back(0);
				std::string dir = ToolDir();
				BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
				CloseHandle(wr);
				if (ok) {
					std::string text;
					char buf[4096];
					DWORD n = 0;
					auto flush = [&](bool all) {
						size_t nl;
						while ((nl = text.find('\n')) != std::string::npos || (all && !text.empty())) {
							std::string l = nl == std::string::npos ? text : text.substr(0, nl);
							text.erase(0, nl == std::string::npos ? text.size() : nl + 1);
							if (!l.empty() && l.back() == '\r') l.pop_back();
							if (l.empty()) continue;
							std::lock_guard<std::mutex> g(gM.lock);
							gM.pending.push_back(l);
						}
					};
					while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) { text.append(buf, n); flush(false); }
					flush(true);
					WaitForSingleObject(pi.hProcess, INFINITE);
					DWORD c = 0;
					GetExitCodeProcess(pi.hProcess, &c);
					code = (int)c;
					CloseHandle(pi.hProcess);
					CloseHandle(pi.hThread);
				} else {
					std::lock_guard<std::mutex> g(gM.lock);
					gM.pending.push_back("error: could not start the tool (error " + std::to_string(GetLastError()) + ")");
				}
				CloseHandle(rd);
			}
			gM.exitCode = code;
			gM.finished = true;
		});
	}

	static void ReplayNext();
	static void ApplyKeptValues();

	// Each frame: the tool's output into the log; when it is done, the result opened.
	static void Pump()
	{
		{
			std::lock_guard<std::mutex> g(gM.lock);
			for (auto& l : gM.pending) AddLog(l.rfind("error", 0) == 0 ? 4 : 2, "  " + l);
			if (!gM.pending.empty()) gLogScroll = true;
			gM.pending.clear();
		}
		// The build's kept edits, one at a time (a proxy generation runs a tool: the next waits for it).
		if (gM.replaying && !gM.running) { ReplayNext(); return; }
		// then the kept Bone values (they may be on shapes the kept edits add)
		if (gM.valuesAfterBuild && !gM.running && !gM.replaying) { gM.valuesAfterBuild = false; ApplyKeptValues(); return; }
		if (!gM.finished || !gM.worker.joinable()) return;
		gM.worker.join();
		gM.running = false;
		gM.finished = false;
		auto after = std::move(gM.afterSuccess);
		gM.afterSuccess = nullptr;
		const bool replayNow = gM.replayAfterBuild;
		gM.replayAfterBuild = false;
		if (gM.exitCode != 0) {
			gM.lastResult = "failed (see the log)";
			AddLog(4, "the build failed: nothing was opened");
			if (gM.replaying) AddLog(3, "a kept edit failed; going on with the rest");
			return;
		}
		if (!gM.recipeToSave.empty()) {
			std::ofstream(gM.recipeToSave) << RecipeText();
			AddLog(2, "recipe saved: " + gM.recipeToSave);
		}
		if (after) after();
		gM.lastResult = "done: " + gM.resultNif;
		LoadItem(gM.resultNif);
		if (replayNow && !gM.edits.empty() && gItem.loaded) {
			gM.replay.assign(gM.edits.begin(), gM.edits.end());
			gM.replaying = true;
			AddLog(2, "doing again the " + std::to_string(gM.edits.size()) + " edit(s) made on the last build (proxies, shapes, collisions)");
		}
		if (replayNow && gItem.loaded) gM.valuesAfterBuild = true;
	}

	static void BuildOutfit()
	{
		std::string nif = OutNif(), xml = OutXml();
		if (Lower(fs::path(nif).lexically_normal().string()) == Lower(fs::path(gM.source).lexically_normal().string())) { AddLog(4, "the output would replace the source NIF: pick another output folder"); return; }
		std::vector<std::string> a = { gM.source, nif, gM.physics, xml };
		std::vector<std::string> colliders;
		bool jobs = false;
		if (gM.mode == Coat) {
			const ShapeRow* coat = nullptr;
			std::string legs;
			for (auto& s : gM.shapes) {
				if (s.role == Cloth && !coat) coat = &s;
				else if (s.role == Collider) colliders.push_back(s.name);
				else if (s.role == VirtualLegs && legs.empty()) legs = s.name;
			}
			if (!coat) { AddLog(4, "pick the coat or skirt shape (role Coat)"); return; }
			a = { gM.source, nif, coat->name, gM.physics, xml };
			for (auto& c : colliders) { a.push_back("--collide"); a.push_back(c); }
			if (!legs.empty()) { a.push_back("--virtual-legs"); a.push_back(legs); }
			a.insert(a.end(), { "--grid", std::to_string(gM.coatCols), std::to_string(gM.coatRows), "--top", Num(gM.coatTop), "--angle", Num(gM.coatAngle) });
		} else {
			// Painted chains: one --painted per shape, all reading the one paint file beside the XML.
			std::set<std::string> paintedShapes;
			for (auto& c : gM.chains) if (!c.paint.empty() && c.path.size() >= 2) paintedShapes.insert(c.shape);
			for (auto& s : gM.shapes)
				if (paintedShapes.count(s.name) && (s.role == Cloth || s.role == Chains || s.role == Collider)) {
					AddLog(4, s.name + " has painted chains and the role " + kOutfitRoles[s.role] + ": a shape is one or the other (set it to Leave as is, or delete its chains)");
					return;
				}
			if (!paintedShapes.empty()) {
				std::string paintFile = xml + ".smppaint";
				std::error_code ec;
				fs::create_directories(fs::path(paintFile).parent_path(), ec);
				std::ofstream(paintFile) << PaintText();
				for (auto& s : paintedShapes) a.insert(a.end(), { "--painted", s, paintFile });
				jobs = true;
			}
			for (auto& s : gM.shapes) {
				if (s.role == Collider) colliders.push_back(s.name);
				if (s.role != Cloth && s.role != Chains) continue;
				jobs = true;
				if (s.role == Cloth) {
					a.insert(a.end(), { "--cloth", s.name, "--anchor", s.anchor, "--grid", std::to_string(s.cols), std::to_string(s.rows) });
					if (s.radial) a.insert(a.end(), { "--radial", "--collar", Num(s.collar) });
					else if (!s.autoTop) a.insert(a.end(), { "--top", Num(s.top) });
					if (!s.autoAngles) a.insert(a.end(), { "--angles", Num(s.angleLo), Num(s.angleHi) });
					a.insert(a.end(), { "--fixed-rows", std::to_string(s.fixedRows) });
					for (auto& [b, t] : s.detach) if (!b.empty() && !t.empty()) a.insert(a.end(), { "--detach", b + "=" + t });
					for (auto& k : s.keep) if (!k.bone.empty()) a.insert(a.end(), { "--keep", k.bone, Num(k.w0), Num(k.w1) });
				} else {
					a.insert(a.end(), { "--chains", s.name, "--anchor", s.anchor, "--links", std::to_string(s.links) });
					if (s.keepTop) a.insert(a.end(), { "--keep-top", Num(s.keepTopA), Num(s.keepTopB) });
				}
			}
			for (auto& c : colliders) { a.push_back("--collide"); a.push_back(c); }
			for (auto& b : gM.collideSkip) { a.push_back("--collide-skip"); a.push_back(b); }
			if (gM.proxy && !colliders.empty()) { a.push_back("--proxy"); a.push_back(Num(gM.proxyCell)); }
			if (!jobs && !(gM.proxy && !colliders.empty())) { AddLog(4, "nothing to do: paint a chain, or give a shape the role Cloth or Chains (or colliders with proxies on)"); return; }
			if (jobs) {
				a.insert(a.end(), { "--cloth-margin", Num(gM.clothMargin), "--body-margin", Num(gM.bodyMargin), "--penetration", Num(gM.penetration),
					"--bend", Num(gM.bend), "--stiffness", Num(gM.stiffness), "--cloth-friction", Num(gM.friction), "--cloth-mass", Num(gM.mass),
					"--cloth-restitution", Num(gM.restitution), "--cloth-damping", Num(gM.dampL), Num(gM.dampA) });
				if (!gM.weft) a.push_back("--no-weft");
			}
		}
		std::error_code ec;
		fs::create_directories(fs::path(nif).parent_path(), ec);
		fs::create_directories(fs::path(xml).parent_path(), ec);
		Backup(nif);
		if (gM.mode == Coat || jobs) Backup(xml);
		gM.replay.clear(); gM.replaying = false;
		Run(gM.mode == Coat ? "smpify.exe" : "smpdress.exe", a, nif, (gM.mode == Coat || jobs) ? xml + ".smprecipe" : std::string());
		gM.replayAfterBuild = gM.running;   // (Run could not start it: nothing to redo)
	}

	/* Low-poly collision proxies for an armour that collides with its full meshes: smpdress's proxy-only mode adds a
	   hidden "SMP Proxy:<name>" stand-in beside each of `shapes` in the NIF (vertices merged in cells of `cell` units),
	   then, once that worked, the XML's per-triangle-shapes of those names are renamed to the proxies (the plugin
	   hides the proxies in game). Both files kept first (backup.h); the result opened. A shape that is a proxy already
	   ("SMP Proxy:...") is rebuilt in place under its name, and a shape's existing proxy is replaced (smpdress).
	   maxTris > 0: each proxy's cell grows from `cell` until it has at most that many triangles (smpdress --proxy-tris). */
	static void GenerateProxies(const std::string& nif, const std::string& xml, const std::vector<std::string>& shapes, float cell, int maxTris = 0)
	{
		if (nif.empty() || shapes.empty()) return;
		Backup(nif);
		if (!xml.empty()) Backup(xml);
		std::vector<std::string> a = { nif, nif, "x", "x" };
		for (auto& s : shapes) { a.push_back("--collide"); a.push_back(s); }
		a.push_back("--proxy");
		a.push_back(Num(cell));
		if (maxTris > 0) { a.push_back("--proxy-tris"); a.push_back(std::to_string(maxTris)); }
		Run("smpdress.exe", a, nif, {});
		if (!gM.running) return;   // could not start it
		gM.afterSuccess = [xml, shapes, cell, maxTris]() {
			std::string e = "proxies|" + Num(cell) + "|" + std::to_string(maxTris);
			for (auto& s : shapes) e += "|" + s;
			RecordEdit(e);
			if (xml.empty()) return;
			std::ifstream in(xml, std::ios::binary);
			std::string text((std::istreambuf_iterator<char>(in)), {});
			in.close();
			int renamed = 0;
			for (auto& s : shapes) {
				// A proxy rebuilt in place keeps its name: the XML already points at it.
				if (_strnicmp(s.c_str(), "SMP Proxy:", 10) == 0) continue;
				// <per-triangle-shape ... name="Shape" ...>: the name, matched as the plugin does (case aside).
				std::regex re("(<per-triangle-shape[^>]*\\bname\\s*=\\s*\")([^\"]+)(\")", std::regex::icase);
				std::string outText;
				auto begin = text.cbegin();
				for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
					outText.append(begin, (*it)[0].first);
					if (_stricmp((*it)[2].str().c_str(), s.c_str()) == 0) { outText += (*it)[1].str() + "SMP Proxy:" + s + (*it)[3].str(); renamed++; }
					else outText += (*it)[0].str();
					begin = (*it)[0].second;
				}
				outText.append(begin, text.cend());
				text = outText;
			}
			std::ofstream(xml, std::ios::binary) << text;
			AddLog(2, "the XML collides with the proxies now (" + std::to_string(renamed) + " collision shape(s) renamed): " + xml);
		};
	}

	static bool SamePath(const std::string& a, const std::string& b)
	{
		return !a.empty() && !b.empty() && Lower(fs::path(a).lexically_normal().string()) == Lower(fs::path(b).lexically_normal().string());
	}

	// An edit made in the previewer on a Maker build's output (its recipe beside its XML): added to that recipe on disk
	// and, when it is this session's build, to the kept edits, so the next build does it again. Not while replaying them.
	static void RecordEdit(const std::string& e)
	{
		if (gM.replaying) return;
		const std::string recipe = RecipeOf(gItem.xmlPath);
		if (recipe.empty()) return;   // not a Maker build: nothing rebuilds it
		std::ofstream(recipe, std::ios::app) << "edit=" << e << "\n";
		if (!gM.source.empty() && SamePath(gItem.nifPath, OutNif())) gM.edits.push_back(e);
		AddLog(2, "kept for the next build: " + e);
	}

	// One kept edit done again on the fresh build (open in the view), as the previewer did it. A proxy generation runs a
	// tool: Pump calls this again once it is done.
	static void ReplayNext()
	{
		if (gM.replay.empty()) { gM.replaying = false; AddLog(2, "the kept edits are done again"); return; }
		const std::string e = gM.replay.front();
		gM.replay.pop_front();
		std::vector<std::string> p;
		for (size_t a = 0, b; ; a = b + 1) { b = e.find('|', a); p.push_back(e.substr(a, b == std::string::npos ? std::string::npos : b - a)); if (b == std::string::npos) break; }
		const std::string& kind = p[0];
		AddLog(2, "again: " + e);
		if (kind == "proxies" && p.size() >= 4)
			GenerateProxies(gItem.nifPath, gItem.xmlPath, std::vector<std::string>(p.begin() + 3, p.end()), (float)atof(p[1].c_str()), atoi(p[2].c_str()));
		else if (kind == "proxyflag" && p.size() >= 3) ToggleProxy(p[1], atoi(p[2].c_str()));
		else if (kind == "delete" && p.size() >= 3) DeleteShapes({ { p[1], atoi(p[2].c_str()) } });
		else if (kind == "xmladd" && p.size() >= 3) AddXmlCollision(p[1] == "body", p[2]);
		else if (kind == "useproxies") UseProxiesInXml();
		else if (kind == "xmlcol" && p.size() >= 3) {
			// by the collision shape's name (its place in the list can differ in the new XML)
			auto list = XmlCollisions(ReadText(gItem.xmlPath));
			int k = -1;
			for (int i = 0; i < (int)list.size() && k < 0; i++) if (_stricmp(list[i].name.c_str(), p[1].c_str()) == 0) k = i;
			if (k >= 0) EditXmlCollision(k, p[2]);
			else AddLog(3, "the new XML has no collision shape " + p[1] + ": skipped");
		}
		else AddLog(3, "unknown kept edit, skipped: " + e);
	}

	// --- Bone values kept across rebuilds (2026-11-14 user: rebuilds should remember the Bone values changes) ----------
	// A build's XML as built (its kept edits done again) is kept as <xml>.smpbuilt; the values saved from Bone values that
	// differ from it, by name (xmledit::Values), go in <xml>.smpvalues; after the next build they are put back in.
	static std::string ReadAll(const std::string& path) { std::ifstream in(path, std::ios::binary); return std::string((std::istreambuf_iterator<char>(in)), {}); }
	static std::map<std::string, std::string> ReadKeptValues(const std::string& xml)
	{
		std::map<std::string, std::string> v;
		std::ifstream in(xml + ".smpvalues");
		std::string line;
		while (std::getline(in, line)) {
			if (!line.empty() && line.back() == '\r') line.pop_back();
			const size_t tab = line.find('\t');
			if (tab != std::string::npos) v[line.substr(0, tab)] = line.substr(tab + 1);
		}
		return v;
	}

	// Save to XML on a Maker build (its recipe beside the XML): what differs from the build is kept for the next one.
	static void RecordValues(const std::string& xml, const xmledit::Doc& now)
	{
		if (RecipeOf(xml).empty()) return;
		std::error_code ec;
		std::string base = xml + ".smpbuilt";
		if (!fs::exists(base, ec)) {
			// built before this was kept: the file as it was before the first save from Bone values
			base = xml + ".bak";
			if (!fs::exists(base, ec)) { AddLog(3, "the bone values are not kept for the next build: build once more first (nothing to tell them from)"); return; }
			AddLog(3, "comparing the bone values with " + base + " (this build is older than kept bone values)");
		}
		xmledit::Doc b;
		b.Parse(ReadAll(base));
		const auto was = xmledit::Values(b), is = xmledit::Values(now);
		std::ofstream out(xml + ".smpvalues", std::ios::binary);
		int n = 0;
		for (auto& [k, v] : is) {
			auto it = was.find(k);
			if (it != was.end() && it->second == v) continue;
			out << k << "\t" << v << "\n";
			n++;
		}
		out.close();
		if (!n) fs::remove(xml + ".smpvalues", ec);
		AddLog(2, n ? "kept for the next build: " + std::to_string(n) + " bone value(s) changed from the build" : "no bone values differ from the build: none kept");
	}

	// After a build (and its kept edits): the XML as built kept, then the kept bone values put in.
	static void ApplyKeptValues()
	{
		const std::string xml = gItem.xmlPath;
		if (xml.empty() || !SamePath(gItem.nifPath, OutNif())) return;
		std::error_code ec;
		fs::copy_file(xml, xml + ".smpbuilt", fs::copy_options::overwrite_existing, ec);
		const auto kept = ReadKeptValues(xml);
		if (kept.empty()) return;
		xmledit::Doc d;
		d.Parse(ReadAll(xml));
		int missing = 0;
		const int applied = xmledit::ApplyValues(d, kept, missing);
		if (applied) {
			std::ofstream(xml, std::ios::binary) << d.Compose();
			LoadItem(gItem.nifPath);
		}
		AddLog(missing ? 3 : 2, "the kept bone values are back: " + std::to_string(applied) + " put in" +
			(missing ? ", " + std::to_string(missing) + " not found in the new build (a chain or shape no longer built)" : ""));
	}

	// Writes the missing "SMP Bones" lists into the NIF (smprepair --list, the game's skeleton deciding which bones are
	// the item's own), keeping the old file (backup.h), and opens the result.
	static void AddBoneLists(const std::string& nif, const std::string& skeleton)
	{
		if (nif.empty() || skeleton.empty()) { AddLog(4, "the NIF and the game's skeleton.nif are both needed"); return; }
		Backup(nif);
		Run("smprepair.exe", { "--list", nif, skeleton, nif }, nif, {});
	}

	static void BuildRepair()
	{
		if (!gM.edited[0] || !gM.reference[0] || !gM.repaired[0]) { AddLog(4, "pick the edited NIF, the one before the edit, and where the repaired one goes"); return; }
		Backup(gM.repaired);
		Run("smprepair.exe", { gM.edited, gM.reference, gM.repaired }, gM.repaired, {});
	}

	static bool BoneCombo(const char* label, std::string& bone, const std::vector<std::string>& bones);
	static void OutputRows();
	static void BuildButtons(bool repair);

	// --- painting -------------------------------------------------------------------------------------------------

	// The source is in the view and the paint tab is up: the body holds still and the left mouse paints.
	static bool Painting() { return gM.open && gM.paintTab && gM.paintOn && gItem.loaded && !gM.source.empty() && gItem.nifPath == gM.source; }

	static int ShapeIndex(const std::string& name)
	{
		for (size_t i = 0; i < gItem.shapes.size(); i++) {
			auto* g = gItem.shapes[i].info.geometry;
			if (g->m_name && name == g->m_name) return (int)i;
		}
		return -1;
	}

	static V3 VertexAt(const ShapeDraw& s, int v) { auto& m = s.verts[v]; return V3(m.px, m.py, m.pz); }

	// The nearest triangle under the ray, over the visible shapes (or only shape `only`): its shape and the point.
	static bool Raycast(V3 o, V3 d, int only, int& shape, V3& point, V3& normal, int& nearVertex)
	{
		float best = 1e30f;
		shape = -1;
		for (size_t si = 0; si < gItem.shapes.size(); si++) {
			auto& s = gItem.shapes[si];
			if (!s.visible || s.indices.empty() || (only >= 0 && (int)si != only)) continue;
			for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
				V3 a = VertexAt(s, s.indices[t]), b = VertexAt(s, s.indices[t + 1]), c = VertexAt(s, s.indices[t + 2]);
				V3 e1 = b - a, e2 = c - a, p = Cross(d, e2);
				float det = Dot(e1, p);
				if (std::fabs(det) < 1e-9f) continue;
				float inv = 1.0f / det;
				V3 q = o - a;
				float u = Dot(q, p) * inv;
				if (u < 0 || u > 1) continue;
				V3 r = Cross(q, e1);
				float w = Dot(d, r) * inv;
				if (w < 0 || u + w > 1) continue;
				float dist = Dot(e2, r) * inv;
				if (dist <= 0 || dist >= best) continue;
				best = dist;
				shape = (int)si;
				point = o + d * dist;
				normal = Norm(Cross(e1, e2));
				float wa = 1 - u - w;
				nearVertex = wa >= u && wa >= w ? s.indices[t] : u >= w ? s.indices[t + 1] : s.indices[t + 2];
			}
		}
		return shape >= 0;
	}

	// The anchor a new chain hangs from: the bone its first stretch was weighted to most (not one of the SMP bones).
	static std::string GuessAnchor(const ShapeDraw& s, const std::vector<int>& path)
	{
		std::map<std::string, float> sum;
		size_t n = std::max<size_t>(1, std::min<size_t>(3, path.size()));
		for (size_t i = 0; i < n && i < path.size(); i++)
			for (auto& [slot, w] : s.weights[path[i]])
				if (const char* name = SkinBoneName(s, slot)) if (_strnicmp(name, "SMP", 3) != 0) sum[name] += w;
		std::string best = "Bip01 Pelvis";
		float bw = 0;
		for (auto& [name, w] : sum) if (w > bw) { bw = w; best = name; }
		return best;
	}

	static float PathLength(const ShapeDraw& s, const std::vector<int>& path)
	{
		float l = 0;
		for (size_t i = 1; i < path.size(); i++) { V3 d = VertexAt(s, path[i]) - VertexAt(s, path[i - 1]); l += std::sqrt(Dot(d, d)); }
		return l;
	}

	// smpdress's space for a shape (the skin's: NiSkinData's transform on the vertices) to the NIF pose the Maker shows.
	static RE::NiTransform SkinToWorld(const ShapeDraw& s)
	{
		auto* g = s.info.geometry;
		if (!g->skinInstance || !g->skinInstance->skinData) return g->world;
		return NifPoseTransform(g) * g->skinInstance->skinData->rootParentToSkin.Invert();
	}

	// The chain's bones as they will be built: links + 1 points evenly along its stroke, or where they were moved to.
	static std::vector<V3> PlannedBones(const ShapeDraw& s, const PaintChain& c)
	{
		std::vector<V3> line;
		for (int v : c.path) line.push_back(VertexAt(s, v));
		std::vector<V3> out;
		if (line.size() < 2) return out;
		std::vector<float> at(line.size(), 0.0f);
		for (size_t i = 1; i < line.size(); i++) { V3 d = line[i] - line[i - 1]; at[i] = at[i - 1] + std::sqrt(Dot(d, d)); }
		for (int k = 0; k <= c.links; k++) {
			float target = at.back() * k / c.links;
			size_t i = 1;
			while (i + 1 < line.size() && at[i] < target) i++;
			float t = std::clamp((target - at[i - 1]) / std::max(at[i] - at[i - 1], 1e-6f), 0.0f, 1.0f);
			out.push_back(line[i - 1] + (line[i] - line[i - 1]) * t);
		}
		if (!c.points.empty()) {
			const RE::NiTransform toWorld = SkinToWorld(s);
			for (auto& [k, p] : c.points)
				if (k >= 0 && k < (int)out.size()) { RE::NiPoint3 w = toWorld * RE::NiPoint3{ p.x, p.y, p.z }; out[k] = V3(w.x, w.y, w.z); }
		}
		return out;
	}

	// --- weights -----------------------------------------------------------------------------------------------------

	// Each vertex's neighbours over the triangles, with vertices at the same spot in the NIF (UV and normal seams split
	// them) joined, so smoothing runs across seams. Cached per shape (by name and vertex count).
	static const std::vector<std::vector<int>>& Neighbours(const ShapeDraw& s)
	{
		static std::map<std::string, std::vector<std::vector<int>>> cache;
		std::string key = (s.info.geometry->m_name ? s.info.geometry->m_name : "") + std::string("#") + std::to_string(s.verts.size());
		auto it = cache.find(key);
		if (it != cache.end()) return it->second;
		const int n = (int)s.verts.size();
		auto* d = s.info.geometry->data;
		std::vector<int> weld(n);
		std::map<std::tuple<int, int, int>, int> at;
		for (int v = 0; v < n; v++) {
			RE::NiPoint3 p = d && d->vertices && v < d->vertexCount ? d->vertices[v] : RE::NiPoint3{ s.verts[v].px, s.verts[v].py, s.verts[v].pz };
			auto k = std::make_tuple((int)std::lround(p.x * 100), (int)std::lround(p.y * 100), (int)std::lround(p.z * 100));
			weld[v] = at.emplace(k, v).first->second;   // the first vertex at this spot stands for all of them
		}
		std::vector<std::set<int>> sets(n);
		for (size_t t = 0; t + 2 < s.indices.size(); t += 3)
			for (int a = 0; a < 3; a++)
				for (int b = 0; b < 3; b++)
					if (a != b) sets[weld[s.indices[t + a]]].insert(weld[s.indices[t + b]]);
		auto& out = cache[key];
		out.assign(n, {});
		for (int v = 0; v < n; v++) {
			for (int w : sets[weld[v]]) out[v].push_back(w);
			if (weld[v] != v) out[v].push_back(weld[v]);   // a seam copy also leans on its twin
		}
		return out;
	}

	// Point k (not the tie-on) taken out of the chain, one link fewer: the others keep their places, as moved points
	// (left to the stroke they would be spread out again).
	static void DeletePoint(PaintChain& c, const ShapeDraw& s, int k)
	{
		if (k < 1 || k > c.links || c.links < 2) return;
		std::vector<V3> bones = PlannedBones(s, c);
		if ((int)bones.size() != c.links + 1) return;
		bones.erase(bones.begin() + k);
		const RE::NiTransform toSkin = SkinToWorld(s).Invert();
		c.links--;
		c.points.clear();
		for (int j = 1; j <= c.links; j++) { RE::NiPoint3 p = toSkin * RE::NiPoint3{ bones[j].x, bones[j].y, bones[j].z }; c.points[j] = V3(p.x, p.y, p.z); }
	}

	// (2026-11-14 user: chains come out jagged, evening every point by hand is tedious.) The chain's middle points (not
	// the tie-on, not the end) moved, as moved points: the paint stays.
	// Smooth: each halfway to its neighbours' midpoint (zig-zags iron out; each click a bit more, the chain straightening
	// towards the line between its ends). Even: spread at equal distances along the path the points make now (its shape kept).
	static void RelaxPoints(PaintChain& c, const ShapeDraw& s, bool even)
	{
		const std::vector<V3> pts = PlannedBones(s, c);
		const int n = (int)pts.size() - 1;
		if (n < 2 || n != c.links) return;
		std::vector<V3> out = pts;
		if (!even)
			for (int k = 1; k < n; k++) out[k] = pts[k] + ((pts[k - 1] + pts[k + 1]) * 0.5f - pts[k]) * 0.5f;
		else {
			std::vector<float> at(n + 1, 0.0f);
			for (int k = 1; k <= n; k++) { V3 d = pts[k] - pts[k - 1]; at[k] = at[k - 1] + std::sqrt(Dot(d, d)); }
			for (int k = 1, i = 1; k < n; k++) {
				const float want = at[n] * k / n;
				while (i < n && at[i] < want) i++;
				const float t = at[i] > at[i - 1] ? (want - at[i - 1]) / (at[i] - at[i - 1]) : 0.0f;
				out[k] = pts[i - 1] + (pts[i] - pts[i - 1]) * t;
			}
		}
		const RE::NiTransform toSkin = SkinToWorld(s).Invert();
		for (int k = 1; k < n; k++) { RE::NiPoint3 p = toSkin * RE::NiPoint3{ out[k].x, out[k].y, out[k].z }; c.points[k] = V3(p.x, p.y, p.z); }
	}

	// The paint of a chain at a vertex (0 where it is not painted).
	static float PaintAt(const PaintChain& c, int v) { auto it = c.paint.find(v); return it == c.paint.end() ? 0.0f : it->second; }
	// Each vertex's weight on the chain's point k as smpdress will build it: its paint, split between the two links it lies
	// between (by where it projects onto the stroke, or onto the line through the bones once a point is moved).
	static std::vector<float> PointWeights(const ShapeDraw& s, const PaintChain& c, int k)
	{
		std::vector<float> out(s.verts.size(), 0.0f);
		const int n = c.links;
		std::vector<V3> line;
		for (int v : c.path) { V3 p = VertexAt(s, v); if (line.empty() || Dot(p - line.back(), p - line.back()) > 0.0025f) line.push_back(p); }
		const std::vector<V3> bones = PlannedBones(s, c);
		if (line.size() < 2 || (int)bones.size() != n + 1) return out;
		std::vector<float> at(line.size(), 0.0f);
		for (size_t i = 1; i < line.size(); i++) { V3 d = line[i] - line[i - 1]; at[i] = at[i - 1] + std::sqrt(Dot(d, d)); }
		const float step = std::max(at.back() / n, 1e-6f);
		const bool moved = !c.points.empty();
		const std::vector<V3>& poly = moved ? bones : line;
		for (auto& [v, paint] : c.paint) {
			if (v < 0 || v >= (int)out.size()) continue;
			V3 p = VertexAt(s, v);
			float best = 1e30f, u = 0;
			for (size_t i = 1; i < poly.size(); i++) {
				V3 d = poly[i] - poly[i - 1], q = p - poly[i - 1];
				float t = std::clamp(Dot(q, d) / std::max(Dot(d, d), 1e-9f), 0.0f, 1.0f);
				V3 r = q - d * t;
				float e = Dot(r, r);
				if (e < best) { best = e; u = moved ? (i - 1) + t : (at[i - 1] + t * (at[i] - at[i - 1])) / step; }
			}
			u = std::clamp(u, 0.0f, (float)n);
			int k0 = std::min((int)u, n - 1);
			float fk = u - k0, w = k == k0 ? 1 - fk : k == k0 + 1 ? fk : 0.0f;
			out[v] = std::clamp(paint, 0.0f, 1.0f) * w;
		}
		return out;
	}
	static bool Masked(const std::string& shape, int v) { auto it = gM.masks.find(shape); return it != gM.masks.end() && it->second.count(v); }
	// Every paint edit goes through here: a masked vertex keeps what it has.
	static void SetPaint(PaintChain& c, int v, float p) { if (Masked(c.shape, v)) return; if (p < 0.03f) c.paint.erase(v); else c.paint[v] = std::min(p, 1.0f); }

	// The mask brush: every vertex under it masked (or unmasked).
	static void MaskBrush(const std::string& shape, const ShapeDraw& s, V3 at, bool unmask)
	{
		auto& m = gM.masks[shape];
		float r2 = gM.radius * gM.radius;
		for (int v = 0; v < (int)s.verts.size(); v++) {
			V3 d = VertexAt(s, v) - at;
			if (Dot(d, d) > r2) continue;
			if (unmask) m.erase(v); else m.insert(v);
		}
		if (m.empty()) gM.masks.erase(shape);
	}

	// Ctrl+click with Mask / Unmask: the whole element vertex v is on (the connected piece, over the triangles and across
	// seams) masked (or unmasked). How many vertices that was.
	static int MaskElement(const std::string& shape, const ShapeDraw& s, int v, bool unmask)
	{
		const auto& nb = Neighbours(s);
		if (v < 0 || v >= (int)nb.size()) return 0;
		std::vector<char> in(nb.size(), 0);
		std::vector<int> open{ v };
		in[v] = 1;
		while (!open.empty()) {
			const int x = open.back();
			open.pop_back();
			for (int w : nb[x]) if (!in[w]) { in[w] = 1; open.push_back(w); }
		}
		// A seam copy lists the vertex it is welded to, which does not list it back: taken in by its link into the piece.
		for (int u = 0; u < (int)nb.size(); u++)
			if (!in[u]) for (int w : nb[u]) if (in[w]) { in[u] = 2; break; }
		auto& m = gM.masks[shape];
		int count = 0;
		for (int u = 0; u < (int)nb.size(); u++)
			if (in[u]) { if (unmask) m.erase(u); else m.insert(u); ++count; }
		if (m.empty()) gM.masks.erase(shape);
		return count;
	}

	// Smoothing: each vertex moves toward its neighbours' average (unpainted ones count as 0), by `f` (0..1) each.
	static void SmoothVertices(PaintChain& c, const ShapeDraw& s, const std::vector<std::pair<int, float>>& verts)
	{
		const auto& nb = Neighbours(s);
		std::vector<std::pair<int, float>> next;
		for (auto& [v, f] : verts) {
			if (v >= (int)nb.size() || nb[v].empty()) continue;
			float sum = 0;
			for (int w : nb[v]) sum += PaintAt(c, w);
			float p = PaintAt(c, v);
			next.push_back({ v, p + (sum / nb[v].size() - p) * f });
		}
		for (auto& [v, p] : next) SetPaint(c, v, p);
	}

	// The smoothing brush: the vertices under it, by its falloff and strength.
	static void SmoothBrush(PaintChain& c, const ShapeDraw& s, V3 at)
	{
		float r2 = gM.radius * gM.radius;
		std::vector<std::pair<int, float>> verts;
		for (int v = 0; v < (int)s.verts.size(); v++) {
			V3 d = VertexAt(s, v) - at;
			float dd = Dot(d, d);
			if (dd > r2) continue;
			float f = 1 - dd / r2;
			verts.push_back({ v, f * f * (3 - 2 * f) * gM.strength * 0.5f });
		}
		SmoothVertices(c, s, verts);
	}

	// The whole chain smoothed once: every painted vertex and the ones around it.
	static void SmoothChain(PaintChain& c, const ShapeDraw& s)
	{
		const auto& nb = Neighbours(s);
		std::set<int> region;
		for (auto& [v, p] : c.paint) {
			region.insert(v);
			if (v < (int)nb.size()) for (int w : nb[v]) region.insert(w);
		}
		std::vector<std::pair<int, float>> verts;
		for (int v : region) verts.push_back({ v, 0.5f });
		SmoothVertices(c, s, verts);
	}

	// Grow or fade the chain's influence around one of its points: within `reach` of it, by `amount` at the point,
	// falling off smoothly to nothing at the reach. Growing also takes in vertices not painted yet.
	static void GrowAround(PaintChain& c, const ShapeDraw& s, V3 at, bool grow)
	{
		float r2 = gM.reach * gM.reach;
		for (int v = 0; v < (int)s.verts.size(); v++) {
			V3 d = VertexAt(s, v) - at;
			float dd = Dot(d, d);
			if (dd > r2) continue;
			float f = 1 - dd / r2;
			f = f * f * (3 - 2 * f) * gM.amount;
			float p = PaintAt(c, v);
			SetPaint(c, v, grow ? p + (1 - p) * f : p * (1 - f));
		}
	}

	// --weights-test (testing): on the first chain, smoothing three times, then growing and fading at point 2, each
	// logged as painted vertices, total paint and the largest step between neighbours (smoothing lowers it).
	static void WeightsTest()
	{
		if (gM.chains.empty()) { AddLog(4, "weights test: no chain"); return; }
		PaintChain& c = gM.chains[0];
		int si = ShapeIndex(c.shape);
		if (si < 0) { AddLog(4, "weights test: the chain's shape is gone"); return; }
		const ShapeDraw& s = gItem.shapes[si];
		auto report = [&](const char* what) {
			const PaintChain& c = gM.chains[0];   // fresh: Undo replaces the chains
			const auto& nb = Neighbours(s);
			float total = 0, step = 0;
			for (auto& [v, p] : c.paint) {
				total += p;
				if (v < (int)nb.size()) for (int w : nb[v]) step = std::max(step, std::fabs(p - PaintAt(c, w)));
			}
			char b[160];
			snprintf(b, sizeof b, "weights test, %s: %zu vertices, paint %.2f, largest neighbour step %.2f", what, c.paint.size(), total, step);
			AddLog(2, b);
		};
		report("painted");
		const size_t before = gM.undo.size();
		for (int i = 0; i < 3; i++) { PushUndo(); SmoothChain(gM.chains[0], s); }
		report("smoothed x3");
		auto points = PlannedBones(s, c);
		if (points.size() > 2) {
			gM.reach = 8; gM.amount = 0.5f;
			PushUndo(); GrowAround(gM.chains[0], s, points[2], true);
			report("grown at point 2");
			PushUndo(); GrowAround(gM.chains[0], s, points[2], false);
			report("faded at point 2");
		}
		while (gM.undo.size() > before) Undo();
		report("all undone");
		while (!gM.redo.empty()) Redo();
		report("all redone");
		while (gM.undo.size() > before) Undo();
		// Masked vertices keep their paint: every painted one masked, then a grow at point 2 (new vertices may still be taken in).
		std::map<int, float> was = gM.chains[0].paint;
		for (auto& [v, p] : was) gM.masks[gM.chains[0].shape].insert(v);
		if (points.size() > 2) GrowAround(gM.chains[0], s, points[2], true);
		int changed = 0;
		for (auto& [v, p] : was) changed += PaintAt(gM.chains[0], v) != p;
		AddLog(2, "weights test, masked grow: " + std::to_string(changed) + " masked vertices changed (0 expected), " + std::to_string(gM.chains[0].paint.size() - was.size()) + " unmasked taken in");
		gM.masks.clear();
		// A moved point: point 2 two units along x in skin space; the planned bone follows, the paint file names it.
		if (points.size() > 2) {
			auto before2 = PlannedBones(s, gM.chains[0])[2];
			auto weightsBefore = PointWeights(s, gM.chains[0], 2);
			RE::NiPoint3 p = SkinToWorld(s).Invert() * RE::NiPoint3{ before2.x, before2.y, before2.z };
			gM.chains[0].points[2] = V3(p.x + 2, p.y, p.z);
			auto weightsAfter = PointWeights(s, gM.chains[0], 2);
			float sumB = 0, sumA = 0, diff = 0;
			for (size_t v = 0; v < weightsBefore.size(); v++) { sumB += weightsBefore[v]; sumA += weightsAfter[v]; diff += std::fabs(weightsAfter[v] - weightsBefore[v]); }
			char wb[200];
			snprintf(wb, sizeof wb, "weights test, point 2's weights: %.2f before the move, %.2f after, %.2f changed", sumB, sumA, diff);
			AddLog(2, wb);
			auto after2 = PlannedBones(s, gM.chains[0])[2];
			V3 d = after2 - before2;
			char b[200];
			snprintf(b, sizeof b, "weights test, moved point 2: bone moved %.2f units, paint file has it: %d", std::sqrt(Dot(d, d)), PaintText().find("\npoint 2 ") != std::string::npos);
			AddLog(2, b);
			// Point 1 deleted: a link fewer, the old point 2 now point 1 where it was; undone, both back.
			const int links = gM.chains[0].links;
			PushUndo(); DeletePoint(gM.chains[0], s, 1);
			V3 e = PlannedBones(s, gM.chains[0])[1] - after2;
			snprintf(b, sizeof b, "weights test, deleted point 1: links %d -> %d, old point 2 off by %.3f", links, gM.chains[0].links, std::sqrt(Dot(e, e)));
			AddLog(2, b);
			Undo();
			snprintf(b, sizeof b, "weights test, delete undone: links %d", gM.chains[0].links);
			AddLog(2, b);
		}
	}

	static void Brush(PaintChain& c, const ShapeDraw& s, V3 at, bool erase)
	{
		float r2 = gM.radius * gM.radius;
		for (int v = 0; v < (int)s.verts.size(); v++) {
			V3 d = VertexAt(s, v) - at;
			float dd = Dot(d, d);
			if (dd > r2) continue;
			float f = 1 - dd / r2;
			f = f * f * (3 - 2 * f) * gM.strength;
			if (erase) {
				auto it = c.paint.find(v);
				if (it == c.paint.end()) continue;
				SetPaint(c, v, it->second * (1 - f));
			} else
				SetPaint(c, v, std::max(PaintAt(c, v), f));
		}
	}

	// A point of the Move points group still there (its chain and point not deleted since).
	static bool GroupValid(const std::pair<int, int>& g) { return g.first >= 0 && g.first < (int)gM.chains.size() && g.second >= 0 && g.second <= gM.chains[g.first].links; }
	static bool InGroup(int ci, int k) { return std::find(gM.group.begin(), gM.group.end(), std::make_pair(ci, k)) != gM.group.end(); }

	// Move points: a click near a chain's point (the selected chain's first) picks it up; dragging moves it in the plane
	// through it facing the camera. Ctrl+click adds a point to the group or takes it out (2026-11-14): dragging any point of
	// the group moves them all by as much; a plain click on a point outside it starts a group of that one. Only the bones
	// move: the mesh stays, and the build weights it to the bones where they are. On the NIF's pose only (what the build reads).
	static void MovePoints(V3 eye, V3 dir, bool overUi, int forced)
	{
		ImGuiIO& io = ImGui::GetIO();
		bool down = forced >= 0 ? forced == 1 : ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyCtrl;
		bool clicked = forced >= 0 ? forced == 1 : ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		if (gM.stroking) {
			if (!down || gM.group.empty()) { gM.stroking = false; return; }
			float dn = Dot(dir, gM.dragNormal);
			if (std::fabs(dn) < 1e-6f) return;
			float along = Dot(gM.dragPlane - eye, gM.dragNormal) / dn;
			if (along <= 0) return;
			const V3 delta = eye + dir * along - gM.dragPlane;   // how far the grabbed point went: every one in the group goes as far
			for (size_t i = 0; i < gM.group.size() && i < gM.groupStart.size(); i++) {
				if (!GroupValid(gM.group[i])) continue;
				PaintChain& c = gM.chains[gM.group[i].first];
				int si = ShapeIndex(c.shape);
				if (si < 0) continue;
				const V3 w = gM.groupStart[i] + delta;
				RE::NiPoint3 p = SkinToWorld(gItem.shapes[si]).Invert() * RE::NiPoint3{ w.x, w.y, w.z };
				c.points[gM.group[i].second] = V3(p.x, p.y, p.z);
			}
			return;
		}
		// Ctrl+click picks points into the group (down leaves Ctrl out: a Ctrl+click never drags)
		const bool ctrlClick = forced < 0 && io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		if (overUi || !clicked || (!down && !ctrlClick)) return;
		if (!gM.nifPose) { AddLog(3, "moving points works on the NIF's pose: turn on Show the NIF's pose"); return; }
		float best = 1e30f;
		int bc = -1, bk = -1;
		V3 bp;
		for (size_t ci = 0; ci < gM.chains.size(); ci++) {
			int si = ShapeIndex(gM.chains[ci].shape);
			if (si < 0 || gM.chains[ci].hidden) continue;   // (a hidden chain's points are not drawn: not picked either)
			auto pts = PlannedBones(gItem.shapes[si], gM.chains[ci]);
			for (size_t k = 0; k < pts.size(); k++) {
				V3 q = pts[k] - eye;
				float along = Dot(q, dir);
				if (along <= 0) continue;
				V3 perp = q - dir * along;
				float tol = std::max(0.8f, along * 0.02f), d = std::sqrt(Dot(perp, perp));
				if (d > tol) continue;
				float score = d / tol + ((int)ci == gM.selected ? 0.0f : 0.5f);
				if (score < best) { best = score; bc = (int)ci; bk = (int)k; bp = pts[k]; }
			}
		}
		gM.group.erase(std::remove_if(gM.group.begin(), gM.group.end(), [](const std::pair<int, int>& g) { return !GroupValid(g); }), gM.group.end());
		if (bc < 0) { if (!ctrlClick) gM.group.clear(); return; }   // a click on nothing: the group goes
		if (ctrlClick) {
			// the point already picked joins first, so Ctrl+click after a plain click makes a group of two
			if (gM.group.empty() && gM.selected >= 0 && gM.point >= 0 && !(gM.selected == bc && gM.point == bk)) gM.group.push_back({ gM.selected, gM.point });
			auto it = std::find(gM.group.begin(), gM.group.end(), std::make_pair(bc, bk));
			if (it != gM.group.end()) gM.group.erase(it); else gM.group.push_back({ bc, bk });
			gM.selected = bc;
			gM.point = bk;
			return;
		}
		if (!InGroup(bc, bk)) gM.group = { { bc, bk } };
		gM.groupStart.clear();
		for (auto& g : gM.group) {
			int si = ShapeIndex(gM.chains[g.first].shape);
			auto pts = si >= 0 ? PlannedBones(gItem.shapes[si], gM.chains[g.first]) : std::vector<V3>();
			gM.groupStart.push_back(g.second < (int)pts.size() ? pts[g.second] : bp);
		}
		PushUndo();
		gM.selected = bc;
		gM.point = bk;
		gM.dragPlane = bp;
		gM.dragNormal = Norm(eye - bp);
		gM.stroking = true;
	}

	// Each frame, with the camera's eye and the ray under the mouse: the brush.
	// forced: -1 the real mouse; 0 or 1 a simulated button (--paint-stroke, for testing), pressed on the first 1.
	static void Mouse(V3 eye, V3 dir, int forced = -1)
	{
		gM.hit = false;
		if (!Painting()) { gM.stroking = false; return; }
		// Ctrl+Z: the last edit undone; Ctrl+Y: redone (not mid-stroke, nor while typing in a field).
		if (forced < 0 && !gM.stroking && ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput) {
			if (ImGui::IsKeyPressed(ImGuiKey_Z, true)) Undo();
			else if (ImGui::IsKeyPressed(ImGuiKey_Y, true)) Redo();
		}
		ImGuiIO& io = ImGui::GetIO();
		bool overUi = io.WantCaptureMouse && !gM.stroking && forced < 0;
		if (gM.tool == 6) { MovePoints(eye, dir, overUi, forced); return; }
		int only = -1;
		if (gM.stroking && gM.tool >= 4) only = ShapeIndex(gM.maskShape);
		else if (gM.stroking && gM.selected >= 0) only = ShapeIndex(gM.chains[gM.selected].shape);
		int shape, nearV;
		V3 p, n;
		if (!overUi && Raycast(eye, dir, only, shape, p, n, nearV)) { gM.hit = true; gM.hitPoint = p; gM.hitNormal = n; gM.hitShape = shape; }
		// Ctrl+click with Mask / Unmask: the whole element under the mouse (see MaskElement), one undo step.
		if (gM.tool >= 4 && forced < 0 && !gM.stroking && io.KeyCtrl && gM.hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			const std::string name = gItem.shapes[shape].info.geometry->m_name ? gItem.shapes[shape].info.geometry->m_name : "";
			PushUndo();
			const int count = MaskElement(name, gItem.shapes[shape], nearV, gM.tool == 5);
			AddLog(2, std::string(gM.tool == 5 ? "unmasked" : "masked") + " an element of " + name + ": " + std::to_string(count) + " vertices");
			return;
		}
		bool down = forced >= 0 ? forced == 1 : ImGui::IsMouseDown(ImGuiMouseButton_Left) && !io.KeyCtrl;
		bool clicked = forced >= 0 ? forced == 1 : ImGui::IsMouseClicked(ImGuiMouseButton_Left);
		if (!gM.stroking) {
			if (!down || !clicked || !gM.hit) return;
			std::string name = gItem.shapes[shape].info.geometry->m_name ? gItem.shapes[shape].info.geometry->m_name : "";
			if (gM.tool >= 4) {
				PushUndo();
				gM.maskShape = name;
			} else if (gM.tool == 0) {
				PushUndo();
				PaintChain c;
				c.shape = name;
				gM.chains.push_back(c);
				gM.selected = (int)gM.chains.size() - 1;
				gM.point = -1;
			} else if (gM.selected < 0 || gM.chains[gM.selected].shape != name) {
				AddLog(3, gM.selected < 0 ? "select a chain to add to or erase from" : "the selected chain is on " + gM.chains[gM.selected].shape + ": paint on that shape");
				return;
			} else PushUndo();
			gM.stroking = true;
		}
		if (gM.tool >= 4) {
			int mi = ShapeIndex(gM.maskShape);
			if (mi >= 0 && gM.hit) MaskBrush(gM.maskShape, gItem.shapes[mi], gM.hitPoint, gM.tool == 5);
			if (!down || mi < 0) gM.stroking = false;
			return;
		}
		PaintChain& c = gM.chains[gM.selected];
		int si = ShapeIndex(c.shape);
		if (si < 0) { gM.stroking = false; return; }
		const ShapeDraw& s = gItem.shapes[si];
		if (gM.hit) {
			if (gM.tool == 3) SmoothBrush(c, s, gM.hitPoint);
			else Brush(c, s, gM.hitPoint, gM.tool == 2);
			// The stroke: a vertex every quarter brush, in the order painted.
			if (gM.tool == 0) {
				if (c.path.empty()) c.path.push_back(nearV);
				else {
					V3 d = VertexAt(s, nearV) - VertexAt(s, c.path.back());
					if (nearV != c.path.back() && Dot(d, d) > gM.radius * gM.radius * 0.0625f) c.path.push_back(nearV);
				}
			}
		}
		if (!down) {
			gM.stroking = false;
			if (gM.tool == 0) {
				if (c.path.size() < 2 || c.paint.empty()) {
					gM.chains.erase(gM.chains.begin() + gM.selected);
					gM.selected = gM.chains.empty() ? -1 : (int)gM.chains.size() - 1;
					if (!gM.undo.empty()) gM.undo.pop_back();   // nothing was made: no step to undo
					AddLog(3, "the stroke was too short for a chain: drag along it, from where it is tied on to its end");
					return;
				}
				c.anchor = GuessAnchor(s, c.path);
				c.links = std::clamp((int)std::lround(PathLength(s, c.path) / 8.0f), 2, 8);
				AddLog(2, "chain " + std::to_string(gM.selected + 1) + " on " + c.shape + ": " + std::to_string(c.paint.size()) + " vertices, " +
					std::to_string(c.links) + " links, hangs from " + c.anchor);
			}
		}
	}

	// Draws a shape with per-vertex colours: the painted chains over a grey surface, the hovered shape in white.
	static bool DrawOverlay(const ShapeDraw& s)
	{
		if (!gM.open || gM.source.empty() || gItem.nifPath != gM.source) return false;
		std::string name = s.info.geometry->m_name ? s.info.geometry->m_name : "";
		bool hovered = !gHighlightShape.empty() && gHighlightShape == name;
		bool any = hovered;
		for (auto& c : gM.chains) if (c.shape == name) any = true;
		if (gM.masks.count(name)) any = true;
		if (!any && !(gM.paintTab && gM.paintOn)) return false;
		std::vector<float> r(s.verts.size(), 0.62f), g(s.verts.size(), 0.62f), b(s.verts.size(), 0.64f);
		if (hovered) std::fill(r.begin(), r.end(), 0.95f), std::fill(g.begin(), g.end(), 0.95f), std::fill(b.begin(), b.end(), 0.95f);
		// The selected chain's shape: its paint as a weights heat map (as the rig previewer's: dark blue none, blue little,
		// yellow half, red all), masked vertices dark grey. Other shapes, or no chain selected: each chain in its colour.
		const bool heat = gM.selected >= 0 && gM.selected < (int)gM.chains.size() && gM.chains[gM.selected].shape == name && !gM.chains[gM.selected].hidden;
		if (heat) {
			// With a point selected: that point's own weights (what moving it changes), redone every frame.
			// (2026-11-14 user: the paint alone is all red where painted fully; "the preview should show the weight value of
			// every point".) No point selected: every point's weights at once, each vertex its largest weight on any one point
			// (red at the points, yellow halfway between two where the weight splits, blue where the paint is soft; the
			// points' weights add up to the paint, so a sum would be red again). The paint alone while the chain has no
			// points yet (a new stroke still being drawn).
			// (2026-11-14) Several points picked (Move points, Ctrl+click) and the selected one among them: each picked point's
			// own weights (every picked point on this shape, of any chain), each vertex its largest weight on any one of them
			// (user: a sum turned neighbours all red; "the accurate representation of the points").
			const PaintChain& c = gM.chains[gM.selected];
			const bool one = gM.point >= 0 && gM.point <= c.links;
			const bool several = one && gM.group.size() > 1 && InGroup(gM.selected, gM.point);
			std::vector<float> pw;
			if (several) {
				pw.assign(s.verts.size(), 0.0f);
				for (auto& gp : gM.group) {
					if (!GroupValid(gp) || gM.chains[gp.first].shape != name || gM.chains[gp.first].hidden) continue;
					const std::vector<float> w = PointWeights(s, gM.chains[gp.first], gp.second);
					for (size_t v = 0; v < pw.size(); v++) pw[v] = std::max(pw[v], w[v]);
				}
			}
			else if (one) pw = PointWeights(s, c, gM.point);
			else if (c.links >= 1) {
				pw.assign(s.verts.size(), 0.0f);
				for (int k = 0; k <= c.links; k++) { const std::vector<float> w = PointWeights(s, c, k); for (size_t v = 0; v < pw.size(); v++) pw[v] = std::max(pw[v], w[v]); }
			}
			for (size_t v = 0; v < s.verts.size(); v++) {
				D3DCOLOR col = Heat(!pw.empty() ? pw[v] : PaintAt(c, (int)v));
				r[v] = ((col >> 16) & 255) / 255.0f; g[v] = ((col >> 8) & 255) / 255.0f; b[v] = (col & 255) / 255.0f;
			}
			if (auto it = gM.masks.find(name); it != gM.masks.end())
				for (int v : it->second)
					if (v < (int)s.verts.size()) { r[v] = 0.22f; g[v] = 0.22f; b[v] = 0.22f; }
		}
		for (size_t ci = 0; ci < gM.chains.size() && !heat; ci++) {
			auto& c = gM.chains[ci];
			if (c.shape != name || c.hidden) continue;
			D3DCOLOR col = ChainColor(ci);
			float cr = ((col >> 16) & 255) / 255.0f, cg = ((col >> 8) & 255) / 255.0f, cb = (col & 255) / 255.0f;
			float boost = (int)ci == gM.selected ? 1.0f : 0.8f;
			for (auto& [v, st] : c.paint) {
				if (v >= (int)s.verts.size()) continue;
				float t = std::clamp(st, 0.0f, 1.0f) * boost;
				r[v] += (cr - r[v]) * t; g[v] += (cg - g[v]) * t; b[v] += (cb - b[v]) * t;
			}
		}
		if (auto it = gM.masks.find(name); !heat && it != gM.masks.end())
			for (int v : it->second)
				if (v < (int)s.verts.size()) { r[v] = r[v] * 0.3f + 0.08f; g[v] = g[v] * 0.3f + 0.12f; b[v] = b[v] * 0.3f + 0.45f; }
		V3 key = Norm(V3(0.35f, 0.6f, 0.7f));
		std::vector<LineVertex> lv(s.verts.size());
		for (size_t i = 0; i < s.verts.size(); i++) {
			auto& m = s.verts[i];
			V3 nn(m.nx, m.ny, m.nz);
			float l = std::sqrt(Dot(nn, nn));
			float shade = l > 1e-6f ? 0.5f + 0.5f * std::fabs(Dot(nn, key) / l) : 1.0f;
			auto c8 = [&](float x) { return (int)std::clamp(x * shade * 255.0f, 0.0f, 255.0f); };
			lv[i] = { m.px, m.py, m.pz, D3DCOLOR_ARGB(255, c8(r[i]), c8(g[i]), c8(b[i])) };
		}
		gDev->SetVertexShader(nullptr);
		gDev->SetPixelShader(nullptr);
		gDev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
		gDev->SetTexture(0, nullptr);
		gDev->SetRenderState(D3DRS_LIGHTING, FALSE);
		gDev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)lv.size(), (UINT)s.indices.size() / 3, s.indices.data(), D3DFMT_INDEX16, lv.data(), sizeof(LineVertex));
		gDev->SetVertexShader(gVS);
		gDev->SetPixelShader(gPS);
		gDev->SetVertexDeclaration(gDecl);
		return true;
	}

	// On top: every chain's stroke and planned bones, and the brush ring under the mouse.
	static void DrawPaths(V3 eye)
	{
		if (!gM.open || gM.source.empty() || gItem.nifPath != gM.source) return;
		std::vector<LineVertex> lines, points;
		for (size_t ci = 0; ci < gM.chains.size(); ci++) {
			auto& c = gM.chains[ci];
			if (c.hidden) continue;
			int si = ShapeIndex(c.shape);
			if (si < 0) continue;
			const ShapeDraw& s = gItem.shapes[si];
			D3DCOLOR col = (int)ci == gM.selected ? D3DCOLOR_ARGB(255, 255, 255, 255) : ChainColor(ci);
			auto bones = PlannedBones(s, c);
			for (size_t k = 0; k < bones.size(); k++) {
				// (Move points' group, Ctrl+click: yellow)
				points.push_back({ bones[k].x, bones[k].y, bones[k].z, gM.tool == 6 && gM.group.size() > 1 && InGroup((int)ci, (int)k) ? D3DCOLOR_ARGB(255, 255, 230, 40) :
					c.points.count((int)k) ? D3DCOLOR_ARGB(255, 90, 255, 255) : k == 0 ? D3DCOLOR_ARGB(255, 150, 160, 180) : col });
				if (k) { lines.push_back({ bones[k - 1].x, bones[k - 1].y, bones[k - 1].z, col }); lines.push_back({ bones[k].x, bones[k].y, bones[k].z, col }); }
			}
			// The selected point: a yellow cross and its reach as a ring facing the camera.
			if ((int)ci == gM.selected && gM.point >= 0 && gM.point < (int)bones.size()) {
				const D3DCOLOR yellow = D3DCOLOR_ARGB(255, 255, 230, 40);
				V3 p = bones[gM.point], view = Norm(eye - p);
				V3 u = Norm(Cross(view, std::fabs(view.z) < 0.9f ? V3(0, 0, 1) : V3(1, 0, 0))), w = Cross(view, u);
				float arm = std::max(0.6f, gM.reach * 0.15f);
				for (V3 a : { u, w }) { V3 p0 = p - a * arm, p1 = p + a * arm; lines.push_back({ p0.x, p0.y, p0.z, yellow }); lines.push_back({ p1.x, p1.y, p1.z, yellow }); }
				const int seg = 40;
				for (int i = 0; i < seg; i++) {
					float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
					V3 q0 = p + (u * std::cos(a0) + w * std::sin(a0)) * gM.reach, q1 = p + (u * std::cos(a1) + w * std::sin(a1)) * gM.reach;
					lines.push_back({ q0.x, q0.y, q0.z, yellow });
					lines.push_back({ q1.x, q1.y, q1.z, yellow });
				}
			}
		}
		if (gM.hit && Painting()) {
			V3 n = gM.hitNormal, view = Norm(eye - gM.hitPoint);
			if (Dot(n, view) < 0) n = n * -1.0f;
			V3 u = Norm(Cross(n, std::fabs(n.z) < 0.9f ? V3(0, 0, 1) : V3(1, 0, 0))), w = Cross(n, u);
			D3DCOLOR ring = gM.tool == 2 ? D3DCOLOR_ARGB(255, 255, 90, 90) : gM.tool == 3 ? D3DCOLOR_ARGB(255, 80, 220, 255) :
			                gM.tool == 4 ? D3DCOLOR_ARGB(255, 70, 110, 255) : gM.tool == 5 ? D3DCOLOR_ARGB(255, 190, 190, 190) : D3DCOLOR_ARGB(255, 255, 255, 255);
			const int seg = 32;
			for (int i = 0; i < seg; i++) {
				float a0 = 6.2831853f * i / seg, a1 = 6.2831853f * (i + 1) / seg;
				V3 p0 = gM.hitPoint + (u * std::cos(a0) + w * std::sin(a0)) * gM.radius, p1 = gM.hitPoint + (u * std::cos(a1) + w * std::sin(a1)) * gM.radius;
				lines.push_back({ p0.x, p0.y, p0.z, ring });
				lines.push_back({ p1.x, p1.y, p1.z, ring });
			}
		}
		DrawLines(lines, D3DPT_LINELIST, true);
		DrawLines(points, D3DPT_POINTLIST, true);
	}

	static void PaintTab()
	{
		ImGui::TextWrapped("Paint where something should hang and swing: start the stroke where it is tied on and drag to its end. Each stroke becomes a chain of bones along it; what you paint moves with it, soft brush edges blending into the mesh's own weights.");
		if (gM.source.empty()) return;
		if (gItem.nifPath != gM.source) {
			ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "The view shows another NIF.");
			if (ImGui::Button("Back to the source to paint")) LoadItem(gM.source);
			return;
		}
		ImGui::Checkbox("Paint (the body holds still)", &gM.paintOn);
		Help("Left drag on the mesh paints. Right drag orbits, middle drag pans, the wheel zooms, as always.");
		ImGui::Checkbox("Show the NIF's pose", &gM.nifPose);
		Help("While painting, the item as it was modelled (what the build reads), not the animation's frame. Off: the pose the animation was in.");
		ImGui::RadioButton("New chain", &gM.tool, 0); ImGui::SameLine();
		ImGui::RadioButton("Add to selected", &gM.tool, 1); ImGui::SameLine();
		ImGui::RadioButton("Erase", &gM.tool, 2); ImGui::SameLine();
		ImGui::RadioButton("Smooth", &gM.tool, 3);
		ImGui::RadioButton("Mask", &gM.tool, 4); ImGui::SameLine();
		ImGui::RadioButton("Unmask", &gM.tool, 5); ImGui::SameLine();
		ImGui::RadioButton("Move points", &gM.tool, 6); ImGui::SameLine();
		ImGui::BeginDisabled(gM.masks.empty());
		if (ImGui::SmallButton("Clear mask")) { PushUndo(); gM.masks.clear(); }
		ImGui::EndDisabled();
		ImGui::SameLine();
		const std::string invertOn = gM.selected >= 0 ? gM.chains[gM.selected].shape : gM.hitShape >= 0 && gM.hitShape < (int)gItem.shapes.size() && gItem.shapes[gM.hitShape].info.geometry->m_name ? gItem.shapes[gM.hitShape].info.geometry->m_name : "";
		ImGui::BeginDisabled(invertOn.empty() || ShapeIndex(invertOn) < 0);
		if (ImGui::SmallButton("Invert mask")) {
			PushUndo();
			auto& m = gM.masks[invertOn];
			std::set<int> inv;
			for (int v = 0; v < (int)gItem.shapes[ShapeIndex(invertOn)].verts.size(); v++) if (!m.count(v)) inv.insert(v);
			m = std::move(inv);
			if (m.empty()) gM.masks.erase(invertOn);
		}
		ImGui::EndDisabled();
		Help("Mask: brush over vertices to protect them (dark blue): no brush, smoothing or grow / fade changes their paint. Unmask frees them. Ctrl+click with either: the whole element under the mouse (the connected piece of the mesh) at once. Invert: on the selected chain's shape (or the last one under the brush), masks what is free and frees what is masked, to work only on a part. Move points: drag a chain's point (its bone) to where it should be; the mesh stays, and builds weighted to the bones where they are (moved points show cyan). Ctrl+click points to pick several (yellow), then drag any of them: they all move together; a click on empty space lets them go.");
		Help("New chain: each stroke is a new chain, laid along the stroke. Add: paints more of the mesh onto the selected chain (its bones stay). Erase: takes paint off the selected chain. Smooth: evens out the selected chain's paint under the brush (hard edges blend, spots fill in).");
		ImGui::BeginDisabled(gM.undo.empty());
		if (ImGui::Button("Undo")) Undo();
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::TextDisabled("Ctrl+Z / Ctrl+Y (%zu to undo, %zu to redo)", gM.undo.size(), gM.redo.size());
		Help("Undo: the last stroke, smoothing, grow or fade, mask, deleted chain or loaded recipe; the last 50 are kept. Ctrl+Y redoes what Undo took back, until the next edit.");
		ImGui::SliderFloat("Brush size", &gM.radius, 0.5f, 30, "%.1f units");
		ImGui::SliderFloat("Brush strength", &gM.strength, 0.05f, 1, "%.2f");
		Help("Below 1 the paint only partly follows the chain: a softer blend into the mesh's own weights.");

		ImGui::SeparatorText("Chains");
		if (gM.chains.empty()) ImGui::TextDisabled("None yet: paint a stroke on the mesh.");
		for (size_t i = 0; i < gM.chains.size(); i++) {
			auto& c = gM.chains[i];
			ImGui::PushID((int)i);
			// Its square: a click hides or shows the whole chain in the view (lines, points, paint); hidden, it is dim.
			D3DCOLOR col = ChainColor(i);
			const float dim = c.hidden ? 0.25f : 1.0f;
			if (ImGui::ColorButton("##c", ImVec4(((col >> 16) & 255) / 255.0f * dim, ((col >> 8) & 255) / 255.0f * dim, (col & 255) / 255.0f * dim, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(14, 14)))
				c.hidden = !c.hidden;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(c.hidden ? "Hidden: click to show this chain" : "Click to hide this chain (its lines, points and paint)");
			ImGui::SameLine();
			char label[160];
			snprintf(label, sizeof label, "Chain %zu on %s (%zu vertices)", i + 1, c.shape.c_str(), c.paint.size());
			if (ImGui::Selectable(label, gM.selected == (int)i)) { if (gM.selected != (int)i) gM.point = -1; gM.selected = (int)i; }
			if (gM.selected == (int)i) {
				ImGui::Indent();
				int si = ShapeIndex(c.shape);
				std::vector<std::string> anchors = gM.skeleton;
				BoneCombo("Hangs from", c.anchor, anchors);
				Help("The bone the chain is tied to. Picked from what the start of the stroke was weighted to; change it if the chain should follow another part of the body.");
				if (ImGui::SliderInt("Links", &c.links, 1, 12)) c.points.clear();   // other points now: moved ones go back on the stroke
				if (si >= 0) { ImGui::SameLine(); ImGui::TextDisabled("%.1f units each", PathLength(gItem.shapes[si], c.path) / std::max(1, c.links)); }
				Help("Bones along the stroke. More bends more smoothly; about one per 6-10 units.");
				ImGui::SliderFloat("Stiffness", &c.stiffness, 0, 1, "%.2f");
				Help("0 swings freely like a rope or a tassel; 0.4 a strap or a flap; 0.8 and up holds its modelled shape and only bounces (a stiff collar, a holster flap).");
				ImGui::SliderFloat("Weight", &c.weight, 0, 1, "%.2f");
				Help("0 light and lively (ribbons, hair; the wind moves it); 1 heavy and calm (leather, chain, a weighted end).");
				// (2026-11-14 user: every chain with the same number of points)
				ImGui::BeginDisabled(gM.chains.size() < 2);
				if (ImGui::SmallButton("Same number of points on every chain")) {
					PushUndo();
					int changed = 0, relaid = 0;
					for (auto& o : gM.chains) {
						if (&o == &c || o.links == c.links) continue;
						o.links = c.links;   // as the Links slider: other points now, moved ones back on the stroke
						if (!o.points.empty()) relaid++;
						o.points.clear();
						changed++;
					}
					AddLog(2, std::to_string(changed) + " chain(s) now have " + std::to_string(c.links) + " links (" + std::to_string(c.links + 1) + " points with the tie-on)" +
						(relaid ? "; " + std::to_string(relaid) + " of them had moved points, put back on the stroke" : ""));
				}
				ImGui::EndDisabled();
				Help("Gives every other chain as many points as this one (its Links); stiffness, weight and what each hangs from stay their own. A chain whose count changes gets new points along its stroke, as with the Links slider: its moved points go back on the stroke. Ctrl+Z undoes it.");

				// Weights: how much of the mesh follows the chain (the paint), evened out or grown and faded by its points.
				ImGui::SeparatorText("Weights");
				{
					// The heat map's scale (the shape shows this chain's paint in it while the chain is selected, or the
					// selected point's weights).
					ImDrawList* dl = ImGui::GetWindowDrawList();
					ImVec2 p = ImGui::GetCursorScreenPos();
					const float w = ImGui::GetContentRegionAvail().x, h = 8;
					for (int k = 0; k < 64; k++) {
						D3DCOLOR col = Heat(k / 63.0f);
						dl->AddRectFilled(ImVec2(p.x + w * k / 64, p.y), ImVec2(p.x + w * (k + 1) / 64 + 1, p.y + h), IM_COL32((col >> 16) & 255, (col >> 8) & 255, col & 255, 255));
					}
					ImGui::Dummy(ImVec2(w, h));
					ImGui::TextDisabled("none   little (blue)   half (yellow)   all (red)");
					ImGui::TextDisabled(gM.point >= 0 && gM.group.size() > 1 && InGroup((int)i, gM.point) ? "Showing each picked point's weights (red at a point, yellow where two share a vertex)." :
						gM.point >= 0 ? "Showing the selected point's weights." :"Showing every point's weights (red at a point, yellow where two share a vertex); select a point to see only its own.");
				}
				if (si >= 0 && ImGui::Button("Smooth the whole chain")) {
					PushUndo();
					SmoothChain(c, gItem.shapes[si]);
					AddLog(2, "chain " + std::to_string(i + 1) + " smoothed: " + std::to_string(c.paint.size()) + " vertices");
				}
				Help("Evens out the chain's paint everywhere at once (each click a bit more). The Smooth brush does it where you paint.");
				if (gM.point > c.links) gM.point = -1;
				ImGui::TextUnformatted("Point:");
				for (int k = 0; k <= c.links; k++) {
					ImGui::SameLine();
					char pl[24];
					if (k == 0) snprintf(pl, sizeof pl, "Tie-on##p0");
					else snprintf(pl, sizeof pl, "%d##p%d", k, k);
					if (ImGui::RadioButton(pl, gM.point == k)) gM.point = gM.point == k ? -1 : k;
				}
				Help("Pick one of the chain's points (drawn yellow on the mesh, its reach as a ring), then grow or fade how much of the mesh around it follows the chain.");
				ImGui::BeginDisabled(si < 0 || gM.point < 1 || c.links < 2);
				if (ImGui::SmallButton("Delete point")) { PushUndo(); DeletePoint(c, gItem.shapes[si], gM.point); gM.point = -1; }
				ImGui::EndDisabled();
				Help("Takes the selected point (and its bone) out of the chain: one link fewer. The other points stay where they are, so they count as moved. Not the tie-on, nor the last link.");
			ImGui::BeginDisabled(si < 0 || c.links < 2);
			if (ImGui::SmallButton("Smooth points")) { PushUndo(); RelaxPoints(c, gItem.shapes[si], false); }
			ImGui::SameLine();
			if (ImGui::SmallButton("Even spacing")) { PushUndo(); RelaxPoints(c, gItem.shapes[si], true); }
			ImGui::EndDisabled();
			Help("Moves the chain's points, not its paint; the tie-on and the end stay. Smooth points: each middle point halfway to the middle of its neighbours (zig-zags iron out; click again for more, the chain straightens). Even spacing: the middle points spread at equal distances along the chain's path, its shape kept. Ctrl+Z undoes either.");
				if (!c.points.empty()) {
					ImGui::BeginDisabled(gM.point < 0 || !c.points.count(gM.point));
					if (ImGui::SmallButton("Reset point")) { PushUndo(); c.points.erase(gM.point); }
					ImGui::EndDisabled();
					ImGui::SameLine();
					if (ImGui::SmallButton("Reset all points")) { PushUndo(); c.points.clear(); }
					ImGui::SameLine();
					ImGui::TextDisabled("%zu moved", c.points.size());
				}
				ImGui::SliderFloat("Reach", &gM.reach, 1, 40, "%.1f units");
				ImGui::SliderFloat("Amount", &gM.amount, 0.05f, 1, "%.2f");
				Help("Reach: how far from the point it works, fading to nothing at the edge. Amount: how much each click does at the point itself.");
				ImGui::BeginDisabled(gM.point < 0 || si < 0);
				const bool grow = ImGui::Button("Grow influence");
				ImGui::SameLine();
				const bool fade = ImGui::Button("Fade influence");
				ImGui::EndDisabled();
				if ((grow || fade) && si >= 0 && gM.point >= 0) {
					PushUndo();
					auto points = PlannedBones(gItem.shapes[si], c);
					if (gM.point < (int)points.size()) {
						GrowAround(c, gItem.shapes[si], points[gM.point], grow);
						AddLog(2, std::string("chain ") + std::to_string(i + 1) + (grow ? ": influence grown" : ": influence faded") + " around point " +
							(gM.point == 0 ? std::string("tie-on") : std::to_string(gM.point)) + " (" + std::to_string(c.paint.size()) + " vertices)");
					}
				}
				Help("Grow: more of the mesh around the point follows the chain (vertices not painted yet are taken in). Fade: less of it does, blending back into the mesh's own weights.");
				if (ImGui::Button("Delete chain")) { PushUndo(); gM.chains.erase(gM.chains.begin() + i); gM.selected = -1; ImGui::Unindent(); ImGui::PopID(); break; }
				ImGui::Unindent();
			}
			ImGui::PopID();
		}
		if (!gM.chains.empty() && ImGui::SmallButton("Delete all chains")) { PushUndo(); gM.chains.clear(); gM.selected = -1; }

		ImGui::SeparatorText("Collides with");
		std::set<std::string> painted;
		for (auto& c : gM.chains) painted.insert(c.shape);
		for (auto& s : gM.shapes) {
			if (painted.count(s.name)) continue;
			bool on = s.role == Collider;
			if (ImGui::Checkbox(s.name.c_str(), &on)) s.role = on ? Collider : None;
			if (ImGui::IsItemHovered()) gHighlightShape = s.name;
		}
		ImGui::Checkbox("Low-poly collision proxies", &gM.proxy);
		Help("The colliders collide through simplified copies hidden in the NIF: far cheaper in game. The margins and penetration keep their defaults.");
		OutputRows();
		BuildButtons(false);
	}

	// --- the window ------------------------------------------------------------------------------------------------

	// A bone picked from a list (with a filter box).
	static bool BoneCombo(const char* label, std::string& bone, const std::vector<std::string>& bones)
	{
		bool changed = false;
		if (ImGui::BeginCombo(label, bone.empty() ? "(pick a bone)" : bone.c_str(), ImGuiComboFlags_HeightLarge)) {
			static char filter[64] = "";
			if (ImGui::IsWindowAppearing()) { filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
			ImGui::InputTextWithHint("##filter", "filter", filter, sizeof filter);
			std::string f = Lower(filter);
			for (auto& b : bones) {
				if (!f.empty() && Lower(b).find(f) == std::string::npos) continue;
				if (ImGui::Selectable(b.c_str(), b == bone)) { bone = b; changed = true; }
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	static void PathRow(const char* label, char* buf, size_t n, bool folder, const char* help)
	{
		ImGui::PushID(label);
		ImGui::SetNextItemWidth(-70);
		ImGui::InputText(label, buf, n);
		ImGui::SameLine();
		if (ImGui::Button("Pick...")) {
			std::string p = folder ? PickFolder() : PickFile("NIF meshes (*.nif)\0*.nif\0", buf[0] ? fs::path(buf).parent_path().string() : std::string());
			if (!p.empty()) Copy(buf, n, p);
		}
		if (help) Help(help);
		ImGui::PopID();
	}

	static void OutputRows()
	{
		ImGui::SeparatorText("Output");
		ImGui::SetNextItemWidth(-70);
		ImGui::InputText("Physics file", gM.physics, sizeof gM.physics);
		Help("Where the physics XML will be in the game, relative to Data (e.g. Meshes\\Armor\\MyOutfit\\outfit_smp.xml). The NIF names it, so it must match where you install it, or the game logs \"physics file not found\".");
		PathRow("Output folder", gM.outDir, sizeof gM.outDir, true, "A folder laid out as Data: the NIF and XML go in its Meshes\\... folder. Pick an empty mod folder in MO2's mods to install it straight away.");
		ImGui::TextDisabled("NIF: %s", OutNif().c_str());
		if (gM.mode == Coat || !gM.chains.empty() || std::any_of(gM.shapes.begin(), gM.shapes.end(), [](const ShapeRow& s) { return s.role == Cloth || s.role == Chains; }))
			ImGui::TextDisabled("XML: %s", OutXml().c_str());
		if (fs::exists(OutXml())) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "The XML exists: building replaces it (the old one is kept as .bak),\nincluding values tuned in Bone values.");
	}

	static void BuildButtons(bool repair)
	{
		ImGui::Spacing();
		ImGui::BeginDisabled(gM.running);
		if (ImGui::Button(gM.running ? "Building..." : "Build and preview", ImVec2(180, 0))) { if (repair) BuildRepair(); else BuildOutfit(); }
		ImGui::EndDisabled();
		ImGui::SameLine();
		ImGui::BeginDisabled(gM.lastCommand.empty());
		if (ImGui::Button("Copy command")) ImGui::SetClipboardText(gM.lastCommand.c_str());
		ImGui::EndDisabled();
		Help("The last build's command line, for running the tool yourself.");
		if (!gM.lastResult.empty()) ImGui::TextWrapped("Last build: %s", gM.lastResult.c_str());
		// The bone values saved from Bone values on the output, put back after each build (RecordValues / ApplyKeptValues).
		if (!repair && !gM.source.empty()) {
			std::error_code ec;
			const std::string vf = OutXml() + ".smpvalues";
			if (fs::exists(vf, ec)) {
				ImGui::Text("Kept bone values: %zu, put back after every build", ReadKeptValues(OutXml()).size());
				ImGui::SameLine();
				if (ImGui::SmallButton("Forget them")) { fs::remove(vf, ec); AddLog(2, "the kept bone values are forgotten: the next build keeps its own"); }
				Help("Values changed in Bone values and saved to the XML on the built NIF (bone and constraint settings, chain sliders, removed constraints, collision tags, impact sounds), as changes from what the build wrote. The next build puts them back by name; the build's own values stay wherever you did not change them.");
			}
		}
		// The edits made on the output in the previewer, done again after each build (RecordEdit / ReplayNext).
		if (!repair && !gM.edits.empty()) {
			const std::string recipe = OutXml() + ".smprecipe";
			bool changed = false;
			if (ImGui::TreeNode("kept", "Kept edits (%zu): done again after every build", gM.edits.size())) {
				for (size_t i = 0; i < gM.edits.size(); i++) {
					ImGui::PushID((int)i);
					if (ImGui::SmallButton("x")) { gM.edits.erase(gM.edits.begin() + i); changed = true; ImGui::PopID(); break; }
					ImGui::SameLine();
					ImGui::TextUnformatted(gM.edits[i].c_str());
					ImGui::PopID();
				}
				if (ImGui::SmallButton("Forget them all")) { gM.edits.clear(); changed = true; }
				ImGui::TreePop();
			}
			Help("Proxies generated, shapes marked as proxies or deleted, and the XML's collision shapes added, removed or pointed elsewhere, on the built NIF in the previewer: the next build from the source does them again, in this order. x: not any more.");
			std::error_code ec;
			if (changed && fs::exists(recipe, ec)) std::ofstream(recipe) << RecipeText();
		}
	}

	static void Window()
	{
		Pump();
		if (gM.buildOnOpen && !gM.running && !gM.source.empty()) { gM.buildOnOpen = false; if (gM.mode == Repair) BuildRepair(); else BuildOutfit(); }
		if (!gM.open) return;
		// A plain NIF opened in the view becomes the source (unless chains are painted on the current one).
		if (gItem.loaded && gItem.nifPath != gM.source && !gM.running && (gM.source.empty() || (gM.chains.empty() && gItem.scene.physicsFile.empty()))) Capture();
		// An SMP NIF the Maker built (its recipe beside its physics XML): its chains are brought back to edit, once, when none
		// are open (so the result a build opens stays): the recipe reopens the original NIF with them; building replaces the
		// SMP version.
		static std::string recipeTried;
		if (gItem.loaded && !gM.running && !gItem.scene.physicsFile.empty() && gItem.nifPath != recipeTried && gM.chains.empty()) {
			recipeTried = gItem.nifPath;
			const std::string recipe = RecipeOf(gItem.xmlPath);
			if (!recipe.empty()) { AddLog(2, "this NIF was built by the Maker: its chains are loaded to edit (from " + recipe + ")"); LoadRecipe(recipe); }
		}
		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 488, 8), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowSize(ImVec2(480, io.DisplaySize.y - 190), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Make SMP", &gM.open)) { ImGui::End(); return; }

		if (ImGui::Button("Use the open NIF")) { if (gItem.loaded) Capture(); }
		Help("Reads the shapes and bones of the NIF open in the previewer (open the plain, non-SMP armour first).");
		ImGui::SameLine();
		if (ImGui::Button("Load recipe...")) {
			std::string p = PickFile("Recipes (*.smprecipe)\0*.smprecipe\0", gM.outDir);
			if (!p.empty()) LoadRecipe(p);
		}
		Help("Every build saves its settings beside the XML (<name>_smp.xml.smprecipe). Load one to rebuild or change it.");
		if (gM.source.empty()) ImGui::TextWrapped("Open the armour in the previewer (Open NIF...), then click Use the open NIF.");
		else {
			ImGui::TextWrapped("Source: %s", gM.source.c_str());
			if (gM.sourceHasPhysics) {
				const std::string recipe = gItem.nifPath == gM.source ? RecipeOf(gItem.xmlPath) : std::string();
				if (!recipe.empty()) {
					ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "Built by the Maker: its chains can be edited.");
					if (ImGui::Button("Edit its chains")) LoadRecipe(recipe);
					Help("Loads its recipe: the original NIF opens with the chains, their points and the settings; Build makes the SMP version again (the old one kept as .bak).");
				} else
					ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "This NIF already has SMP physics, not made in the Maker (no recipe beside\nits XML). Build from the original armour; collision proxies can be added\nfrom the Shapes list in the main window.");
			}
		}

		if (ImGui::BeginTabBar("modes")) {
			gM.paintTab = false;
			if (ImGui::BeginTabItem("Paint chains")) {
				gM.mode = Outfit;
				gM.paintTab = true;
				PaintTab();
				ImGui::EndTabItem();
			}
			if (ImGui::BeginTabItem("Fix after Outfit Studio")) {
				gM.mode = Repair;
				ImGui::TextWrapped("Outfit Studio drops the SMP bones no vertex is weighted to (the cloth's anchors, a rope's top) when it saves. This puts them back from the NIF as it was before the edit. smprepair.");
				PathRow("Edited NIF", gM.edited, sizeof gM.edited, false, "The NIF as Outfit Studio saved it.");
				PathRow("Before the edit", gM.reference, sizeof gM.reference, false, "The same NIF before the edit (the build's output, or its .bak).");
				if (gM.edited[0] && !gM.repaired[0]) { fs::path e(gM.edited); Copy(gM.repaired, sizeof gM.repaired, (e.parent_path() / (e.stem().string() + ".repaired.nif")).string()); }
				ImGui::SetNextItemWidth(-70);
				ImGui::InputText("Repaired NIF", gM.repaired, sizeof gM.repaired);
				Help("Where the repaired NIF goes (it may be the edited file itself: that is kept as .bak).");
				BuildButtons(true);

				ImGui::SeparatorText("Missing SMP Bones list");
				ImGui::TextWrapped("For an SMP NIF made elsewhere (a Skyrim body or outfit): New Vegas drops the bones its skeleton lacks, unless each shape lists them in \"SMP Bones\". This writes the list into the NIF: bones the skeleton has are the body's, the others are rebuilt on their parent (or, on the NIF's root, the body bone they share most weight with).");
				PathRow("NIF", gM.listNif, sizeof gM.listNif, false, "The SMP NIF (it is rewritten; the old one is kept as .bak).");
				if (!gM.listSkeleton[0] && !gAnim.skeletonPath.empty()) Copy(gM.listSkeleton, sizeof gM.listSkeleton, gAnim.skeletonPath);
				PathRow("Skeleton", gM.listSkeleton, sizeof gM.listSkeleton, false, "The game's skeleton.nif the body is used with (found in your Data folders when the previewer finds one).");
				ImGui::BeginDisabled(gM.running);
				if (ImGui::Button("Add SMP Bones list", ImVec2(180, 0))) AddBoneLists(gM.listNif, gM.listSkeleton);
				ImGui::EndDisabled();
				ImGui::EndTabItem();
			}
			ImGui::EndTabBar();
		}
		ImGui::End();
	}
}
