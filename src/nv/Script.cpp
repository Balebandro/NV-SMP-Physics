// Script commands for mods: physics switched off on chosen bones of an actor, an item's physics file swapped for another.
// Registered with xNVSE in NVSEPlugin_Load (main.cpp). Everything is per actor reference and kept until a save is loaded
// or the game goes to the main menu (a script sets it again on GetGameLoaded); the work is done by ActorManager.
//
//   ref.SMPSetBonePhysics "bone,bone2|prefix*" 0/1  off = the bones follow the animation; returns how many names were set
//   ref.SMPGetBonePhysics "bone"                     1 simulated, 0 not (by its file or a command), -1 not built on this actor
//   ref.SMPSetPhysicsFile "original.xml" "new.xml"   items built from original.xml use new.xml; "" (or omitted) goes back
//   ref.SMPGetPhysicsFile "original.xml"             (string) the file the item is built from now, "" if none such is worn
//   ref.SMPGetItemPhysicsFile ArmorForm             (string) the physics file of the SMP item the actor wears for that armor, "" if none
//   ref.SMPSetItemPhysicsFile ArmorForm "new.xml"   the same swap as SMPSetPhysicsFile, by the file that armor's NIF names (it must be worn)
//   ref.SMPRebuild                                   the actor's physics built again from scratch
//
// Opcodes: xNVSE's test range (0x2000), free in this load order; a public release needs a range assigned by the xNVSE team.
#include "ActorManager.h"

namespace nvsmp::Script
{
	struct ParamInfo { const char* typeStr; uint32_t typeID; uint32_t isOptional; };
	enum : uint32_t { kParamType_String = 0, kParamType_Integer = 1, kParamType_ObjectID = 3 };
	enum : uint32_t { kRetnType_Default = 0, kRetnType_String = 2 };

#define SMP_COMMAND_ARGS ParamInfo* paramInfo, void* scriptData, void* thisObj, void* containingObj, void* scriptObj, void* eventList, double* result, uint32_t* opcodeOffsetPtr
	typedef bool (*Execute_t)(SMP_COMMAND_ARGS);

	struct CommandInfo
	{
		const char* longName;
		const char* shortName;
		uint32_t opcode;
		const char* helpText;
		uint16_t needsParent;
		uint16_t numParams;
		ParamInfo* params;
		Execute_t execute;
		void* parse;
		void* eval;
		uint32_t flags;
	};

	struct StringVarInterface
	{
		uint32_t version;
		const char* (*GetString)(uint32_t);
		void (*SetString)(uint32_t, const char*);
		uint32_t (*CreateString)(const char*, void*);
		void (*Register)(StringVarInterface*);
		bool (*Assign)(SMP_COMMAND_ARGS, const char*);
	};

	typedef bool (*ExtractArgs_t)(ParamInfo*, void*, uint32_t*, void*, void*, void*, void*, ...);
	static const auto ExtractArgs = (ExtractArgs_t)0x005ACCB0;   // the game's (1.4.0.525)
	static void* const kDefaultParse = nullptr;   // xNVSE fills in its own default parse (the game's and the GECK's differ)
	static constexpr uint32_t kOpcodeBase = 0x2000;
	static StringVarInterface* g_strings = nullptr;

	static ParamInfo kStringInt[] = { { "bones", kParamType_String, 0 }, { "on", kParamType_Integer, 0 } };
	static ParamInfo kString[] = { { "name", kParamType_String, 0 } };
	static ParamInfo kStringOptString[] = { { "original", kParamType_String, 0 }, { "replacement", kParamType_String, 1 } };
	static ParamInfo kForm[] = { { "armor", kParamType_ObjectID, 0 } };
	static ParamInfo kFormOptString[] = { { "armor", kParamType_ObjectID, 0 }, { "replacement", kParamType_String, 1 } };

	static bool SetBonePhysics(SMP_COMMAND_ARGS)
	{
		*result = 0;
		char bones[0x200] = {};
		uint32_t on = 1;
		if (!thisObj || !ExtractArgs(paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList, bones, &on))
			return true;
		*result = ActorManager::instance()->setBonePhysics(thisObj, bones, on != 0);
		logger::info("[script] {:p}: SMPSetBonePhysics \"{}\" {}: {} name(s), the actor's physics built again", thisObj, bones, on, *result);
		return true;
	}

	static bool GetBonePhysics(SMP_COMMAND_ARGS)
	{
		*result = -1;
		char bone[0x200] = {};
		if (!thisObj || !ExtractArgs(paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList, bone))
			return true;
		*result = ActorManager::instance()->getBonePhysics(thisObj, bone);
		return true;
	}

	static bool SetPhysicsFile(SMP_COMMAND_ARGS)
	{
		*result = 0;
		char original[0x200] = {}, replacement[0x200] = {};
		if (!thisObj || !ExtractArgs(paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList, original, replacement))
			return true;
		ActorManager::instance()->setPhysicsFile(thisObj, original, replacement);
		logger::info("[script] {:p}: SMPSetPhysicsFile \"{}\" -> \"{}\"", thisObj, original, replacement[0] ? replacement : "(its own)");
		*result = 1;
		return true;
	}

	static bool GetPhysicsFile(SMP_COMMAND_ARGS)
	{
		*result = 0;
		char original[0x200] = {};
		std::string file;
		if (thisObj && ExtractArgs(paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList, original))
			file = ActorManager::instance()->getPhysicsFile(thisObj, original);
		if (g_strings)
			g_strings->Assign(paramInfo, scriptData, thisObj, containingObj, scriptObj, eventList, result, opcodeOffsetPtr, file.c_str());
		return true;
	}

	static bool GetItemPhysicsFile(SMP_COMMAND_ARGS)
	{
		*result = 0;
		void* form = nullptr;
		std::string file;
		if (thisObj && ExtractArgs(paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList, &form))
			file = ActorManager::instance()->getItemPhysicsFile(thisObj, form);
		if (g_strings)
			g_strings->Assign(paramInfo, scriptData, thisObj, containingObj, scriptObj, eventList, result, opcodeOffsetPtr, file.c_str());
		return true;
	}

	static bool SetItemPhysicsFile(SMP_COMMAND_ARGS)
	{
		*result = 0;
		void* form = nullptr;
		char replacement[0x200] = {};
		if (!thisObj || !ExtractArgs(paramInfo, scriptData, opcodeOffsetPtr, thisObj, containingObj, scriptObj, eventList, &form, replacement))
			return true;
		*result = ActorManager::instance()->setItemPhysicsFile(thisObj, form, replacement);
		logger::info("[script] {:p}: SMPSetItemPhysicsFile {:p} -> \"{}\": {}", thisObj, form, replacement[0] ? replacement : "(its own)", *result ? "set" : "that armor has no SMP item on the actor");
		return true;
	}

	static bool Rebuild(SMP_COMMAND_ARGS)
	{
		*result = 0;
		if (!thisObj) return true;
		ActorManager::instance()->requestRebuild(thisObj);
		*result = 1;
		return true;
	}

	static CommandInfo g_commands[] = {
		{ "SMPSetBonePhysics", "", 0, "Switches SMP physics on (1) or off (0) for the named bones of the actor (',' or '|' list, trailing * = prefix)", 1, 2, kStringInt, SetBonePhysics, kDefaultParse, nullptr, 0 },
		{ "SMPGetBonePhysics", "", 0, "1 if the actor's SMP bone is simulated, 0 if not, -1 if the actor has no such bone built", 1, 1, kString, GetBonePhysics, kDefaultParse, nullptr, 0 },
		{ "SMPSetPhysicsFile", "", 0, "Builds the actor's items that use the first physics file from the second instead (\"\" restores)", 1, 2, kStringOptString, SetPhysicsFile, kDefaultParse, nullptr, 0 },
		{ "SMPGetPhysicsFile", "", 0, "The physics file the actor's item using this file is built from now (string)", 1, 1, kString, GetPhysicsFile, kDefaultParse, nullptr, 0 },
		{ "SMPRebuild", "", 0, "Builds the actor's SMP physics again", 1, 0, nullptr, Rebuild, kDefaultParse, nullptr, 0 },
		{ "SMPGetItemPhysicsFile", "", 0, "The physics file of the SMP item the actor wears for this armor (string, \"\" if none)", 1, 1, kForm, GetItemPhysicsFile, kDefaultParse, nullptr, 0 },
		{ "SMPSetItemPhysicsFile", "", 0, "Builds the SMP item the actor wears for this armor from another physics file (\"\" restores)", 1, 2, kFormOptString, SetItemPhysicsFile, kDefaultParse, nullptr, 0 },
	};

	void Register(void* registerCommand, void* registerTypedCommand, void* setOpcodeBase, void* stringVarInterface)
	{
		auto setBase = (void (*)(uint32_t))setOpcodeBase;
		auto reg = (bool (*)(CommandInfo*))registerCommand;
		auto regTyped = (bool (*)(CommandInfo*, uint32_t))registerTypedCommand;
		g_strings = (StringVarInterface*)stringVarInterface;
		if (!setBase || !reg) { logger::warn("[script] xNVSE gave no command registration: no script commands"); return; }
		setBase(kOpcodeBase);
		int ok = 0;
		for (auto& c : g_commands) {
			const bool string = c.execute == GetPhysicsFile || c.execute == GetItemPhysicsFile;
			if (string && !regTyped) { logger::warn("[script] {}: no string support from xNVSE, not registered", c.longName); continue; }   // no g_strings in the GECK (xNVSE gives StringVar at runtime only); it never executes there
			ok += string ? regTyped(&c, kRetnType_String) : reg(&c);
		}
		logger::info("[script] {} script command(s) registered at opcode {:04X}", ok, kOpcodeBase);
	}
}
