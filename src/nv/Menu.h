#pragma once
// In-game settings menu (ImGui on the game's D3D9 device, F7 by default). Every change applies to
// the running simulation immediately; Save writes NVSMPPhysics.ini.

namespace Menu
{
	// Once per frame from the main thread: installs the menu once the renderer exists.
	void Tick();
	// xNVSE's DirectInput hook, for blocking the game's keys while the menu is open.
	void SetInputHook(void* diHookControl);
	void SetKey(int virtualKey);
	bool IsVisible();
	// True once after the menu's Reset button: rebuild every system.
	bool ConsumeReset();
}
