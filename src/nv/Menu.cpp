// In-game settings menu. ImGui drawn on the game's D3D9 device just before it presents, with
// input handled the way NVR's menu does it: keys and mouse buttons polled (window messages are
// unreliable under DXVK, and the game registers raw input without legacy messages), the game's
// DirectInput mouse muted and its keys blocked through xNVSE while the menu is open.
#include "Game.h"
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "ActorManager.h"
#include "Menu.h"
#include "Settings.h"
#include "hdtSkyrimPhysicsWorld.h"
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace nvsmp { void Log(const char* fmt, ...); }
using nvsmp::Log;

namespace Menu {

static bool sResetPhysics = false;

static const uint32_t kRenderer = 0x011F4748;       // NiRenderer::ms_pkRenderer (NiDX9Renderer)
static const uint32_t kInputControl = 0x011F35CC;   // OSInputGlobals; DirectInput mouse at 0x30

static bool sInit = false, sVisible = false;
static IDirect3DDevice9* sDevice = nullptr;
static HWND sWnd = nullptr;
static WNDPROC sOrigWndProc = nullptr;
static int sKey = VK_F7;
static void* sDIHook = nullptr;
static BYTE sPrevKeys[256];
static float sWheel = 0;

typedef HRESULT(WINAPI* Present_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(WINAPI* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(WINAPI* GetDeviceState_t)(IDirectInputDevice8*, DWORD, LPVOID);
static Present_t sPresent = nullptr;
typedef HRESULT(WINAPI* SwapPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
static SwapPresent_t sSwapPresent = nullptr;
static int sDrawnFrame = -1, sPresentCalls = 0;
static Reset_t sReset = nullptr;
static GetDeviceState_t sGetDeviceState = nullptr;

void SetInputHook(void* h) { sDIHook = h; }
void SetKey(int vk) { if (vk > 0 && vk < 256) sKey = vk; }
bool IsVisible() { return sVisible; }
bool ConsumeReset() { bool r = sResetPhysics; sResetPhysics = false; return r; }

// xNVSE DIHookControl: a vtable pointer, then 266 KeyInfo of 7 bools; userDisable is the 6th.
static void BlockGameKeys(bool block) {
	if (!sDIHook) return;
	bool* keys = (bool*)((char*)sDIHook + 4);
	static const int kModifiers[] = { 0x2A, 0x36, 0x1D, 0x9D, 0x38, 0xB8 };   // shifts, ctrls, alts: keep (sprint etc.)
	for (int code = 0; code < 266; code++) {
		bool mod = false;
		for (int m : kModifiers) if (code == m) mod = true;
		if (!mod) keys[code * 7 + 5] = block;
	}
}

// The game reads the mouse through DirectInput; while the menu is open it gets nothing (the
// wheel goes to the menu instead).
static HRESULT WINAPI GetDeviceStateHook(IDirectInputDevice8* device, DWORD size, LPVOID data) {
	HRESULT hr = sGetDeviceState(device, size, data);
	if (SUCCEEDED(hr) && sVisible && size == sizeof(DIMOUSESTATE2)) {
		sWheel += ((DIMOUSESTATE2*)data)->lZ / (float)WHEEL_DELTA;
		memset(data, 0, size);
	}
	return hr;
}

static void PatchVTable(void** vtable, int index, void* hook, void** original) {
	DWORD old;
	VirtualProtect(&vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
	*original = vtable[index];
	vtable[index] = hook;
	VirtualProtect(&vtable[index], sizeof(void*), old, &old);
}

static void PatchMouse() {
	if (sGetDeviceState) return;   // once: the vtable belongs to the class, and re-patching over another mod's hook would loop
	void* input = *(void**)kInputControl;
	if (!input) return;
	void* mouse = Game::At<void*>(input, 0x30);
	if (!mouse) return;
	PatchVTable(*(void***)mouse, 9, (void*)GetDeviceStateHook, (void**)&sGetDeviceState);
}

static void SetVisible(bool visible) {
	if (sVisible == visible) return;
	sVisible = visible;
	ImGuiIO& io = ImGui::GetIO();
	if (visible) {
		for (int vk = 0; vk < 256; vk++) sPrevKeys[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 0x80 : 0;
		PatchMouse();
		ClipCursor(nullptr);
		io.ClearInputKeys();
	}
	BlockGameKeys(visible);
	io.MouseDrawCursor = visible;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	if (msg == WM_ACTIVATE && LOWORD(wParam) == WA_INACTIVE) SetVisible(false);
	if (sVisible && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return TRUE;
	if (sVisible && (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)) return 0;
	return CallWindowProc(sOrigWndProc, hwnd, msg, wParam, lParam);
}

static ImGuiKey ToImGuiKey(int vk) {
	switch (vk) {
	case VK_TAB: return ImGuiKey_Tab; case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow;
	case VK_UP: return ImGuiKey_UpArrow; case VK_DOWN: return ImGuiKey_DownArrow; case VK_HOME: return ImGuiKey_Home;
	case VK_END: return ImGuiKey_End; case VK_DELETE: return ImGuiKey_Delete; case VK_BACK: return ImGuiKey_Backspace;
	case VK_RETURN: return ImGuiKey_Enter; case VK_ESCAPE: return ImGuiKey_Escape;
	case VK_LCONTROL: return ImGuiKey_LeftCtrl; case VK_RCONTROL: return ImGuiKey_RightCtrl;
	case VK_LSHIFT: return ImGuiKey_LeftShift; case VK_RSHIFT: return ImGuiKey_RightShift;
	case 'A': return ImGuiKey_A; case 'C': return ImGuiKey_C; case 'V': return ImGuiKey_V; case 'X': return ImGuiKey_X; case 'Z': return ImGuiKey_Z;
	default: return ImGuiKey_None;
	}
}

// Keys and mouse by polling.
static void PollInput() {
	ImGuiIO& io = ImGui::GetIO();
	for (int vk = 1; vk < 256; vk++) {
		if (vk == sKey) continue;
		bool cur = (GetAsyncKeyState(vk) & 0x8000) != 0, was = (sPrevKeys[vk] & 0x80) != 0;
		if (cur == was) continue;
		sPrevKeys[vk] = cur ? 0x80 : 0;
		ImGuiKey k = ToImGuiKey(vk);
		if (k != ImGuiKey_None) io.AddKeyEvent(k, cur);
		if (vk == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, cur);
		if (vk == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, cur);
		if (!cur) continue;
		WCHAR ch = 0;
		if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) ch = (WCHAR)(L'0' + vk - VK_NUMPAD0);
		else if (vk == VK_DECIMAL) ch = L'.';
		else if (vk == VK_SUBTRACT) ch = L'-';
		if (ch) { io.AddInputCharacterUTF16(ch); continue; }
		BYTE ks[256] = {}; WCHAR buf[4] = {};
		ks[VK_SHIFT] = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 0x80 : 0;
		ks[VK_CONTROL] = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 0x80 : 0;
		ks[vk] = 0x80;
		if (ks[VK_CONTROL]) continue;
		int n = ToUnicode(vk, MapVirtualKey(vk, MAPVK_VK_TO_VSC), ks, buf, 4, 0);
		for (int i = 0; i < n; i++) io.AddInputCharacterUTF16(buf[i]);
	}
	POINT pt; RECT cr;
	if (GetCursorPos(&pt) && GetClientRect(sWnd, &cr)) {
		ScreenToClient(sWnd, &pt);
		if (pt.x >= cr.left && pt.x < cr.right && pt.y >= cr.top && pt.y < cr.bottom) io.AddMousePosEvent((float)pt.x, (float)pt.y);
	}
	io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
	io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
	if (sWheel != 0) { io.AddMouseWheelEvent(0, sWheel); sWheel = 0; }
}

static void Help(const char* text) {
	ImGui::SameLine(); ImGui::TextDisabled("(?)");
	if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

static void BuildUI() {
	using namespace nvsmp;
	Settings& s = gSettings;
	auto* world = hdt::SkyrimPhysicsWorld::get();
	const auto& stats = ActorManager::instance()->stats;
	ImGui::SetNextWindowSize(ImVec2(470, 560), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
	bool open = true;
	if (!ImGui::Begin("NV SMP Physics", &open)) { ImGui::End(); if (!open) SetVisible(false); return; }
	ImGui::PushItemWidth(-190);
	bool changed = false;

	ImGui::Text("%d actor(s), %d system(s), %d rigid bodies, %.2f ms", stats.actors, stats.systems, world->bodyCount(), world->m_averageStepMs);
	if (stats.failed) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "%d item(s) failed to build: see NVSMPPhysics.log", stats.failed);
	ImGui::Separator();

	if (ImGui::CollapsingHeader("Simulation", ImGuiTreeNodeFlags_DefaultOpen)) {
		changed |= ImGui::Checkbox("Enabled", &s.enabled); Help("Off: every item follows its animated pose.");
		changed |= ImGui::SliderFloat("Gravity", &s.gravityScale, 0.0f, 3.0f, "%.2f x"); Help("1 = Earth's.");
		changed |= ImGui::SliderInt("Rate (min fps)", &s.minFps, 30, 240); Help("Physics steps per second, fixed whatever the frame rate (the cloth behaves the same at any fps). Higher = smoother and more accurate, and more expensive.");
		changed |= ImGui::SliderInt("Max substeps", &s.maxSubSteps, 1, 16); Help("A frame longer than this many 60 Hz frames (4 = 1/15 s) drops the rest instead of catching up: the cloth slows down rather than the game.");
		changed |= ImGui::SliderInt("Solver iterations", &s.solverIterations, 1, 64); Help("Higher = stiffer constraints, more expensive.");
		changed |= ImGui::Checkbox("Collision threads", &s.collisionThreadsOn); Help("Spread the collision tests over extra threads. Off: they all run on the physics thread, as before.");
		if (s.collisionThreadsOn) { changed |= ImGui::SliderInt("Thread count", &s.collisionThreads, -1, 8); Help("Extra threads the collision tests are spread over (the physics thread works too). -1 = auto (about half the cores minus one, at most 4), 0 = none."); }
		changed |= ImGui::Checkbox("Collisions", &s.collisions); Help("Off: the cloth passes through the body. For telling whether a collision shape pushes it.");
		changed |= ImGui::Checkbox("Collide once per frame", &s.collideOncePerFrame); Help("The collision tests run on the frame's first physics step only; the frame's other steps reuse those contacts, moved with the body. Cheaper (the more steps a frame has, the more it saves); fast moves can push the cloth through a little more easily.");
		changed |= ImGui::Checkbox("Ground collision", &s.ground); Help("The cloth rests on the ground at the actor's feet instead of sinking through it when crouching or sitting. Changing it rebuilds the physics.");
		changed |= ImGui::Checkbox("Jitter filter", &s.jitterFilter); Help("Smooths the small fast shaking of the cloth (what is drawn, not the physics): a bone barely moving is held steady, a swinging one passes through.");
		if (s.jitterFilter) { changed |= ImGui::SliderFloat("Jitter smoothing", &s.jitterSmoothing, 0.0f, 1.0f, "%.2f"); Help("0 = light, follows every twitch. 1 = strong: still cloth is rock steady, and settles a little slowly after a move."); }
		changed |= ImGui::Checkbox("Reset on fast turns", &s.unclampedResets); Help("Turning faster than the angle below (per second) starts the physics over in its rest pose, as after a teleport.");
		changed |= ImGui::SliderFloat("Reset angle", &s.resetAngle, 10.0f, 360.0f, "%.0f");
	}
	if (ImGui::CollapsingHeader("Wind", ImGuiTreeNodeFlags_DefaultOpen)) {
		changed |= ImGui::Checkbox("Weather wind", &s.wind); Help("The same wind as the grass and trees, with FSMP's gusts.");
		changed |= ImGui::SliderFloat("Wind strength", &s.windStrength, 0.0f, 10.0f, "%.2f"); Help("At the windiest weather, compared with 9.8 for gravity (FSMP's wind strength).");
		changed |= ImGui::Checkbox("Wind in interiors", &s.windInInteriors); Help("Indoors the game keeps the last outdoor wind.");
		ImGui::TextDisabled("Weather wind now: %.0f%%%s", *(float*)Game::kWindMagnitude * 100.0f, Game::InInterior() ? ", interior" : "");
	}
	if (ImGui::CollapsingHeader("Performance")) {
		changed |= ImGui::Checkbox("Step on its own thread", &s.asyncStep); Help("The simulation runs on a thread of its own while the game draws the frame, instead of holding the frame up. The cloth answers the body's moves a frame later.");
		ImGui::TextDisabled("Game thread: %.2f ms of %.2f ms", world->m_averageMainMs, world->m_averageStepMs);
		changed |= ImGui::SliderFloat("Collision distance", &s.collisionDistance, 0.0f, 20000.0f, "%.0f"); Help("Actors farther than this keep their physics (swing, gravity) but without collision, the expensive part: cloth may pass through the body, which hardly shows at that distance. The player always collides.");
		changed |= ImGui::SliderInt("Max actors", &s.maxActors, 1, 40); Help("Most actors with collision at once, nearest first. The others keep their physics without collision.");
	}
	if (changed) ApplySettings();
	ImGui::Separator();
	if (ImGui::Button("Reset physics")) sResetPhysics = true;
	ImGui::SameLine(); if (ImGui::Button("Save to INI")) SaveSettings();
	ImGui::SameLine(); if (ImGui::Button("Reload INI")) LoadSettings();
	ImGui::SameLine(); if (ImGui::Button("Defaults")) DefaultSettings();
	ImGui::TextDisabled("F7 or the X closes this menu. Changes apply immediately.");
	ImGui::PopItemWidth();
	ImGui::End();
	if (!open) SetVisible(false);
}

static void Draw() {
	// Toggle on the key's press, only while the game has focus.
	static bool prev = false;
	DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(sKey) & 0x8000) != 0;
	if (!down) {   // also from the game's DirectInput keyboard (OSInputGlobals current key state at 0x18F8)
		void* input = *(void**)kInputControl;
		UINT dik = MapVirtualKeyA(sKey, MAPVK_VK_TO_VSC);
		if (input && dik && dik < 256) down = Game::At<uint8_t>(input, 0x18F8 + dik) != 0;
	}
	if (down && !prev) { SetVisible(!sVisible); Log("menu %s", sVisible ? "opened" : "closed"); }
	prev = down;
	if (!sVisible) return;
	if (sDevice->TestCooperativeLevel() != D3D_OK) return;

	PollInput();
	ImGui_ImplDX9_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	BuildUI();
	ImGui::EndFrame();
	ImGui::Render();
	if (SUCCEEDED(sDevice->BeginScene())) {
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		sDevice->EndScene();
	}
}

// Once per game frame, from whichever present the game uses.
static void DrawOnce(const char* via) {
	int frame = *(int*)0x011A2FE0;   // TESMain::iFrameCounter
	if (frame == sDrawnFrame) return;
	sDrawnFrame = frame;
	if (sPresentCalls++ < 3) Log("menu: present via %s (frame %d)", via, frame);
	Draw();
}

static HRESULT WINAPI PresentHook(IDirect3DDevice9* device, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty) {
	if (device == sDevice) DrawOnce("device");
	return sPresent(device, src, dst, wnd, dirty);
}

static HRESULT WINAPI SwapPresentHook(IDirect3DSwapChain9* chain, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty, DWORD flags) {
	DrawOnce("swap chain");
	return sSwapPresent(chain, src, dst, wnd, dirty, flags);
}

static HRESULT WINAPI ResetHook(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
	if (device == sDevice) ImGui_ImplDX9_InvalidateDeviceObjects();
	HRESULT hr = sReset(device, params);
	if (device == sDevice && SUCCEEDED(hr)) ImGui_ImplDX9_CreateDeviceObjects();
	return hr;
}

void Tick() {
	if (sInit) return;
	void* renderer = *(void**)kRenderer;
	if (!renderer) return;
	sDevice = Game::At<IDirect3DDevice9*>(renderer, 0x288);
	if (!sDevice) return;
	D3DDEVICE_CREATION_PARAMETERS cp = {};
	if (FAILED(sDevice->GetCreationParameters(&cp))) { Log("menu: the device at renderer+0x288 does not answer"); sInit = true; return; }
	sWnd = cp.hFocusWindow ? cp.hFocusWindow : Game::At<HWND>(renderer, 0x3C0);
	if (!sWnd) return;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	ImGui::StyleColorsDark();
	ImGuiStyle& st = ImGui::GetStyle();
	st.WindowRounding = 4; st.FrameRounding = 3; st.GrabRounding = 3;
	ImGui_ImplWin32_Init(sWnd);
	ImGui_ImplDX9_Init(sDevice);
	sOrigWndProc = (WNDPROC)SetWindowLongPtrA(sWnd, GWLP_WNDPROC, (LONG_PTR)WndProc);
	void** vtable = *(void***)sDevice;
	PatchVTable(vtable, 16, (void*)ResetHook, (void**)&sReset);
	PatchVTable(vtable, 17, (void*)PresentHook, (void**)&sPresent);
	IDirect3DSwapChain9* chain = nullptr;
	if (SUCCEEDED(sDevice->GetSwapChain(0, &chain)) && chain) {
		PatchVTable(*(void***)chain, 3, (void*)SwapPresentHook, (void**)&sSwapPresent);
		chain->Release();
	}
	char title[128] = ""; GetWindowTextA(sWnd, title, sizeof title);
	Log("menu: ready on device %p, window %p '%s', key 0x%X, xNVSE input %s", sDevice, sWnd, title, sKey, sDIHook ? "yes" : "no");
	sInit = true;
}

}
