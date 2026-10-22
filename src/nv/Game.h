#pragma once
// Fallout New Vegas 1.4.0.525 addresses and calls the plugin uses, from the decompiled FalloutNV.exe
// (the same ones NV Cloth Physics and NVR use).
#include "NetImmerseUtils.h"

namespace Game
{
	template <class T>
	inline T& At(const void* p, size_t offset) { return *(T*)((char*)p + offset); }

	// Engine functions
	constexpr uint32_t kNiNew = 0x00AA13E0;                // NiNew (the game's allocator)
	constexpr uint32_t kNiNodeCtor = 0x00A5ECB0;           // NiNode::NiNode(UInt16 childCapacity)
	constexpr uint32_t kCreateFixedString = 0x00A5B690;    // NiFixedString from a C string
	constexpr uint32_t kSetName = 0x00A5B950;              // NiObjectNET::SetName
	constexpr uint32_t kRefGet3D = 0x0043FCD0;             // TESObjectREFR::Get3D
	constexpr uint32_t kPlayerGet3D = 0x00950BB0;          // PlayerCharacter::Get3D(bool firstPerson) (as NV Cloth Physics uses it)
	constexpr uint32_t kWaitForParallelUpdate = 0x00872AD0; // TESMain::DrawWorld_WaitForParallelUpdate
	constexpr uint32_t kFileFinderGetFile = 0x00AFDF20;    // FileFinder::GetFile (loose, then archives)

	// The calls to WaitForParallelUpdate in TESMain's draw paths: the game has finished animating
	// and updating every actor's world transforms, and has not drawn yet.
	constexpr uint32_t kWaitForParallelUpdateCalls[] = { 0x0087080B, 0x00870A4E, 0x00870C16, 0x00872964 };

	// Globals
	constexpr uint32_t kPlayer = 0x011DEA3C;         // PlayerCharacter::pSingleton
	constexpr uint32_t kFrameAnimTime = 0x011DEA30;  // TESMain::fFrameAnimTime: this frame's game time step (0 while paused)
	constexpr uint32_t kFrameCounter = 0x011A2FE0;   // TESMain::iFrameCounter
	constexpr uint32_t kProcessLists = 0x011E0E80;   // ProcessLists::Singleton
	constexpr uint32_t kWindMagnitude = 0x011AD824;  // BSShaderManager::fWindMagnitude: the weather's wind, 0-1 (Sky::UpdateWind)
	constexpr uint32_t kWindDirection = 0x011F95DC;  // BSShaderManager::kWindDirection (NiPoint2)
	constexpr uint32_t kTES = 0x011DEA10;            // TES::pInstance; its interior cell at 0x34
	constexpr uint32_t kRenderer = 0x011F4748;       // NiRenderer::ms_pkRenderer (NiDX9Renderer)
	constexpr uint32_t kInputControl = 0x011F35CC;   // OSInputGlobals

	inline void* Player() { return *(void**)kPlayer; }
	inline float FrameTime() { return *(float*)kFrameAnimTime; }
	inline int FrameCounter() { return *(int*)kFrameCounter; }
	inline bool InInterior()
	{
		void* tes = *(void**)kTES;
		return tes && At<void*>(tes, 0x34);
	}

	// Whether the race menu (ShowRaceMenu, RaceSexMenu 0x40C) is open: InterfaceManager (0x011D8A80) keeps the open
	// menus' types at 0x114 (10 slots).
	constexpr uint32_t kInterfaceManager = 0x011D8A80;
	inline bool RaceMenuOpen()
	{
		__try {
			void* im = *(void**)kInterfaceManager;
			if (!im) return false;
			for (int i = 0; i < 10; ++i)
				if (At<uint32_t>(im, 0x114 + 4 * i) == 0x40C) return true;
			return false;
		} __except (1) {
			return false;
		}
	}

	inline RE::NiNode* Get3D(void* ref)
	{
		typedef RE::NiAVObject*(__thiscall * Get3D_t)(void*);
		auto* obj = ref ? ((Get3D_t)kRefGet3D)(ref) : nullptr;
		return obj ? obj->AsNode() : nullptr;
	}

	// The player's first-person model (arms, the worn items again), shown in first-person view.
	inline RE::NiNode* PlayerFirstPerson3D(void* player)
	{
		typedef RE::NiAVObject*(__thiscall * Get3DPerson_t)(void*, int);
		auto* obj = player ? ((Get3DPerson_t)kPlayerGet3D)(player, 1) : nullptr;
		return obj ? obj->AsNode() : nullptr;
	}

	// Every actor in high process (the ones the game animates in full), the player excluded:
	// ProcessLists::kAllProcessArrays[0], from its begin to end index.
	template <class F>
	inline void ForEachHighProcessActor(F&& f)
	{
		char* pl = (char*)kProcessLists;
		void** objs = *(void***)(pl + 0x08);
		uint32_t begin = *(uint32_t*)(pl + 0x14), end = *(uint32_t*)(pl + 0x24);
		uint16_t count = *(uint16_t*)(pl + 0x0E);
		if (!objs)
			return;
		end = std::min<uint32_t>(end, count);
		for (uint32_t i = begin; i < end; ++i)
			if (objs[i] && objs[i] != Player())
				f(objs[i]);
	}
}
