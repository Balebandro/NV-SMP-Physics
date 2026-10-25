#include "datafiles.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

namespace datafiles
{
	namespace
	{
		std::string Lower(std::string s)
		{
			for (auto& c : s) c = (char)tolower((unsigned char)c);
			return s;
		}

		std::string Normalize(std::string p)
		{
			for (auto& c : p) if (c == '/') c = '\\';
			while (!p.empty() && p[0] == '\\') p.erase(0, 1);
			if (Lower(p).rfind("data\\", 0) == 0) p.erase(0, 5);
			return p;
		}

		bool Exists(const fs::path& p)
		{
			std::error_code ec;
			return fs::exists(p, ec);
		}

		// A value from ModOrganizer.ini: plain, or @ByteArray(...) with doubled backslashes.
		std::string IniValue(const std::string& file, const std::string& key)
		{
			std::ifstream in(file);
			std::string line;
			while (std::getline(in, line)) {
				if (line.rfind(key + "=", 0) != 0) continue;
				std::string v = line.substr(key.size() + 1);
				while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
				if (v.rfind("@ByteArray(", 0) == 0 && v.size() > 12) v = v.substr(11, v.size() - 12);
				std::string out;
				for (size_t i = 0; i < v.size(); i++) {
					if (v[i] == '\\' && i + 1 < v.size() && v[i + 1] == '\\') { out += '\\'; i++; }
					else out += v[i] == '/' ? '\\' : v[i];
				}
				return out;
			}
			return {};
		}

		// An MO2 instance's folders, from its ModOrganizer.ini (MO2's Settings > Paths, %BASE_DIR% expanded).
		struct Instance
		{
			std::string dir, gameName, gamePath, profile, base, mods, profiles, overwrite;
		};

		Instance ReadInstance(const std::string& dir)
		{
			Instance i;
			std::string ini = dir + "\\ModOrganizer.ini";
			i.dir = dir;
			i.gameName = IniValue(ini, "gameName");
			i.gamePath = IniValue(ini, "gamePath");
			i.profile = IniValue(ini, "selected_profile");
			i.base = IniValue(ini, "base_directory");
			if (i.base.empty()) i.base = dir;
			auto path = [&](const char* key, const char* def) {
				std::string v = IniValue(ini, key);
				if (v.empty()) return i.base + "\\" + def;
				size_t at = v.find("%BASE_DIR%");
				if (at != std::string::npos) v.replace(at, 10, i.base);
				return v;
			};
			i.mods = path("mod_directory", "mods");
			i.profiles = path("profiles_directory", "profiles");
			i.overwrite = path("overwrite_directory", "overwrite");
			return i;
		}

		bool IsNewVegas(const Instance& i)
		{
			std::string g = Lower(i.gameName);
			return g.find("new vegas") != std::string::npos || g.find("falloutnv") != std::string::npos ||
			       Exists(fs::path(i.gamePath) / "FalloutNV.exe");
		}

		bool Under(const std::string& path, const std::string& folder)
		{
			if (folder.empty()) return false;
			std::string p = Lower(path), f = Lower(folder);
			for (auto& c : p) if (c == '/') c = '\\';
			for (auto& c : f) if (c == '/') c = '\\';
			if (!f.empty() && f.back() != '\\') f += '\\';
			return p.rfind(f, 0) == 0;
		}

		std::string RegString(HKEY root, const char* key, const char* value)
		{
			char buf[1024];
			DWORD size = sizeof buf;
			if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
			return {};
		}

		// Archives are opened once and kept (their indexes are a few MB at most).
		std::map<std::string, std::shared_ptr<bsa::Archive>> gArchives;

		std::shared_ptr<bsa::Archive> OpenArchive(const std::string& path, std::vector<std::string>& notes)
		{
			auto it = gArchives.find(Lower(path));
			if (it != gArchives.end()) return it->second;
			auto a = std::make_shared<bsa::Archive>();
			std::string err;
			if (!a->Open(path, &err)) {
				notes.push_back(path + ": " + err);
				a = nullptr;
			}
			gArchives[Lower(path)] = a;
			return a;
		}

		// The archives in a folder that can hold meshes, animations, textures or sound effects (an XML's impact sounds).
		// Voice archives are skipped: thousands of entries, nothing the previewer reads.
		void AddArchives(const std::string& folder, Roots& r, bool vanillaLast)
		{
			std::error_code ec;
			if (!fs::is_directory(folder, ec)) return;
			std::vector<std::string> found;
			for (auto& e : fs::directory_iterator(folder, ec)) {
				if (!e.is_regular_file(ec) || Lower(e.path().extension().string()) != ".bsa") continue;
				std::string name = Lower(e.path().filename().string());
				if (name.find("voice") != std::string::npos) continue;
				found.push_back(e.path().string());
			}
			// The game loads the DLCs' archives after its own: they come first here.
			if (vanillaLast)
				std::stable_sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
					bool va = Lower(fs::path(a).filename().string()).rfind("fallout - ", 0) == 0;
					bool vb = Lower(fs::path(b).filename().string()).rfind("fallout - ", 0) == 0;
					return !va && vb;
				});
			for (auto& p : found)
				if (auto a = OpenArchive(p, r.notes)) r.archives.push_back(a);
		}

		std::string CacheFolder()
		{
			char tmp[MAX_PATH];
			GetTempPathA(MAX_PATH, tmp);
			return std::string(tmp) + "NVSMPPreviewer";
		}
	}

	std::string FindGameFolder(std::string* how)
	{
		auto ok = [](const std::string& p) { return !p.empty() && Exists(fs::path(p) / "FalloutNV.exe"); };
		auto found = [&](const std::string& p, const char* source) { if (how) *how = source; return p; };
		for (const char* id : { "1454587428", "1312824873" }) {
			std::string p = RegString(HKEY_LOCAL_MACHINE, (std::string("SOFTWARE\\WOW6432Node\\GOG.com\\Games\\") + id).c_str(), "path");
			if (ok(p)) return found(p, "GOG (registry)");
		}
		std::string p = RegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Bethesda Softworks\\FalloutNV", "Installed Path");
		if (ok(p)) return found(p, "the game's registry key");
		std::string steam = RegString(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath");
		if (steam.empty()) steam = RegString(HKEY_CURRENT_USER, "SOFTWARE\\Valve\\Steam", "SteamPath");
		if (!steam.empty()) {
			std::vector<std::string> libraries = { steam };
			std::ifstream vdf(steam + "\\steamapps\\libraryfolders.vdf");
			std::string line;
			while (std::getline(vdf, line)) {
				size_t k = line.find("\"path\"");
				if (k == std::string::npos) continue;
				size_t a = line.find('"', k + 6), b = a == std::string::npos ? a : line.find('"', a + 1);
				if (b == std::string::npos) continue;
				std::string lib;
				std::string raw = line.substr(a + 1, b - a - 1);
				for (size_t i = 0; i < raw.size(); i++) {
					if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') { lib += '\\'; i++; }
					else lib += raw[i];
				}
				libraries.push_back(lib);
			}
			for (auto& l : libraries) {
				std::string g = l + "\\steamapps\\common\\Fallout New Vegas";
				if (ok(g)) return found(g, "Steam");
			}
		}
		return {};
	}

	Roots Discover(const std::string& nifPath, const Options& options)
	{
		Roots r;
		// The NIF's own Data folder: everything before its "Meshes" folder.
		{
			std::string lower = Lower(nifPath);
			for (auto& c : lower) if (c == '/') c = '\\';
			size_t at = lower.rfind("\\meshes\\");
			if (at != std::string::npos) r.folders.push_back(nifPath.substr(0, at));
		}

		// The MO2 instance: set by hand; else the one the NIF is inside (a portable instance, or a global one's
		// mods folder); else a New Vegas global instance.
		Instance inst;
		bool haveInstance = false;
		if (!options.mo2Instance.empty() && Exists(fs::path(options.mo2Instance) / "ModOrganizer.ini")) {
			inst = ReadInstance(options.mo2Instance);
			haveInstance = true;
			r.mo2How = "set by hand";
		}
		if (!haveInstance) {
			for (fs::path p = fs::path(nifPath).parent_path(); !p.empty() && p != p.root_path(); p = p.parent_path()) {
				if (Exists(p / "ModOrganizer.ini")) {
					inst = ReadInstance(p.string());
					haveInstance = true;
					r.mo2How = "the NIF is inside it";
					break;
				}
			}
		}
		std::vector<Instance> globals;
		char appData[MAX_PATH] = {};
		if (GetEnvironmentVariableA("LOCALAPPDATA", appData, MAX_PATH)) {
			std::error_code ec;
			fs::path g = fs::path(appData) / "ModOrganizer";
			if (fs::is_directory(g, ec))
				for (auto& d : fs::directory_iterator(g, ec))
					if (d.is_directory(ec) && Exists(d.path() / "ModOrganizer.ini")) globals.push_back(ReadInstance(d.path().string()));
		}
		if (!haveInstance)
			for (auto& i : globals)
				if (Under(nifPath, i.mods) || Under(nifPath, i.overwrite)) { inst = i; haveInstance = true; r.mo2How = "the NIF is in its mods folder"; break; }
		if (!haveInstance)
			for (auto& i : globals)
				if (IsNewVegas(i)) { inst = i; haveInstance = true; r.mo2How = "MO2's New Vegas instance"; break; }

		std::vector<std::string> modFolders;
		if (haveInstance) {
			r.mo2Instance = inst.dir;
			std::error_code ec;
			if (fs::is_directory(inst.profiles, ec))
				for (auto& d : fs::directory_iterator(inst.profiles, ec))
					if (d.is_directory(ec)) r.mo2Profiles.push_back(d.path().filename().string());
			r.mo2Profile = !options.mo2Profile.empty() && Exists(fs::path(inst.profiles) / options.mo2Profile) ? options.mo2Profile
			             : !inst.profile.empty() ? inst.profile : "Default";
			if (Exists(inst.overwrite)) r.folders.push_back(inst.overwrite);
			// modlist.txt lists the highest priority first; "+" is enabled.
			std::ifstream list(fs::path(inst.profiles) / r.mo2Profile / "modlist.txt");
			if (!list) r.notes.push_back("no modlist.txt in profile '" + r.mo2Profile + "'");
			std::string line;
			while (std::getline(list, line)) {
				while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
				if (line.size() < 2 || line[0] != '+') continue;
				std::string mod = inst.mods + "\\" + line.substr(1);
				if (Exists(mod)) { r.folders.push_back(mod); modFolders.push_back(mod); }
			}
		} else {
			r.notes.push_back("no Mod Organizer 2 instance found: set one in Data sources if you use MO2");
		}
		for (auto& f : options.extraFolders)
			if (Exists(f)) r.folders.push_back(f);

		// The game.
		std::string game;
		if (!options.gameFolder.empty() && Exists(fs::path(options.gameFolder) / "Data")) { game = options.gameFolder; r.gameHow = "set by hand"; }
		else if (haveInstance && Exists(fs::path(inst.gamePath) / "Data")) { game = inst.gamePath; r.gameHow = "from MO2"; }
		else game = FindGameFolder(&r.gameHow);
		if (!game.empty()) {
			r.gameData = (fs::path(game) / "Data").string();
			r.folders.push_back(r.gameData);
		} else {
			r.notes.push_back("the game's folder was not found: set it in Data sources");
		}

		// No duplicates (the NIF's own folder is usually also an MO2 mod).
		std::vector<std::string> unique;
		for (auto& f : r.folders) {
			bool seen = false;
			for (auto& u : unique) seen |= Lower(u) == Lower(f);
			if (!seen) unique.push_back(f);
		}
		r.folders = unique;

		// Archives: the mods' (by priority), then the game's.
		if (options.useArchives) {
			for (auto& m : modFolders) AddArchives(m, r, false);
			if (!r.gameData.empty()) AddArchives(r.gameData, r, true);
		}
		return r;
	}

	std::string Resolve(const Roots& roots, const std::string& relative, std::string* source)
	{
		if (relative.empty()) return {};
		std::string rel = Normalize(relative);
		for (auto& folder : roots.folders) {
			fs::path p = fs::path(folder) / rel;
			if (Exists(p)) {
				if (source) *source = folder;
				return p.string();
			}
		}
		std::string key = bsa::Normalize(rel);
		for (auto& a : roots.archives) {
			if (!a->Contains(key)) continue;
			fs::path out = fs::path(CacheFolder()) / fs::path(a->Path()).stem() / key;
			std::error_code ec;
			if (!Exists(out) || fs::file_size(out, ec) == 0) {
				auto data = a->Read(key);
				if (data.empty()) continue;
				fs::create_directories(out.parent_path(), ec);
				std::ofstream(out, std::ios::binary).write((const char*)data.data(), data.size());
			}
			if (source) *source = a->Path();
			return out.string();
		}
		return {};
	}

	std::string ReadFile(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) return {};
		return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	}

	IDirect3DTexture9* LoadDDS(IDirect3DDevice9* device, const std::string& path, std::string* error)
	{
		auto fail = [&](const char* why) -> IDirect3DTexture9* { if (error) *error = why; return nullptr; };
		std::string d = ReadFile(path);
		if (d.size() < 128 || memcmp(d.data(), "DDS ", 4) != 0) return fail("not a DDS file");
		auto u32 = [&](size_t o) { uint32_t v; memcpy(&v, d.data() + o, 4); return v; };
		uint32_t height = u32(12), width = u32(16), mips = std::max(1u, u32(28));
		uint32_t pfFlags = u32(80), fourCC = u32(84), bits = u32(88), rMask = u32(92), aMask = u32(104);
		D3DFORMAT fmt = D3DFMT_UNKNOWN;
		int blockBytes = 0, pixelBytes = 0;
		if (pfFlags & 0x4) {   // DDPF_FOURCC
			if (fourCC == MAKEFOURCC('D', 'X', 'T', '1')) { fmt = D3DFMT_DXT1; blockBytes = 8; }
			else if (fourCC == MAKEFOURCC('D', 'X', 'T', '3')) { fmt = D3DFMT_DXT3; blockBytes = 16; }
			else if (fourCC == MAKEFOURCC('D', 'X', 'T', '5')) { fmt = D3DFMT_DXT5; blockBytes = 16; }
			else return fail("unsupported compression (BC5/BC7/DX10 textures are not shown)");
		} else if (bits == 32 && rMask == 0x00FF0000) {
			fmt = aMask ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
			pixelBytes = 4;
		} else {
			return fail("unsupported pixel format");
		}
		IDirect3DTexture9* tex = nullptr;
		if (FAILED(device->CreateTexture(width, height, mips, 0, fmt, D3DPOOL_MANAGED, &tex, nullptr)))
			return fail("CreateTexture failed");
		size_t offset = 128;
		uint32_t w = width, h = height;
		for (uint32_t level = 0; level < mips; level++) {
			uint32_t rows, rowBytes;
			if (blockBytes) { rows = std::max(1u, (h + 3) / 4); rowBytes = std::max(1u, (w + 3) / 4) * blockBytes; }
			else { rows = h; rowBytes = w * pixelBytes; }
			if (offset + (size_t)rows * rowBytes > d.size()) break;
			D3DLOCKED_RECT lr;
			if (SUCCEEDED(tex->LockRect(level, &lr, nullptr, 0))) {
				for (uint32_t y = 0; y < rows; y++)
					memcpy((char*)lr.pBits + y * lr.Pitch, d.data() + offset + (size_t)y * rowBytes, rowBytes);
				tex->UnlockRect(level);
			}
			offset += (size_t)rows * rowBytes;
			w = std::max(1u, w / 2);
			h = std::max(1u, h / 2);
		}
		return tex;
	}
}
