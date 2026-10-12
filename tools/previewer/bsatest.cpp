// bsatest: lists and extracts files from a BSA with tools/previewer/bsa.cpp, to check the reader.
//   bsatest archive.bsa [substring [out-folder]]
// Lists the files whose path contains the substring; with an out folder, extracts them there.
#include "bsa.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv)
{
	if (argc < 2) {
		printf("usage: bsatest archive.bsa [substring [out-folder]]\n");
		return 1;
	}
	bsa::Archive a;
	std::string err;
	if (!a.Open(argv[1], &err)) {
		printf("error: %s\n", err.c_str());
		return 1;
	}
	printf("%zu files\n", a.FileCount());
	if (argc < 3) return 0;
	std::string sub = bsa::Normalize(argv[2]);
	int shown = 0, bad = 0;
	std::vector<std::string> paths;
	a.ForEach([&](const std::string& p) { if (p.find(sub) != std::string::npos) paths.push_back(p); });
	for (auto& p : paths) {
		auto data = a.Read(p);
		if (data.empty()) bad++;
		if (shown++ < 40) printf("  %8zu  %s\n", data.size(), p.c_str());
		if (argc > 3 && !data.empty()) {
			std::filesystem::path out = std::filesystem::path(argv[3]) / std::filesystem::path(p).filename();
			std::ofstream(out, std::ios::binary).write((const char*)data.data(), data.size());
		}
	}
	printf("%zu matching, %d unreadable\n", paths.size(), bad);
	return bad ? 2 : 0;
}
