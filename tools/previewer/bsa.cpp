#include "bsa.h"
#include "inflate.h"

#include <cstring>
#include <fstream>

namespace bsa
{
	std::string Normalize(const std::string& path)
	{
		std::string p;
		p.reserve(path.size());
		for (char c : path) p += c == '/' ? '\\' : (char)tolower((unsigned char)c);
		while (!p.empty() && p[0] == '\\') p.erase(0, 1);
		if (p.rfind("data\\", 0) == 0) p.erase(0, 5);
		return p;
	}

	bool Archive::Open(const std::string& path, std::string* error)
	{
		auto fail = [&](const char* why) { if (error) *error = why; return false; };
		std::ifstream in(path, std::ios::binary);
		if (!in) return fail("cannot open");
		m_path = path;
		auto u32 = [&]() { uint32_t v = 0; in.read((char*)&v, 4); return v; };
		auto u64 = [&]() { uint64_t v = 0; in.read((char*)&v, 8); return v; };
		char magic[4];
		in.read(magic, 4);
		if (memcmp(magic, "BSA\0", 4) != 0) return fail("not a BSA");
		uint32_t version = u32();
		if (version != 104) return fail("not a Fallout 3 / New Vegas BSA (version 104)");
		u32();   // header size (36)
		m_flags = u32();
		uint32_t folderCount = u32(), fileCount = u32();
		u32(); u32();   // total folder name length, total file name length
		u32();          // file flags
		if (!(m_flags & 1) || !(m_flags & 2)) return fail("archive without folder or file names");

		struct Folder { uint32_t count; };
		std::vector<Folder> folders(folderCount);
		for (auto& f : folders) { u64(); f.count = u32(); u32(); }

		struct Rec { std::string folder; uint32_t size, offset; };
		std::vector<Rec> recs;
		recs.reserve(fileCount);
		for (auto& f : folders) {
			uint8_t len = 0;
			in.read((char*)&len, 1);
			std::string name(len, '\0');
			in.read(name.data(), len);
			if (!name.empty() && name.back() == '\0') name.pop_back();
			for (uint32_t i = 0; i < f.count; i++) {
				u64();
				uint32_t size = u32(), offset = u32();
				recs.push_back({ name, size, offset });
			}
		}
		// File names: null-terminated, in record order.
		for (auto& r : recs) {
			std::string name;
			char c;
			while (in.get(c) && c) name += c;
			Entry e;
			e.offset = r.offset;
			e.size = r.size & 0x3FFFFFFF;
			// The archive's compression flag, inverted per file by bit 30 of its size.
			e.compressed = ((m_flags & 4) != 0) != ((r.size & 0x40000000) != 0);
			m_files[Normalize(r.folder + "\\" + name)] = e;
		}
		if (!in) return fail("truncated index");
		return true;
	}

	std::vector<uint8_t> Archive::Read(const std::string& normalized) const
	{
		auto it = m_files.find(normalized);
		if (it == m_files.end()) return {};
		const Entry& e = it->second;
		std::ifstream in(m_path, std::ios::binary);
		in.seekg(e.offset);
		std::vector<uint8_t> raw(e.size);
		in.read((char*)raw.data(), e.size);
		if (!in) return {};
		size_t pos = 0;
		// Some archives prefix each file's data with its full path (a length-prefixed string): skip it.
		if ((m_flags & 0x100) && !raw.empty() && raw[0] <= raw.size() - 1) {
			std::string name((const char*)raw.data() + 1, raw[0]);
			if (Normalize(name) == normalized) pos = 1 + (size_t)raw[0];
		}
		if (!e.compressed) return std::vector<uint8_t>(raw.begin() + pos, raw.end());
		if (raw.size() < pos + 4) return {};
		uint32_t original;
		memcpy(&original, raw.data() + pos, 4);
		std::vector<uint8_t> out(original);
		if (!inflate::Zlib(raw.data() + pos + 4, raw.size() - pos - 4, out)) return {};
		return out;
	}
}
