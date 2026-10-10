#pragma once
// The types FSMP's system builder takes from DefaultBBP. FSMP also maps physics files onto body
// replacers' shape names through defaultBBPs.xml; New Vegas meshes name their own XML, so the file's
// name map is empty and a shape in the XML is looked up by its own name.
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace hdt
{
	class DefaultBBP
	{
	public:
		using NameSet_t = std::unordered_set<std::string>;
		using NameMap_t = std::unordered_map<std::string, NameSet_t>;
		using PhysicsFile_t = std::pair<std::string, NameMap_t>;
	};
}
