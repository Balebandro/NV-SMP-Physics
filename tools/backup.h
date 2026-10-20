#pragma once
// A copy of a file about to be replaced, never overwriting an earlier one: the first is <file>.bak, later ones
// <file>.<date-time>.bak. None is made when the file is the same as its newest backup (pressing a button twice keeps
// the original). Shared by the previewer's Make SMP window and NV SMP Prepare.
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace backup
{
	inline std::string readAll(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), {});
	}

	// The backup's path (the existing one when the file is unchanged since), or empty when there is no file or the
	// copy failed (error then says why).
	inline std::string keep(const std::string& path, std::string* error = nullptr)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		if (!fs::exists(path, ec)) return "";
		const fs::path file(path);
		// The newest backup so far.
		fs::path newest;
		fs::file_time_type newestTime{};
		const std::string prefix = file.filename().string() + ".";
		for (auto& e : fs::directory_iterator(file.parent_path(), ec)) {
			std::string n = e.path().filename().string();
			if (n.rfind(prefix, 0) != 0 || n.size() < 4 || n.compare(n.size() - 4, 4, ".bak") != 0) continue;
			auto t = fs::last_write_time(e.path(), ec);
			if (newest.empty() || t > newestTime) { newest = e.path(); newestTime = t; }
		}
		const std::string content = readAll(file);
		if (!newest.empty() && readAll(newest) == content) return newest.string();
		fs::path target = fs::path(path + ".bak");
		if (fs::exists(target, ec)) {
			std::time_t now = std::time(nullptr);
			std::tm tm{};
			localtime_s(&tm, &now);
			char stamp[32];
			std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
			target = fs::path(path + "." + stamp + ".bak");
		}
		fs::copy_file(file, target, fs::copy_options::overwrite_existing, ec);
		if (ec) { if (error) *error = ec.message(); return ""; }
		return target.string();
	}
}
