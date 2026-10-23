#pragma once
// A copy of a file about to be replaced, never overwriting an earlier one: the first is <file>.bak, later ones
// <file>.<date-time>.bak. None is made when the file is the same as its newest backup (pressing a button twice keeps
// the original). Only the newest kKeep backups of a file are kept: the oldest go once there are more (<file>.bak
// counts as the oldest). Shared by the previewer's Make SMP window and NV SMP Prepare.
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace backup
{
	inline std::string readAll(const std::filesystem::path& p)
	{
		std::ifstream in(p, std::ios::binary);
		return std::string((std::istreambuf_iterator<char>(in)), {});
	}

	constexpr size_t kKeep = 3;

	// Whether `name` is one of `file`'s backups as keep() names them: <file>.bak or <file>.<YYYYMMDD-HHMMSS>.bak.
	// Their order is the stamp's (copy_file keeps the source's write time, so file times can't be used).
	inline bool isBackupName(const std::string& name, const std::string& file, std::string* stamp)
	{
		if (name == file + ".bak") { *stamp = ""; return true; }   // the first: sorts before every stamp
		const std::string prefix = file + ".";
		if (name.size() != prefix.size() + 15 + 4 || name.rfind(prefix, 0) != 0 || name.compare(name.size() - 4, 4, ".bak") != 0)
			return false;
		const std::string s = name.substr(prefix.size(), 15);
		for (size_t i = 0; i < s.size(); ++i)
			if (i == 8 ? s[i] != '-' : (s[i] < '0' || s[i] > '9')) return false;
		*stamp = s;
		return true;
	}

	// Deletes the oldest of `file`'s backups beyond the newest kKeep.
	inline void prune(const std::filesystem::path& file)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		std::vector<std::pair<std::string, fs::path>> found;
		const std::string base = file.filename().string();
		for (auto& e : fs::directory_iterator(file.parent_path(), ec)) {
			std::string stamp;
			if (e.is_regular_file(ec) && isBackupName(e.path().filename().string(), base, &stamp))
				found.push_back({ stamp, e.path() });
		}
		if (found.size() <= kKeep) return;
		std::sort(found.begin(), found.end());
		for (size_t i = 0; i + kKeep < found.size(); ++i)
			fs::remove(found[i].second, ec);
	}

	// The backup's path (the existing one when the file is unchanged since), or empty when there is no file or the
	// copy failed (error then says why).
	inline std::string keep(const std::string& path, std::string* error = nullptr)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		if (!fs::exists(path, ec)) return "";
		const fs::path file(path);
		// The newest backup so far (by its stamp; another file's backup such as <file>.xml.bak is not one).
		fs::path newest;
		std::string newestStamp;
		for (auto& e : fs::directory_iterator(file.parent_path(), ec)) {
			std::string stamp;
			if (!isBackupName(e.path().filename().string(), file.filename().string(), &stamp)) continue;
			if (newest.empty() || stamp > newestStamp) { newest = e.path(); newestStamp = stamp; }
		}
		const std::string content = readAll(file);
		if (!newest.empty() && readAll(newest) == content) return newest.string();
		// <file>.bak only for the very first: once the oldest are pruned it would sort as the oldest again.
		fs::path target = fs::path(path + ".bak");
		if (!newest.empty() || fs::exists(target, ec)) {
			std::time_t now = std::time(nullptr);
			std::tm tm{};
			localtime_s(&tm, &now);
			char stamp[32];
			std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
			target = fs::path(path + "." + stamp + ".bak");
		}
		fs::copy_file(file, target, fs::copy_options::overwrite_existing, ec);
		if (ec) { if (error) *error = ec.message(); return ""; }
		prune(file);
		return target.string();
	}
}
