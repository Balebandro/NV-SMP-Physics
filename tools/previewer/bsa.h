#pragma once
// Fallout 3 / New Vegas BSA archives (version 104): the file index, and reading a file out of one
// (zlib-compressed or stored). Paths are matched lower case with backslashes, "meshes\\x\\y.nif".
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bsa
{
	class Archive
	{
	public:
		// Reads the index. False (and an error) on a file that is not a version 104 BSA.
		bool Open(const std::string& path, std::string* error = nullptr);
		const std::string& Path() const { return m_path; }
		size_t FileCount() const { return m_files.size(); }
		bool Contains(const std::string& normalized) const { return m_files.count(normalized) > 0; }
		// The file's bytes, decompressed; empty if absent or unreadable.
		std::vector<uint8_t> Read(const std::string& normalized) const;
		// Every path in the archive (for listing folders such as AnimGroupOverride).
		template <class F> void ForEach(F f) const { for (auto& [p, e] : m_files) f(p); }

	private:
		struct Entry
		{
			uint32_t offset, size;
			bool compressed;
		};
		std::string m_path;
		uint32_t m_flags = 0;
		std::unordered_map<std::string, Entry> m_files;
	};

	// "Data/Meshes/X.nif" -> "meshes\\x.nif"
	std::string Normalize(const std::string& path);
}
