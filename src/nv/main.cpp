// NV SMP Physics: FSMP (HDT-SMP for Skyrim) physics for Fallout New Vegas, as an xNVSE plugin.
//
// Once a frame, right after the game finishes animating and updating every actor and before it
// draws (TESMain's call to DrawWorld_WaitForParallelUpdate), the actor manager finds the items with
// SMP physics files, and the physics world reads the animated pose, steps Bullet and writes the
// simulated bones back, so the bones it writes are the ones rendered this frame.
#include <share.h>
#include "ActorManager.h"
#include "Game.h"
#include "Menu.h"
#include "Settings.h"
#include "hdtSkyrimPhysicsWorld.h"
#include "hdtSkyrimBone.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")

namespace nvsmp
{
	Settings gSettings;

	static FILE* gLog = nullptr;
	static std::mutex gLogLock;
	static char gIniPath[MAX_PATH];
	static bool gFailed = false;
	static int gLastGameFrame = -1;
	static bool gLoading = false;   // between xNVSE's pre-load and post-load messages

	bool LogEnabled(LogLevel level) { return gLog && (int)level >= gSettings.logLevel; }

	void LogLine(LogLevel level, const std::string& text)
	{
		if (!gLog)
			return;
		static const char* kNames[] = { "trace", "debug", "info", "warn", "error" };
		std::lock_guard<std::mutex> guard(gLogLock);
		fprintf(gLog, "[%s] %s\n", kNames[(int)level], text.c_str());
		fflush(gLog);
	}

	void Log(const char* fmt, ...)
	{
		if (!gLog)
			return;
		char buf[1024];
		va_list a;
		va_start(a, fmt);
		vsnprintf(buf, sizeof buf, fmt, a);
		va_end(a);
		LogLine(LogLevel::Info, buf);
	}

	// --- settings ---------------------------------------------------------------------------------

	struct Key
	{
		const char* name;
		float* f;
		int* i;
		bool* b;
	};

	static std::vector<Key> SettingKeys()
	{
		Settings& s = gSettings;
		return {
			{ "bEnabled", 0, 0, &s.enabled }, { "fCollisionDistance", &s.collisionDistance },{ "iMaxActors", 0, &s.maxActors },
			{ "iMinFps", 0, &s.minFps }, { "iMaxSubSteps", 0, &s.maxSubSteps }, { "iSolverIterations", 0, &s.solverIterations }, { "bCollisionThreads", 0, 0, &s.collisionThreadsOn }, { "iCollisionThreads", 0, &s.collisionThreads },{ "bAsyncStep", 0, 0, &s.asyncStep },
			{ "fGravityScale", &s.gravityScale }, { "bUnclampedResets", 0, 0, &s.unclampedResets }, { "fResetAngle", &s.resetAngle },
			{ "bWind", 0, 0, &s.wind }, { "fWindStrength", &s.windStrength }, { "bWindInInteriors", 0, 0, &s.windInInteriors }, { "bCollisions", 0, 0, &s.collisions }, { "bCollideOncePerFrame", 0, 0, &s.collideOncePerFrame },
			{ "fWaistCollision", &s.waistCollision }, { "bGround", 0, 0, &s.ground }, { "bJitterFilter", 0, 0, &s.jitterFilter }, { "fJitterSmoothing", &s.jitterSmoothing },
			{ "iMenuKey", 0, &s.menuKey }, { "iLogLevel", 0, &s.logLevel },
		};
	}

	void ApplySettings()
	{
		auto* world = hdt::SkyrimPhysicsWorld::get();
		world->waitForStep();   // the menu changes the world's settings and bodies: not under a running step
		auto& s = gSettings;
		s.minFps = std::clamp(s.minFps, 30, 240);
		s.maxSubSteps = std::clamp(s.maxSubSteps, 1, 16);
		s.solverIterations = std::clamp(s.solverIterations, 1, 64);
		world->m_timeTick = 1.0f / (float)s.minFps;
		world->m_maxSubSteps = s.maxSubSteps;
		world->getSolverInfo().m_numIterations = s.solverIterations;
		s.collisionThreads = std::clamp(s.collisionThreads, -1, 8);
		tbb::SetWorkers(!s.collisionThreadsOn ? 0 : s.collisionThreads < 0 ? tbb::AutoWorkers() : s.collisionThreads);   // after waitForStep: no loop is running
		world->setGravity(btVector3(0, 0, -9.8f * hdt::scaleSkyrim * s.gravityScale));
		world->m_unclampedResets = s.unclampedResets;
		world->m_unclampedResetAngle = s.resetAngle;
		world->m_enableWind = s.wind;
		world->m_windStrength = s.windStrength;
		world->disabled = !s.enabled;
		world->m_collisionsEnabled = s.collisions;
		world->m_collideOncePerFrame = s.collideOncePerFrame;
		s.waistCollision = std::clamp(s.waistCollision, 0.0f, 1.0f);
		s.jitterSmoothing = std::clamp(s.jitterSmoothing, 0.0f, 1.0f);
		hdt::SkyrimBone::SetJitterFilter(s.jitterFilter, s.jitterSmoothing);
		ActorManager::instance()->applyCollision();
		Menu::SetKey(s.menuKey);
	}

	void LoadSettings()
	{
		for (auto& k : SettingKeys()) {
			char b[64];
			GetPrivateProfileStringA("Main", k.name, "", b, 64, gIniPath);
			if (!b[0])
				continue;
			if (k.f)
				*k.f = (float)atof(b);
			else if (k.i)
				*k.i = (int)strtol(b, nullptr, 0);
			else
				*k.b = atoi(b) != 0;
		}
		ApplySettings();
	}

	void SaveSettings()
	{
		for (auto& k : SettingKeys()) {
			char b[64];
			if (k.f)
				sprintf_s(b, "%g", *k.f);
			else if (k.i)
				sprintf_s(b, "%d", *k.i);
			else
				sprintf_s(b, "%d", *k.b ? 1 : 0);
			WritePrivateProfileStringA("Main", k.name, b, gIniPath);
		}
		Log("settings saved to %s", gIniPath);
	}

	void DefaultSettings()
	{
		int log = gSettings.logLevel;
		gSettings = Settings();
		gSettings.logLevel = log;
		ApplySettings();
	}

	// --- the frame -----------------------------------------------------------------------------------

	static void UpdateWind()
	{
		auto* world = hdt::SkyrimPhysicsWorld::get();
		btVector3 target(0, 0, 0);
		if (gSettings.wind && (!Game::InInterior() || gSettings.windInInteriors)) {
			// The weather's wind, as the grass and trees get it (Sky::UpdateWind blends the current
			// and previous weather into BSShaderManager). Scaled as FSMP's WeatherManager does.
			float magnitude = std::clamp(*(float*)Game::kWindMagnitude, 0.0f, 1.0f);
			float dx = *(float*)Game::kWindDirection, dy = *(float*)(Game::kWindDirection + 4);
			float dl = std::sqrt(dx * dx + dy * dy);
			if (dl > 1e-4f)
				target = btVector3(dx / dl, dy / dl, 0) * (world->m_windStrength * hdt::scaleSkyrim * magnitude);
		}
		world->setWind(target);
	}

	static void Update()
	{
		int gameFrame = Game::FrameCounter();
		if (gameFrame == gLastGameFrame)
			return;
		gLastGameFrame = gameFrame;
		Menu::Tick();
		if (gLoading)
			return;

		// The step started last frame on the physics thread (bAsyncStep): its bones are written before anything here
		// changes the world. With it off, the whole update runs below, as it always did.
		auto* world = hdt::SkyrimPhysicsWorld::get();
		world->finishUpdate();
		if (std::string fault = world->takeStepFailure(); !fault.empty()) {
			LogLine(LogLevel::Error, "exception in the physics step (physics thread); NV SMP Physics is off for the rest of this session: " + fault);
			gFailed = true;
			return;
		}

		ActorManager::instance()->rebuildIfPending();
		if (Menu::ConsumeReset()) {
			ActorManager::instance()->clear();
		}
		if (!gSettings.enabled) {
			if (hdt::SkyrimPhysicsWorld::get()->systemCount())
				ActorManager::instance()->clear();
			return;
		}

		ActorManager::instance()->update();
		UpdateWind();

		float dt = Game::FrameTime();
		// 0 while paused: the bones keep their last simulated pose. The race menu pauses the game too, but shows the
		// character turning and changing, and in dialogue the speaker keeps animating (both menu mode, frame time 0):
		// there the physics runs on real time.
		static LARGE_INTEGER lastTick{};
		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		const float real = lastTick.QuadPart ? (float)((now.QuadPart - lastTick.QuadPart) / (double)freq.QuadPart) : 0.0f;
		lastTick = now;
		static bool raceMenu = false;
		if (Game::RaceMenuOpen() != raceMenu) {
			raceMenu = !raceMenu;
			logger::info("[SMP] race menu {}", raceMenu ? "open: physics on real time" : "closed");
		}
		static bool dialogMenu = false;
		if (Game::DialogMenuOpen() != dialogMenu) {
			dialogMenu = !dialogMenu;
			logger::info("[SMP] dialogue {}", dialogMenu ? "open: physics on real time" : "closed");
		}
		if (dt <= FLT_EPSILON && (raceMenu || dialogMenu))
			dt = std::min(real, 0.05f);

		// Every 120 frames (by count, so it shows even if the frame time reads 0): what the physics is doing. Before the
		// step starts: the report reads the bodies.
		static int sinceReport = 0;
		if (++sinceReport >= 120) {
			sinceReport = 0;
			logger::info("[stats] frame time {:.4f} s, {} system(s) in the world", dt, world->systemCount());
			ActorManager::instance()->report();
		}

		if (gSettings.asyncStep) {
			// The bones as written above (last frame's step); this frame's step runs while the game draws.
			ActorManager::instance()->skinOnCpu();   // race hair (also while paused: the bones hold)
			if (dt > FLT_EPSILON)
				world->beginUpdate(std::min(dt, 0.25f), true);
		} else {
			if (dt > FLT_EPSILON)
				world->doUpdate(std::min(dt, 0.25f));
			ActorManager::instance()->skinOnCpu();   // race hair, after the bones moved (also while paused: they hold)
		}
	}

	// Where the fault was, filled by the exception filter: the code, the faulting address, and the return
	// addresses inside this DLL found on the stack (callers, as offsets from the DLL's base to look up in the pdb).
	static char gFailureWhere[512];

	static void DescribeAddress(char* out, size_t size, uintptr_t address)
	{
		HMODULE module = nullptr;
		char path[MAX_PATH] = "?";
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module)) {
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = strrchr(path, '\\');
			snprintf(out, size, "%s+%X", name ? name + 1 : path, (unsigned)(address - (uintptr_t)module));
		} else
			snprintf(out, size, "%08X", (unsigned)address);
	}

	static int RecordFailure(EXCEPTION_POINTERS* info)
	{
		char where[128];
		DescribeAddress(where, sizeof(where), (uintptr_t)info->ExceptionRecord->ExceptionAddress);
		int len = snprintf(gFailureWhere, sizeof(gFailureWhere), "code %08X at %s", (unsigned)info->ExceptionRecord->ExceptionCode, where);
		if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2)
			len += snprintf(gFailureWhere + len, sizeof(gFailureWhere) - len, " (%s %08X)",
				info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading", (unsigned)info->ExceptionRecord->ExceptionInformation[1]);

		// Callers: return addresses into this DLL among the first stack slots.
		HMODULE self = nullptr;
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&RecordFailure, &self);
		MODULEINFO mi = {};
		if (self && GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof(mi))) {
			uintptr_t lo = (uintptr_t)mi.lpBaseOfDll, hi = lo + mi.SizeOfImage;
			const uint32_t* sp = (const uint32_t*)info->ContextRecord->Esp;
			len += snprintf(gFailureWhere + len, sizeof(gFailureWhere) - len, "; stack:");
			for (int i = 0, found = 0; i < 512 && found < 12 && len < (int)sizeof(gFailureWhere) - 16; i++) {
				if (IsBadReadPtr(sp + i, 4)) break;
				uintptr_t v = sp[i];
				if (v > lo && v < hi) {
					len += snprintf(gFailureWhere + len, sizeof(gFailureWhere) - len, " +%X", (unsigned)(v - lo));
					found++;
				}
			}
		}
		return EXCEPTION_EXECUTE_HANDLER;
	}

	static void ReportFailure()
	{
		LogLine(LogLevel::Error, std::string("exception in the physics update; NV SMP Physics is off for the rest of this session: ") + gFailureWhere);
	}

	static void SafeUpdate()
	{
		__try {
			Update();
		} __except (RecordFailure(GetExceptionInformation())) {
			gFailed = true;
			ReportFailure();
		}
	}

	static void __cdecl WaitForParallelUpdateHook()
	{
		((void(__cdecl*)())Game::kWaitForParallelUpdate)();
		if (!gFailed)
			SafeUpdate();
	}

	static void PatchCall(uint32_t site)
	{
		uint8_t* p = (uint8_t*)site;
		if (p[0] != 0xE8 || site + 5 + *(int32_t*)(p + 1) != Game::kWaitForParallelUpdate) {
			Log("call at %08X is not the expected one, left alone", site);
			return;
		}
		DWORD old;
		VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old);
		*(int32_t*)(p + 1) = (int32_t)((uint32_t)&WaitForParallelUpdateHook - (site + 5));
		VirtualProtect(p, 5, old, &old);
		FlushInstructionCache(GetCurrentProcess(), p, 5);
	}
}

// --- engine helpers the glue declares (NetImmerseUtils.h) ---------------------------------------------

namespace hdt
{
	RE::NiNode* createNode(const char* name)
	{
		typedef void*(__cdecl * NiNew_t)(size_t);
		typedef RE::NiNode*(__thiscall * NiNodeCtor_t)(void*, uint16_t);
		typedef char*(__cdecl * CreateFixedString_t)(const char*);
		typedef void(__thiscall * SetName_t)(void*, char**);
		void* mem = ((NiNew_t)Game::kNiNew)(sizeof(RE::NiNode));
		if (!mem)
			return nullptr;
		RE::NiNode* node = ((NiNodeCtor_t)Game::kNiNodeCtor)(mem, 0);
		char* s = ((CreateFixedString_t)Game::kCreateFixedString)(name);
		((SetName_t)Game::kSetName)(node, &s);
		return node;
	}

	void setName(RE::NiAVObject* obj, const char* name)
	{
		typedef char*(__cdecl * CreateFixedString_t)(const char*);
		typedef void(__thiscall * SetName_t)(void*, char**);
		char* s = ((CreateFixedString_t)Game::kCreateFixedString)(name);
		((SetName_t)Game::kSetName)(obj, &s);
	}

	// FileFinder::GetFile(name, READ_ONLY, 0x4000, archive types) -> a BSFile: m_bGood at +0x2C,
	// GetSize at vtable slot 10, NiBinaryStream::m_pfnRead at +0x08, Destroy at slot 0. The size
	// goes in *size and the bytes in buffer (at most capacity); false if the game has no such file.
	// The archive types are flags, as the game passes them (meshes 1, textures 2, menus 4, sounds 8,
	// voices 0x10, shaders 0x20, trees 0x40, fonts 0x80, misc 0x100): none (0) searches no archive.
	static bool ReadFromArchives(const char* rel, char* buffer, uint32_t capacity, uint32_t* size, uint32_t types)
	{
		*size = 0;
		__try {
			typedef void*(__cdecl * GetFileFn)(const char*, uint32_t, uint32_t, uint32_t);
			typedef uint32_t(__cdecl * ReadFn)(void*, void*, uint32_t, uint32_t*, uint32_t);
			typedef uint32_t(__thiscall * GetSize_t)(void*);
			typedef void(__thiscall * Destroy_t)(void*, bool);
			void* file = ((GetFileFn)Game::kFileFinderGetFile)(rel, 0, 0x4000, types);
			if (!file)
				return false;
			uint32_t* vtbl = *(uint32_t**)file;
			bool ok = false;
			if (*(bool*)((uint8_t*)file + 0x2C)) {
				uint32_t total = ((GetSize_t)vtbl[10])(file);
				*size = total;
				if (buffer && total <= capacity) {
					uint32_t componentSize = 1;
					ok = (*(ReadFn*)((uint8_t*)file + 0x08))(file, buffer, total, &componentSize, 1) == total;
				} else {
					ok = !buffer;
				}
			}
			((Destroy_t)vtbl[0])(file, true);
			return ok;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return false;
		}
	}

	// Loose first (Mod Organizer's virtual Data folder answers this), then the game's archives
	// through FileFinder, as the game reads its own files.
	std::string readAllFile(const char* path)
	{
		std::string rel = path;
		for (auto& c : rel)
			if (c == '/')
				c = '\\';
		if (_strnicmp(rel.c_str(), "Data\\", 5) == 0)
			rel = rel.substr(5);

		std::ifstream in("Data\\" + rel, std::ios::binary);
		if (in)
			return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

		// The archive type by the folder (a mesh, its .egm and its physics file sit in meshes archives), then each of the
		// others: what an archive is flagged as depends on how it was packed.
		uint32_t first = 0x100;
		if (_strnicmp(rel.c_str(), "meshes\\", 7) == 0) first = 1;
		else if (_strnicmp(rel.c_str(), "textures\\", 9) == 0) first = 2;
		const uint32_t order[] = { first, 1, 0x100, 2, 4, 8, 0x10, 0x20, 0x40, 0x80 };
		for (uint32_t i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
			if (i > 0 && order[i] == first) continue;
			uint32_t size = 0;
			if (!ReadFromArchives(rel.c_str(), nullptr, 0, &size, order[i]) || size == 0 || size > 0x1000000)
				continue;
			std::string out(size, '\0');
			if (ReadFromArchives(rel.c_str(), out.data(), size, &size, order[i]))
				return out;
		}
		return {};
	}
}

// --- xNVSE plugin interface --------------------------------------------------------------------------

struct PluginInfo
{
	enum { kInfoVersion = 1 };
	uint32_t infoVersion;
	const char* name;
	uint32_t version;
};

struct NVSEInterface
{
	uint32_t nvseVersion, runtimeVersion, editorVersion, isEditor;
	void* registerCommand;
	void* setOpcodeBase;
	void* (*QueryInterface)(uint32_t id);
	uint32_t (*GetPluginHandle)();
};

struct NVSEDataInterface
{
	uint32_t version;
	void* (*GetSingleton)(uint32_t id);
};

struct NVSEMessage
{
	const char* sender;
	uint32_t type;
	uint32_t dataLen;
	void* data;
};

struct NVSEMessagingInterface
{
	uint32_t version;
	bool (*RegisterListener)(uint32_t listener, const char* sender, void (*handler)(NVSEMessage*));
};

static constexpr uint32_t kInterface_Messaging = 2, kInterface_Data = 7, kNVSEData_DIHookControl = 1;
enum : uint32_t { kMessage_ExitToMainMenu = 2, kMessage_PreLoadGame = 6, kMessage_PostLoadGame = 8, kMessage_NewGame = 14 };

// A load replaces every actor's 3D, partly on the loading thread: the systems go while the old 3D is
// still whole, and nothing is scanned or built until the load is over.
static void OnNVSEMessage(NVSEMessage* msg)
{
	using namespace nvsmp;
	switch (msg->type) {
	case kMessage_PreLoadGame:
	case kMessage_ExitToMainMenu:
		gLoading = true;
		ActorManager::instance()->clear();
		Log("%s: physics cleared", msg->type == kMessage_PreLoadGame ? "loading a save" : "main menu");
		break;
	case kMessage_PostLoadGame:
	case kMessage_NewGame:
		gLoading = false;
		Log("save loaded: physics resumes");
		break;
	}
}
static HMODULE gSelf = nullptr;

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		gSelf = h;
	return TRUE;
}

extern "C"
{
	__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info)
	{
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "NV SMP Physics";
		info->version = 1;
		return !nvse->isEditor && (nvse->runtimeVersion & ~1u) == 0x040020D0;   // 1.4.0.525 (and its no-gore build)
	}

	__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface* nvse)
	{
		using namespace nvsmp;
		if (nvse->isEditor)
			return false;
		GetModuleFileNameA(gSelf, gIniPath, MAX_PATH);
		if (char* dot = strrchr(gIniPath, '.'))
			strcpy_s(dot, gIniPath + MAX_PATH - dot, ".ini");
		gLog = _fsopen("NVSMPPhysics.log", "w", _SH_DENYWR);   // readable while the game runs
		LoadSettings();
		Log("NV SMP Physics 1 loaded (FSMP physics core, Bullet %d)", btGetVersion());
		if (auto data = (NVSEDataInterface*)nvse->QueryInterface(kInterface_Data))
			Menu::SetInputHook(data->GetSingleton(kNVSEData_DIHookControl));
		if (auto messaging = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging))
			messaging->RegisterListener(nvse->GetPluginHandle(), "NVSE", OnNVSEMessage);
		else
			Log("no xNVSE messaging: physics is not paused across loads");
		for (uint32_t s : Game::kWaitForParallelUpdateCalls)
			PatchCall(s);
		return true;
	}
}
