// NV SMP Prepare: makes SMP NIFs made outside these tools (a Skyrim body or outfit with physics bones) ready for
// NV SMP Physics, without the command line. Double-click it, pick one or more NIFs; for each that has bones the game's
// skeleton lacks it adds what is missing (smplist.h):
//   - "SMP Bones" on the shapes with such bones (New Vegas drops them on attach; the plugin rebuilds them from it);
//   - the physics XML's path on the root ("HDT Skinned Mesh Physics Object"), relative to Data, when there is none:
//     asked for (the XML picked), and the XML copied to where the game will look for it when it is not there;
//   - the keep flags on the geometry the physics reads (the bones' shapes and the XML's collision shapes), else the
//     game drops the vertices.
// The skeleton is found as the previewer finds files (the NIF's Data folder, MO2's enabled mods, the game's Data);
// asked for once otherwise and remembered (NVSMPPrepare.ini beside the exe). Every NIF it changes is kept as .bak.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include "smplist.h"
#include "backup.h"
#include "previewer/datafiles.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

static const char* kTitle = "NV SMP Prepare";
static std::string gIni;

static std::string IniGet(const char* key)
{
	char buf[1024];
	GetPrivateProfileStringA("Prepare", key, "", buf, sizeof buf, gIni.c_str());
	return buf;
}
static void IniSet(const char* key, const std::string& v) { WritePrivateProfileStringA("Prepare", key, v.c_str(), gIni.c_str()); }

static std::string TestEnv(const char* k);
static std::vector<std::string> PickFiles(const char* title, const char* filter, const std::string& startIn, bool multi)
{
	if (!TestEnv("NVSMPPREPARE_XML").empty() && strstr(filter, ".xml")) return { TestEnv("NVSMPPREPARE_XML") };
	std::vector<char> buf(64 * 1024, 0);
	OPENFILENAMEA ofn{ sizeof(ofn) };
	ofn.lpstrTitle = title;
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = buf.data();
	ofn.nMaxFile = (DWORD)buf.size();
	ofn.lpstrInitialDir = startIn.empty() ? nullptr : startIn.c_str();
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | (multi ? OFN_ALLOWMULTISELECT : 0);
	if (!GetOpenFileNameA(&ofn)) return {};
	// One file: the full path. Several: the folder, then each name, zero-separated.
	std::vector<std::string> parts;
	for (const char* p = buf.data(); *p; p += strlen(p) + 1) parts.push_back(p);
	if (parts.size() == 1) return parts;
	std::vector<std::string> out;
	for (size_t i = 1; i < parts.size(); i++) out.push_back((fs::path(parts[0]) / parts[i]).string());
	return out;
}

// Testing without a person: NVSMPPREPARE_LOG (a file) takes the summary and answers every question; NVSMPPREPARE_XML is
// the XML "picked" (no XML: every question answered no).
static std::string TestEnv(const char* k) { char b[1024]; DWORD n = GetEnvironmentVariableA(k, b, sizeof b); return n && n < sizeof b ? std::string(b) : std::string(); }
static bool Ask(const std::string& text)
{
	if (!TestEnv("NVSMPPREPARE_LOG").empty()) return !TestEnv("NVSMPPREPARE_XML").empty();
	return MessageBoxA(nullptr, text.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES;
}

// The Data folder a file sits in (the folder holding its "meshes" folder), or empty.
static std::string DataFolderOf(const std::string& path)
{
	fs::path p = fs::path(path).parent_path();
	for (fs::path q = p; !q.empty() && q != q.root_path(); q = q.parent_path()) {
		std::string n = q.filename().string();
		for (auto& c : n) c = (char)tolower((unsigned char)c);
		if (n == "meshes") return q.parent_path().string();
	}
	return "";
}

static std::string Relative(const std::string& path, const std::string& base)
{
	std::error_code ec;
	auto r = fs::relative(path, base, ec).string();
	if (ec || r.empty() || r.rfind("..", 0) == 0) return "";
	return r;
}

// The shapes an SMP XML collides with (per-vertex-shape, per-triangle-shape, nv-virtual-legs fit).
static std::set<std::string> XmlShapes(const std::string& xmlPath)
{
	std::set<std::string> out;
	std::ifstream in(xmlPath, std::ios::binary);
	std::string s((std::istreambuf_iterator<char>(in)), {});
	std::regex re("<(per-vertex-shape|per-triangle-shape)[^>]*name\\s*=\\s*\"([^\"]+)\"|fit\\s*=\\s*\"([^\"]+)\"");
	for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) out.insert((*it)[2].matched ? (*it)[2].str() : (*it)[3].str());
	return out;
}

static std::string FindSkeleton(const datafiles::Roots& roots)
{
	std::string s = datafiles::Resolve(roots, "meshes\\characters\\_male\\skeleton.nif");
	if (!s.empty()) return s;
	s = IniGet("Skeleton");
	if (!s.empty() && fs::exists(s)) return s;
	MessageBoxA(nullptr, "The game's skeleton (meshes\\characters\\_male\\skeleton.nif) was not found in your Data folders or Mod Organizer mods.\n\nPick the skeleton.nif the body is used with (it decides which bones are the item's own).", kTitle, MB_ICONINFORMATION);
	auto p = PickFiles("The game's skeleton.nif", "skeleton.nif\0skeleton.nif;*.nif\0", "", false);
	if (p.empty()) return "";
	IniSet("Skeleton", p[0]);
	return p[0];
}

// One NIF: what it did, as lines for the summary.
static std::string Prepare(const std::string& nifPath)
{
	std::ostringstream log;
	log << fs::path(nifPath).filename().string() << ":\n";
	datafiles::Roots roots = datafiles::Discover(nifPath, datafiles::Options{});
	std::string skeletonPath = FindSkeleton(roots);
	if (skeletonPath.empty()) { log << "  skipped: no skeleton\n"; return log.str(); }
	nif::File f;
	try { f.load(nifPath); } catch (std::exception& e) { log << "  cannot read it: " << e.what() << "\n"; return log.str(); }
	std::set<std::string> skeleton = smplist::skeletonBones(skeletonPath);

	auto found = smplist::analyse(f, skeleton);
	if (found.empty()) { log << "  no bones beyond the skeleton: nothing to do\n"; return log.str(); }
	bool changed = false;

	// The physics file.
	const std::string dataDir = DataFolderOf(nifPath);
	std::string physics = smplist::physicsPath(f);
	std::string xmlOnDisk;
	if (!physics.empty()) {
		xmlOnDisk = datafiles::Resolve(roots, physics);
		if (xmlOnDisk.empty()) {
			log << "  physics file " << physics << ": not found in your Data folders\n";
			if (Ask(fs::path(nifPath).filename().string() + " names the physics file\n\n" + physics + "\n\nwhich is not in your Data folders. Pick the XML to copy it there?")) {
				auto p = PickFiles("The physics XML", "SMP physics (*.xml)\0*.xml\0", fs::path(nifPath).parent_path().string(), false);
				if (!p.empty()) {
					std::string target = dataDir.empty() ? (fs::path(nifPath).parent_path() / fs::path(physics).filename()).string() : (fs::path(dataDir) / physics).string();
					std::error_code ec;
					fs::create_directories(fs::path(target).parent_path(), ec);
					fs::copy_file(p[0], target, fs::copy_options::overwrite_existing, ec);
					log << (ec ? "  could not copy the XML to " : "  copied the XML to ") << target << "\n";
					if (!ec) xmlOnDisk = target;
					if (dataDir.empty()) log << "  (the NIF is not inside a Data\\Meshes folder: in game the XML must be at Data\\" << physics << ")\n";
				}
			}
		}
	} else {
		log << "  no physics file named\n";
		if (Ask(fs::path(nifPath).filename().string() + " has physics bones but names no physics file (\"HDT Skinned Mesh Physics Object\").\n\nPick its SMP physics XML?")) {
			auto p = PickFiles("The physics XML", "SMP physics (*.xml)\0*.xml\0", fs::path(nifPath).parent_path().string(), false);
			if (!p.empty()) {
				xmlOnDisk = p[0];
				std::string rel = dataDir.empty() ? "" : Relative(p[0], dataDir);
				if (!rel.empty()) physics = rel;   // already under the NIF's Data folder
				else {
					// Beside the NIF in the game's Data, and copied there when the NIF is in a Data folder.
					std::string nifRel = dataDir.empty() ? "" : Relative(fs::path(nifPath).parent_path().string(), dataDir);
					physics = (nifRel.empty() ? std::string("Meshes") : nifRel) + "\\" + fs::path(p[0]).filename().string();
					if (!dataDir.empty()) {
						std::string target = (fs::path(dataDir) / physics).string();
						if (Ask("Copy the XML next to the NIF, as\n\n" + target + "\n\nso the game finds it?")) {
							std::error_code ec;
							fs::copy_file(p[0], target, fs::copy_options::overwrite_existing, ec);
							log << (ec ? "  could not copy the XML to " : "  copied the XML to ") << target << "\n";
						}
					} else log << "  (the NIF is not inside a Data\\Meshes folder: in game the XML must be at Data\\" << physics << ")\n";
				}
				smplist::setPhysicsPath(f, physics);
				log << "  physics file set: " << physics << "\n";
				changed = true;
			}
		}
	}

	// The lists (bones sitting on the NIF's root hang where the XML's constraints say).
	std::map<std::string, std::string> xmlParents;
	if (!xmlOnDisk.empty()) {
		std::ifstream in(xmlOnDisk, std::ios::binary);
		std::stringstream text;
		text << in.rdbuf();
		xmlParents = smplist::constraintParents(text.str(), skeleton);
	}
	auto done = smplist::addLists(f, skeleton, xmlParents);
	for (auto& d : done) {
		log << "  " << d.shape << ": SMP Bones added (";
		for (size_t i = 0; i < d.own.size(); i++) log << (i ? ", " : "") << d.own[i];
		log << ")\n";
	}
	for (auto& r : found) if (r.hadList) log << "  " << r.shape << ": SMP Bones already there\n";
	changed |= !done.empty();

	// The vertices the physics reads.
	std::set<std::string> keep;
	for (auto& r : found) keep.insert(r.shape);
	if (!xmlOnDisk.empty()) for (auto& s : XmlShapes(xmlOnDisk)) keep.insert(s);
	int flags = smplist::keepVertexData(f, keep);
	if (flags) { log << "  kept the vertex data of " << flags << " shape(s) for the physics\n"; changed = true; }

	if (!changed) { log << "  already ready\n"; return log.str(); }
	std::string err, kept = backup::keep(nifPath, &err);
	if (!err.empty()) { log << "  not saved: could not back it up (" << err << ")\n"; return log.str(); }
	try { f.save(nifPath); log << "  saved (the old one kept as " << fs::path(kept).filename().string() << ")\n"; }
	catch (std::exception& e) { log << "  cannot save: " << e.what() << "\n"; }
	return log.str();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
	gIni = (fs::path(exe).parent_path() / "NVSMPPrepare.ini").string();

	// NIFs given on the command line (dropped on the exe), else picked.
	std::vector<std::string> nifs;
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; i < argc; i++) { char b[1024]; WideCharToMultiByte(CP_ACP, 0, argv[i], -1, b, sizeof b, nullptr, nullptr); nifs.push_back(b); }
	LocalFree(argv);
	if (nifs.empty()) nifs = PickFiles("Pick the SMP NIF(s) to prepare", "NIF meshes (*.nif)\0*.nif\0", IniGet("LastFolder"), true);
	if (nifs.empty()) return 0;
	IniSet("LastFolder", fs::path(nifs[0]).parent_path().string());

	std::string summary;
	for (auto& n : nifs) summary += Prepare(n) + "\n";
	if (!TestEnv("NVSMPPREPARE_LOG").empty()) std::ofstream(TestEnv("NVSMPPREPARE_LOG")) << summary;
	else MessageBoxA(nullptr, summary.c_str(), kTitle, MB_ICONINFORMATION);
	return 0;
}
