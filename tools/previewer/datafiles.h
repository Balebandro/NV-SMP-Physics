#pragma once
// Where the previewer finds what a NIF names (its physics XML, the skeleton, animations, textures): the
// game's virtual file system, approximated. Loose files in the Data folders first (the NIF's own Data
// folder, Mod Organizer 2's enabled mods in priority order and its overwrite folder, extra folders, the
// game's Data folder), then the BSA archives (the mods', then the game's). A file found in an archive is
// extracted once to a cache folder, so callers always get a path on disk.
#include "bsa.h"

#include <d3d9.h>
#include <memory>
#include <string>
#include <vector>

namespace datafiles
{
	// What the user set by hand (the Data sources panel); empty means detect.
	struct Options
	{
		std::string mo2Instance;           // a folder holding ModOrganizer.ini
		std::string mo2Profile;
		std::string gameFolder;            // the folder holding FalloutNV.exe
		std::vector<std::string> extraFolders;   // searched after the mods, before the game's Data
		bool useArchives = true;
	};

	struct Roots
	{
		std::vector<std::string> folders;              // loose files, searched first to last
		std::vector<std::shared_ptr<bsa::Archive>> archives;   // then these, first to last
		std::string mo2Instance, mo2Profile, mo2How;   // the instance used, and how it was found
		std::vector<std::string> mo2Profiles;          // the instance's profiles, for the panel
		std::string gameData, gameHow;                 // the game's Data folder, and how it was found
		std::vector<std::string> notes;                // what could not be found or read
	};

	Roots Discover(const std::string& nifPath, const Options& options);

	// The first file for a Data-relative path ("Meshes/armor/x.xml", "textures\\x.dds", "Data\\..."), as a
	// path on disk (an archive's file extracted to the cache), or empty. source, if given, gets where it
	// came from (the folder, or the archive).
	std::string Resolve(const Roots& roots, const std::string& relative, std::string* source = nullptr);

	std::string ReadFile(const std::string& path);

	// A DDS file as a Direct3D 9 texture (DXT1/3/5 and 32-bit RGB(A); others give nullptr).
	IDirect3DTexture9* LoadDDS(IDirect3DDevice9* device, const std::string& path, std::string* error = nullptr);

	// The game folder from the registry (GOG, Steam, the Bethesda launcher key) or Steam's libraries.
	std::string FindGameFolder(std::string* how = nullptr);
}
