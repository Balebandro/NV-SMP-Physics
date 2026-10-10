#pragma once
// Forced into every translation unit (/FI), as FSMP's own PCH.h is: the standard library, the
// CommonLibSSE and oneTBB stand-ins, and FSMP's logger namespace on top of our log file.
#include <windows.h>   // CommonLibSSE brought it in: the core uses UINT and ZeroMemory
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "RE.h"
#include "tbb/tbb.h"

using namespace std::literals;

namespace nvsmp
{
	enum class LogLevel { Trace, Debug, Info, Warn, Error };
	void LogLine(LogLevel level, const std::string& text);
	bool LogEnabled(LogLevel level);
}

// FSMP's logger calls (spdlog style, "{}" formats).
namespace logger
{
	template <class... Args>
	void log_at(nvsmp::LogLevel level, std::string_view fmt, Args&&... args)
	{
		if (!nvsmp::LogEnabled(level)) return;
		try {
			nvsmp::LogLine(level, std::vformat(fmt, std::make_format_args(args...)));
		} catch (...) {
			nvsmp::LogLine(level, std::string(fmt));
		}
	}

	template <class... Args> void trace(std::string_view f, Args&&... a) { log_at(nvsmp::LogLevel::Trace, f, a...); }
	template <class... Args> void debug(std::string_view f, Args&&... a) { log_at(nvsmp::LogLevel::Debug, f, a...); }
	template <class... Args> void info(std::string_view f, Args&&... a) { log_at(nvsmp::LogLevel::Info, f, a...); }
	template <class... Args> void warn(std::string_view f, Args&&... a) { log_at(nvsmp::LogLevel::Warn, f, a...); }
	template <class... Args> void error(std::string_view f, Args&&... a) { log_at(nvsmp::LogLevel::Error, f, a...); }
	template <class... Args> void critical(std::string_view f, Args&&... a) { log_at(nvsmp::LogLevel::Error, f, a...); }
}
