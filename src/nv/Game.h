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
	constexpr uint32_t kFindSkinnedNode = 0x004910D0;      // Animation::FindSkinnedNode(NiNode*): the model has a skinned shape

	// BipedAnim::LoadAndAttachAddOn's (0x004AEED0) FindSkinnedNode call: a skinned add-on model (a weapon) is refused
	// ("MODELS: Tyring to add skinned object ... as an add on to skeleton.") and never attached.
	constexpr uint32_t kAddOnSkinnedCheckCall = 0x004AF0FC;

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
	inline bool MenuOpen(uint32_t type)
	{
		__try {
			void* im = *(void**)kInterfaceManager;
			if (!im) return false;
			for (int i = 0; i < 10; ++i)
				if (At<uint32_t>(im, 0x114 + 4 * i) == type) return true;
			return false;
		} __except (1) {
			return false;
		}
	}
	inline bool RaceMenuOpen() { return MenuOpen(0x40C); }
	// The dialogue menu (DialogMenu 0x3F1): menu mode, so fFrameAnimTime is 0 (TESMain sets it to 0 whenever
	// bInMenuMode), but the speaker keeps animating on the dialogue camera.
	inline bool DialogMenuOpen() { return MenuOpen(0x3F1); }

	// "tfc 1" (ToggleFlyCam with freeze): PlayerCharacter::ToggleFlyCam (0x961E30) calls TESMain::SetFreezeTime
	// (0x961F50), which sets TESMain::bFreezeTime (TESMain* at 0x011DEA0C, +0x7). The frame time keeps running.
	constexpr uint32_t kTESMain = 0x011DEA0C;
	inline bool TimeFrozen()
	{
		__try {
			void* main = *(void**)kTESMain;
			return main && At<uint8_t>(main, 0x7) != 0;
		} __except (1) {
			return false;
		}
	}

	// The game's grab control (UACTN_GRAB 0x1B, Z by default) pressed this frame, read as PlayerCharacter::HandlePhysicsGrab
	// (0x95F6C0) reads it: TESMain::GetControls (0x877720) then Controls::GetUserAction (0xA24660) with isPressed (1). The
	// game's own grab holds something while bIsGrabbing (0x11E0D5C) is set.
	inline bool GrabPressed()
	{
		__try {
			void* main = *(void**)kTESMain;
			void* controls = main ? ((void*(__thiscall*)(void*))0x00877720)(main) : nullptr;
			return controls && ((int(__thiscall*)(void*, int, int))0x00A24660)(controls, 0x1B, 1) != 0;
		} __except (1) {
			return false;
		}
	}
	inline bool GameGrabbing() { return *(const uint8_t*)0x011E0D5C != 0; }

	// The player's eye and view direction as the game's grab aims (Actor::GetEyeVector 0x8AFF80, third person true, as
	// PlayerCharacter::UpdateMouseSpring calls it).
	inline bool EyeVector(RE::NiPoint3& eye, RE::NiPoint3& dir)
	{
		__try {
			void* player = Player();
			if (!player) return false;
			((void(__thiscall*)(void*, RE::NiPoint3*, RE::NiPoint3*, int))0x008AFF80)(player, &eye, &dir, 1);
			return true;
		} __except (1) {
			return false;
		}
	}

	// A reference's place (TESObjectREFR +0x30).
	inline RE::NiPoint3 RefPosition(const void* ref) { return At<RE::NiPoint3>(ref, 0x30); }

	// A projectile reference, by its form type (+0x04): missile (bullets) 0x3D, grenade 0x3E, beam 0x3F, flame 0x40,
	// continuous beam 0x69.
	inline bool IsProjectile(const void* ref)
	{
		const uint8_t type = At<uint8_t>(ref, 0x04);
		return (type >= 0x3D && type <= 0x40) || type == 0x69;
	}

	// An explosion (a MobileObject in the process lists, by its vtable, Explosion::vftable 0x108EE04), and its radius in
	// game units (+0x94, from BGSExplosion::GetRadiusBSUnits; fAge at +0x88, the object 0x104 bytes).
	inline bool IsExplosion(const void* ref) { return At<uint32_t>(ref, 0x00) == 0x108EE04; }
	inline float ExplosionRadius(const void* ref) { return At<float>(ref, 0x94); }

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

	// Every actor in a process list, the player excluded: high, middle-high, middle-low and low process (the game moves
	// actors down the levels with distance; the lower ones are still drawn). ProcessLists' one array holds all four,
	// each level from its begin index (0x14 + 4 * level) to its end index (0x24 + 4 * level).
	template <class F>
	inline void ForEachProcessActor(F&& f)
	{
		char* pl = (char*)kProcessLists;
		void** objs = *(void***)(pl + 0x08);
		uint16_t count = *(uint16_t*)(pl + 0x0E);
		if (!objs)
			return;
		for (int level = 0; level < 4; ++level) {
			uint32_t begin = *(uint32_t*)(pl + 0x14 + 4 * level), end = std::min<uint32_t>(*(uint32_t*)(pl + 0x24 + 4 * level), count);
			for (uint32_t i = begin; i < end; ++i)
				if (objs[i] && objs[i] != Player())
					f(objs[i]);
		}
	}

	// The loaded cells: the interior one (TES +0x34), else the exterior grid's (TES::pGridCellArray +0x08: its size a side
	// at +0x0C, its cells at +0x10, a TESObjectCELL* each).
	template <class F>
	inline void ForEachLoadedCell(F&& f)
	{
		void* tes = *(void**)kTES;
		if (!tes) return;
		if (void* interior = At<void*>(tes, 0x34)) { f(interior); return; }
		void* grid = At<void*>(tes, 0x08);
		if (!grid) return;
		const uint32_t side = At<uint32_t>(grid, 0x0C);
		void** cells = At<void**>(grid, 0x10);
		if (!cells || side > 32) return;
		for (uint32_t i = 0; i < side * side; ++i)
			if (cells[i]) f(cells[i]);
	}

	// A cell's references (TESObjectCELL::kReferences, a BSSimpleList at +0xAC: data, next), walked under the cell's own
	// lock (CellRefLockEnter 0x541AC0 / CellRefLockLeave 0x541AE0): the game adds and removes them on its loading threads.
	// The form type at +0x04 is passed too (0x3B a character, 0x3C a creature).
	template <class F>
	inline void ForEachReference(void* cell, F&& f)
	{
		typedef void(__thiscall * Lock_t)(void*);
		((Lock_t)0x541AC0)(cell);
		struct Node { void* data; Node* next; };
		for (const Node* n = (const Node*)((char*)cell + 0xAC); n; n = n->next)
			if (n->data) f(n->data, At<uint8_t>(n->data, 0x04));
		((Lock_t)0x541AE0)(cell);
	}
}
