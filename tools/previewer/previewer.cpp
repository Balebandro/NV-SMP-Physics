// NV SMP Previewer: an SMP item (a NIF with "HDT Skinned Mesh Physics Object" and its physics XML)
// on a moving body, outside the game. The item is laid out in memory as New Vegas holds it (tools/
// scene.cpp) and simulated by the plugin's own system builder and physics world, so what it shows is
// what the game does. Edit the XML in any editor and save: the previewer rebuilds it.
//
//   NVSMPPreviewer.exe [item.nif]
//
// Files the NIF names (the physics XML, textures) are looked up in the item's own Data folder, Mod
// Organizer 2's enabled mods in priority order, then the game's Data folder (loose files only).
#include "datafiles.h"
#include "kf.h"
#include "scene.h"
#include "xmledit.h"
#include "backup.h"
#include "nifclean.h"

#include "hdtSkinnedMesh/hdtSkinnedMeshShape.h"
#include "hdtSkyrimBone.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <commdlg.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <array>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace fs = std::filesystem;

// --- the log --------------------------------------------------------------------------------------------

struct LogEntry
{
	int level;
	std::string text;
};
static std::deque<LogEntry> gLog;
static bool gShotPending = false;   // the Screenshot button: the next frame, without the panels
static bool gLogScroll = false;

static void AddLog(int level, const std::string& text)
{
	gLog.push_back({ level, text });	while (gLog.size() > 2000) gLog.pop_front();
	gLogScroll = true;
	OutputDebugStringA((text + "\n").c_str());
}

namespace nvsmp
{
	bool LogEnabled(LogLevel level) { return level >= LogLevel::Info; }
	void LogLine(LogLevel level, const std::string& text)
	{
		// The builder's per-bone chatter ("Found node", "OK: constraint") only at the verbose level.
		if (level == LogLevel::Info && (text.rfind("Found node", 0) == 0 || text.rfind("OK: constraint", 0) == 0 || text.rfind("Created bone node", 0) == 0))
			return;
		AddLog((int)level, text);
	}
}

// --- small math (Z up, right-handed, as the game) ------------------------------------------------------

struct V3
{
	float x = 0, y = 0, z = 0;
	V3() = default;
	V3(float a, float b, float c) : x(a), y(b), z(c) {}
	V3(const RE::NiPoint3& p) : x(p.x), y(p.y), z(p.z) {}
	V3 operator+(V3 o) const { return { x + o.x, y + o.y, z + o.z }; }
	V3 operator-(V3 o) const { return { x - o.x, y - o.y, z - o.z }; }
	V3 operator*(float s) const { return { x * s, y * s, z * s }; }
};
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static V3 Norm(V3 a) { float l = std::sqrt(Dot(a, a)); return l > 1e-8f ? a * (1.0f / l) : V3(0, 0, 1); }

// Row-vector matrices (v' = v * M), as Direct3D 9's fixed function and the shaders (row_major) use.
struct M4
{
	float m[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
};
static M4 Mul(const M4& a, const M4& b)
{
	M4 r;
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++) {
			r.m[i][j] = 0;
			for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
		}
	return r;
}
static M4 LookAtRH(V3 eye, V3 at, V3 up)
{
	V3 z = Norm(eye - at), x = Norm(Cross(up, z)), y = Cross(z, x);
	M4 r;
	r.m[0][0] = x.x; r.m[0][1] = y.x; r.m[0][2] = z.x; r.m[0][3] = 0;
	r.m[1][0] = x.y; r.m[1][1] = y.y; r.m[1][2] = z.y; r.m[1][3] = 0;
	r.m[2][0] = x.z; r.m[2][1] = y.z; r.m[2][2] = z.z; r.m[2][3] = 0;
	r.m[3][0] = -Dot(x, eye); r.m[3][1] = -Dot(y, eye); r.m[3][2] = -Dot(z, eye); r.m[3][3] = 1;
	return r;
}
static M4 PerspectiveRH(float fovY, float aspect, float zn, float zf)
{
	float ys = 1.0f / std::tan(fovY / 2), xs = ys / aspect;
	M4 r;
	r.m[0][0] = xs; r.m[1][1] = ys;
	r.m[2][2] = zf / (zn - zf); r.m[2][3] = -1;
	r.m[3][2] = zn * zf / (zn - zf); r.m[3][3] = 0;
	return r;
}

// --- Direct3D --------------------------------------------------------------------------------------------

static HWND gWnd;
static IDirect3D9* gD3D;
static IDirect3DDevice9* gDev;
static D3DPRESENT_PARAMETERS gPP;
static UINT gResizeW, gResizeH;
static IDirect3DVertexShader9* gVS;
static IDirect3DPixelShader9* gPS;
static IDirect3DVertexDeclaration9* gDecl;

struct MeshVertex
{
	float px, py, pz, nx, ny, nz, u, v;
};
struct LineVertex
{
	float x, y, z;
	D3DCOLOR c;
};

static const char* kShader = R"(
row_major float4x4 gViewProj : register(c0);
struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD0; };
VSOut vs(VSIn i) { VSOut o; o.pos = mul(float4(i.pos, 1), gViewProj); o.nrm = i.nrm; o.uv = i.uv; return o; }

float4 gKey : register(c0);      // key light direction (toward the light)
float4 gColor : register(c1);    // tint
float4 gParams : register(c2);   // x: textured, y: alpha test, z: unlit
float4 gFill : register(c3);     // fill light direction
sampler2D gTex : register(s0);
float4 ps(VSOut i) : COLOR
{
	float4 base = gParams.x > 0.5 ? tex2D(gTex, i.uv) : float4(1, 1, 1, 1);
	if (gParams.y > 0.5) clip(base.a - 0.5);
	base *= gColor;
	if (gParams.z > 0.5) return base;
	float3 n = normalize(i.nrm);
	// Two-sided: cloth is seen from inside too.
	float key = abs(dot(n, gKey.xyz)), fill = abs(dot(n, gFill.xyz));
	float light = 0.28 + 0.62 * key + 0.22 * fill;
	return float4(base.rgb * light, 1);
}
)";

static bool CompileShaders()
{
	ID3DBlob *code = nullptr, *err = nullptr;
	if (FAILED(D3DCompile(kShader, strlen(kShader), "previewer", nullptr, nullptr, "vs", "vs_3_0", 0, 0, &code, &err))) {
		AddLog(4, std::string("vertex shader: ") + (err ? (const char*)err->GetBufferPointer() : "?"));
		return false;
	}
	gDev->CreateVertexShader((DWORD*)code->GetBufferPointer(), &gVS);
	code->Release();
	if (FAILED(D3DCompile(kShader, strlen(kShader), "previewer", nullptr, nullptr, "ps", "ps_3_0", 0, 0, &code, &err))) {
		AddLog(4, std::string("pixel shader: ") + (err ? (const char*)err->GetBufferPointer() : "?"));
		return false;
	}
	gDev->CreatePixelShader((DWORD*)code->GetBufferPointer(), &gPS);
	code->Release();
	D3DVERTEXELEMENT9 el[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
		{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		D3DDECL_END()
	};
	gDev->CreateVertexDeclaration(el, &gDecl);
	return true;
}

static bool CreateDevice()
{
	gD3D = Direct3DCreate9(D3D_SDK_VERSION);
	if (!gD3D) return false;
	ZeroMemory(&gPP, sizeof(gPP));
	gPP.Windowed = TRUE;
	gPP.SwapEffect = D3DSWAPEFFECT_DISCARD;
	gPP.BackBufferFormat = D3DFMT_UNKNOWN;
	gPP.EnableAutoDepthStencil = TRUE;
	gPP.AutoDepthStencilFormat = D3DFMT_D24S8;
	gPP.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
	DWORD q = 0;
	if (SUCCEEDED(gD3D->CheckDeviceMultiSampleType(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES, &q)))
		gPP.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;
	return SUCCEEDED(gD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, gWnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &gPP, &gDev));
}

// The back buffer as a 32-bit BMP (resolved through a plain render target: a multisampled back buffer
// cannot be read directly).
static bool SaveScreenshot(const std::string& path)
{
	IDirect3DSurface9 *back = nullptr, *resolved = nullptr, *sys = nullptr;
	bool ok = false;
	D3DSURFACE_DESC d;
	if (SUCCEEDED(gDev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && SUCCEEDED(back->GetDesc(&d)) &&
		SUCCEEDED(gDev->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
		SUCCEEDED(gDev->StretchRect(back, nullptr, resolved, nullptr, D3DTEXF_NONE)) &&
		SUCCEEDED(gDev->CreateOffscreenPlainSurface(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
		SUCCEEDED(gDev->GetRenderTargetData(resolved, sys))) {
		D3DLOCKED_RECT lr;
		if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
			FILE* f = fopen(path.c_str(), "wb");
			if (f) {
				BITMAPFILEHEADER fh{};
				BITMAPINFOHEADER ih{};
				ih.biSize = sizeof(ih);
				ih.biWidth = d.Width;
				ih.biHeight = -(LONG)d.Height;   // top-down
				ih.biPlanes = 1;
				ih.biBitCount = 32;
				fh.bfType = 0x4D42;
				fh.bfOffBits = sizeof(fh) + sizeof(ih);
				fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
				fwrite(&fh, sizeof(fh), 1, f);
				fwrite(&ih, sizeof(ih), 1, f);
				for (UINT y = 0; y < d.Height; y++) fwrite((char*)lr.pBits + y * lr.Pitch, 4, d.Width, f);
				fclose(f);
				ok = true;
			}
			sys->UnlockRect();
		}
	}
	if (back) back->Release();
	if (resolved) resolved->Release();
	if (sys) sys->Release();
	return ok;
}

static void ResetDevice()
{
	ImGui_ImplDX9_InvalidateDeviceObjects();
	if (gDev->Reset(&gPP) == D3DERR_INVALIDCALL) AddLog(4, "device reset failed");
	ImGui_ImplDX9_CreateDeviceObjects();
}

// --- settings (NVSMPPreviewer.ini next to the exe) -------------------------------------------------------

static std::string gIniPath;

static std::string IniGet(const char* key, const char* def = "")
{
	char buf[1024];
	GetPrivateProfileStringA("Previewer", key, def, buf, sizeof buf, gIniPath.c_str());
	return buf;
}
static void IniSet(const char* key, const std::string& value) { WritePrivateProfileStringA("Previewer", key, value.c_str(), gIniPath.c_str()); }

// The Data sources set by hand (empty: detected), kept in the INI.
static datafiles::Options gSources;

static void LoadSources()
{
	gSources.mo2Instance = IniGet("Mo2Instance");
	gSources.mo2Profile = IniGet("Mo2Profile");
	gSources.gameFolder = IniGet("GameFolder");
	gSources.useArchives = IniGet("UseArchives", "1") != "0";
	gSources.extraFolders.clear();
	std::stringstream extra(IniGet("ExtraFolders"));
	std::string f;
	while (std::getline(extra, f, '|'))
		if (!f.empty()) gSources.extraFolders.push_back(f);
}

static void SaveSources()
{
	IniSet("Mo2Instance", gSources.mo2Instance);
	IniSet("Mo2Profile", gSources.mo2Profile);
	IniSet("GameFolder", gSources.gameFolder);
	IniSet("UseArchives", gSources.useArchives ? "1" : "0");
	std::string extra;
	for (auto& f : gSources.extraFolders) extra += (extra.empty() ? "" : "|") + f;
	IniSet("ExtraFolders", extra);
}

// --- the loaded item ----------------------------------------------------------------------------------------

struct ShapeDraw
{
	scene::ShapeInfo info;
	scene::Weights weights;
	IDirect3DTexture9* texture = nullptr;
	std::string texturePath, textureNote;
	bool visible = true;
	int collider = 0;   // 0 none, 1 per-vertex (cloth), 2 per-triangle (body)
	std::vector<MeshVertex> verts;
	std::vector<uint16_t> indices;
};

struct Settings
{
	// Physics (the plugin's F7 menu).
	bool physics = true;
	float gravity = 1.0f;
	int minFps = 180;   // as the plugin's default
	int maxSubSteps = 4;
	int solverIterations = 10;
	int collisionThreads = -1;   // as the plugin's iCollisionThreads
	bool collisions = true;
	bool collideOncePerFrame = false;   // as the plugin's bCollideOncePerFrame
	float waistCollision = 0.0f;
	bool ground = true;
	bool jitterFilter = true;     // the plugin's jitter filter (SkyrimBone::SetJitterFilter)
	float jitterSmoothing = 0.5f;
	float jitterBeta = -1;        // --beta: the filter's speed term, for tuning (-1: the plugin's)
	bool wind = false;
	float windStrength = 2.0f;
	float windDirection = 0.0f;   // degrees, 0 = toward +y (the body's front at start)
	bool windGusts = true;
	// Motion.
	int preset = 1;
	float speedScale = 1.0f;
	float timeScale = 1.0f;
	// Display.
	bool textures = true;
	bool wireframe = false;
	bool showBones = false;
	bool showConstraints = false;
	bool showColliders = false;
	bool showProxies = false;   // the hidden "SMP Proxy:" collision stand-ins, as a wireframe
	std::string heatBone;        // Weights: the bone whose weights the item shows as a heat map (empty: off)
	bool showFloor = true;
	bool autoReload = true;
	float background[3] = { 0.17f, 0.18f, 0.20f };
} gS;

struct Item
{
	bool loaded = false;
	std::string nifPath, xmlPath, xmlNamed;
	fs::file_time_type nifTime, xmlTime;
	datafiles::Roots roots;
	scene::Item scene;
	RE::BSTSmartPointer<hdt::SkyrimSystem> system;
	std::vector<ShapeDraw> shapes;
	int simulatedBones = 0, constraintCount = 0;
	float builtWaist = -1;
	// The body's animated bones and their rest locals.
	RE::NiNode *pelvis = nullptr, *lThigh = nullptr, *rThigh = nullptr, *lCalf = nullptr, *rCalf = nullptr;
	RE::NiTransform pelvis0, lThigh0, rThigh0, lCalf0, rCalf0;
} gItem;

static std::unordered_map<std::string, IDirect3DTexture9*> gTextureCache;

static fs::file_time_type TimeOf(const std::string& p)
{
	std::error_code ec;
	auto t = fs::last_write_time(p, ec);
	return ec ? fs::file_time_type{} : t;
}

static void ApplyWorldSettings()
{
	auto* world = hdt::SkyrimPhysicsWorld::get();
	hdt::SkyrimBone::SetJitterFilter(gS.jitterFilter, gS.jitterSmoothing, gS.jitterBeta);
	gS.minFps = std::clamp(gS.minFps, 30, 240);
	gS.maxSubSteps = std::clamp(gS.maxSubSteps, 1, 16);
	gS.solverIterations = std::clamp(gS.solverIterations, 1, 64);
	world->m_timeTick = 1.0f / (float)gS.minFps;
	world->m_maxSubSteps = gS.maxSubSteps;
	world->getSolverInfo().m_numIterations = gS.solverIterations;
	gS.collisionThreads = std::clamp(gS.collisionThreads, -1, 8);
	tbb::SetWorkers(gS.collisionThreads < 0 ? tbb::AutoWorkers() : gS.collisionThreads);
	world->setGravity(btVector3(0, 0, -9.8f * hdt::scaleSkyrim * gS.gravity));
	world->m_enableWind = gS.wind;
	world->m_windStrength = gS.windStrength;
	world->m_collisionsEnabled = gS.collisions;
	world->m_collideOncePerFrame = gS.collideOncePerFrame;
	world->m_unclampedResets = false;   // the previewer's turns are deliberate
}

// The physics file's numbers as the Bone values panel has them (xmledit.h).
static xmledit::Doc gXml;
static std::string gXmlPath;
static bool gOpenValues = false;   // --values: the panel starts open (for screenshots)
static bool gAsyncStep = false;    // --async: the step on the physics thread, as the plugin's bAsyncStep (testing)

// The XML's collision meshes (per-triangle-shape) the NIF has no low-poly stand-in for ("SMP Proxy:<name>", smpdress
// --proxy): skinned in full for every contact, they cost far more in game. Names as the NIF has them.
static std::vector<std::string> gMissingProxies;
static bool gXmlArchived = false;   // the XML was found inside a BSA (it cannot be rewritten in place)
static std::string gXmlFixPath;     // the NIF names an XML the game will not find, one is next to the NIF: its path from Data

static std::vector<std::string> MissingProxies(const std::string& xmlText)
{
	std::vector<std::string> out;
	std::regex re("<per-triangle-shape[^>]*\\bname\\s*=\\s*\"([^\"]+)\"", std::regex::icase);
	std::set<std::string> names;
	for (auto& s : gItem.shapes) if (s.info.geometry->m_name) names.insert(s.info.geometry->m_name);
	auto has = [&](const std::string& n) { for (auto& m : names) if (_stricmp(m.c_str(), n.c_str()) == 0) return m; return std::string(); };
	for (std::sregex_iterator it(xmlText.begin(), xmlText.end(), re), end; it != end; ++it) {
		std::string n = (*it)[1].str();
		if (_strnicmp(n.c_str(), "SMP Proxy:", 10) == 0) continue;
		std::string shape = has(n);
		if (shape.empty() || !has("SMP Proxy:" + n).empty()) continue;   // not in the NIF (a virtual body's), or done
		bool skinned = false;
		for (auto& s : gItem.shapes) if (s.info.geometry->m_name && shape == s.info.geometry->m_name) skinned = s.info.geometry->skinInstance != nullptr;
		if (skinned && std::find(out.begin(), out.end(), shape) == out.end()) out.push_back(shape);
	}
	return out;
}

// The NIF's proxies ("SMP Proxy:<name>") the XML's collision shapes do not use, each paired with the XML name it stands
// in for when that is clear: the full mesh it was made from ("NoHat" for "SMP Proxy:NoHat"), a proxy name the NIF no
// longer has that it starts with or that starts with it ("SMP Proxy:Head" for "SMP Proxy:HeadM"), or the last ones
// left on each side. Unpaired proxies come back with an empty XML name.
static std::vector<std::pair<std::string, std::string>> gProxyRenames;   // { XML name, NIF proxy }
static std::regex ShapeNameRegex() { return std::regex("(<per-(?:vertex|triangle)-shape[^>]*\\bname\\s*=\\s*\")([^\"]+)(\")", std::regex::icase); }

static std::vector<std::pair<std::string, std::string>> UnusedProxies(const std::string& xmlText)
{
	auto same = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; };
	auto isProxy = [](const std::string& n) { return _strnicmp(n.c_str(), "SMP Proxy:", 10) == 0; };
	std::vector<std::string> xmlNames, nifNames;
	const std::regex re = ShapeNameRegex();
	for (std::sregex_iterator it(xmlText.begin(), xmlText.end(), re), end; it != end; ++it) xmlNames.push_back((*it)[2].str());
	for (auto& s : gItem.shapes) if (s.info.geometry->m_name) nifNames.push_back(s.info.geometry->m_name);
	auto in = [&](const std::vector<std::string>& v, const std::string& n) { for (auto& m : v) if (same(m, n)) return true; return false; };

	std::vector<std::string> unused, stale;   // proxies the XML does not name; proxy names in the XML the NIF does not have
	for (auto& n : nifNames) if (isProxy(n) && !in(xmlNames, n) && !in(unused, n)) unused.push_back(n);
	for (auto& n : xmlNames) if (isProxy(n) && !in(nifNames, n) && !in(stale, n)) stale.push_back(n);

	std::vector<std::pair<std::string, std::string>> out;
	auto take = [&](std::vector<std::string>& v, const std::string& n) { for (auto it = v.begin(); it != v.end(); ++it) if (same(*it, n)) { v.erase(it); return; } };
	for (auto& p : std::vector<std::string>(unused)) {
		const std::string full = p.substr(10);
		if (in(xmlNames, full)) { out.push_back({ full, p }); take(unused, p); continue; }
		std::string match;
		int matches = 0;
		for (auto& s : stale) {
			const std::string a = s.substr(10);
			if (_strnicmp(a.c_str(), full.c_str(), std::min(a.size(), full.size())) == 0) { match = s; ++matches; }
		}
		if (matches == 1) { out.push_back({ match, p }); take(unused, p); take(stale, match); }
	}
	if (unused.size() == 1 && stale.size() == 1) { out.push_back({ stale[0], unused[0] }); unused.clear(); }
	for (auto& p : unused) out.push_back({ std::string(), p });
	return out;
}

// Points the XML's collision shapes at the NIF's proxies (gProxyRenames), keeping the file as it was in .bak.
static void BuildSystem();
static void UseProxiesInXml()
{
	std::string text = datafiles::ReadFile(gItem.xmlPath), out;
	const std::regex re = ShapeNameRegex();
	size_t last = 0;
	int renamed = 0;
	for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
		std::string name = (*it)[2].str();
		for (auto& [from, to] : gProxyRenames)
			if (!from.empty() && _stricmp(name.c_str(), from.c_str()) == 0) { name = to; ++renamed; break; }
		out += text.substr(last, it->position() - last) + (*it)[1].str() + name + (*it)[3].str();
		last = it->position() + it->length();
	}
	out += text.substr(last);
	if (!renamed) return;
	std::string bak = gItem.xmlPath + ".bak";
	if (!fs::exists(bak)) { std::error_code ec; fs::copy_file(gItem.xmlPath, bak, ec); }
	std::ofstream o(gItem.xmlPath, std::ios::binary);
	o << out;
	if (!o) { AddLog(4, "could not write " + gItem.xmlPath); return; }
	o.close();
	AddLog(2, "pointed " + std::to_string(renamed) + " collision shape(s) in " + gItem.xmlPath + " at the NIF's proxies (the file as it was: " + bak + ")");
	BuildSystem();
}

// Builds (or rebuilds) the physics for the loaded item from its XML.
static void BuildSystem()
{
	auto* world = hdt::SkyrimPhysicsWorld::get();
	gMissingProxies.clear();
	gProxyRenames.clear();
	if (gItem.system) {
		world->removeSkinnedMeshSystem(gItem.system.get());
		gItem.system = nullptr;   // gives the skin slots back
	}
	for (auto& s : gItem.shapes) s.collider = 0;
	gItem.simulatedBones = gItem.constraintCount = 0;
	if (gItem.scene.physicsFile.empty()) {
		AddLog(3, "the NIF names no physics file (no \"HDT Skinned Mesh Physics Object\" string on its root)");
		return;
	}
	gItem.xmlNamed = gItem.scene.physicsFile;
	gXmlFixPath.clear();
	{
		std::string source;
		gItem.xmlPath = datafiles::Resolve(gItem.roots, gItem.xmlNamed, &source);
		std::string l = source;
		for (auto& c : l) c = (char)tolower((unsigned char)c);
		gXmlArchived = l.size() > 4 && l.compare(l.size() - 4, 4, ".bsa") == 0;
	}
	if (gItem.xmlPath.empty()) {
		// Not under a Data folder: next to the NIF, by file name.
		std::string base = fs::path(gItem.xmlNamed).filename().string();
		auto next = fs::path(gItem.nifPath).parent_path() / base;
		if (fs::exists(next)) {
			gItem.xmlPath = next.string();
			// The game only looks where the NIF says (Data folders and archives), not next to the NIF. When the NIF is under a
			// Meshes folder (a Data folder or a mod's), the path it should name is known: offered as a fix (FixXmlPath).
			for (fs::path p = fs::absolute(gItem.nifPath).parent_path(); p.has_parent_path() && p != p.parent_path(); p = p.parent_path())
				if (_stricmp(p.filename().string().c_str(), "meshes") == 0) {
					gXmlFixPath = fs::relative(next, p.parent_path()).string();
					std::string named = gItem.xmlNamed;
					for (auto& c : named) if (c == '/') c = '\\';
					if (_stricmp(named.c_str(), gXmlFixPath.c_str()) == 0) gXmlFixPath.clear();   // already right (a folder outside the game's Data)
					break;
				}
			AddLog(3, "physics file '" + gItem.xmlNamed + "' is not in any Data folder: using the one next to the NIF. "
			          "The game will NOT find it there: " + (gXmlFixPath.empty() ? "install it at Data\\" + gItem.xmlNamed + " or change the NIF's path."
			          : "'Point the NIF at this XML' changes the NIF's path to " + gXmlFixPath + "."));
		}
	}
	gItem.xmlTime = TimeOf(gItem.xmlPath);
	std::string xml = gItem.xmlPath;	// The physics is built from the file with the Bone values panel's edits in; a file that changed on disk (saved,
	// edited by hand, another item) is read afresh and its edits dropped.
	if (!xml.empty()) {
		std::string text = datafiles::ReadFile(xml);
		if (text != gXml.text || xml != gXmlPath) { gXml.Parse(text); gXmlPath = xml; }
	} else { gXml = xmledit::Doc(); gXmlPath.clear(); }
	scene::gReadFile = [](const char*) { return gXml.Compose(); };
	if (xml.empty()) {
		AddLog(4, "physics file '" + gItem.xmlNamed + "' not found in any Data folder or next to the NIF");
		return;
	}
	gMissingProxies = MissingProxies(datafiles::ReadFile(xml));
	if (!gMissingProxies.empty()) {
		std::string l;
		for (auto& m : gMissingProxies) l += (l.empty() ? "" : ", ") + m;
		AddLog(3, "collision against the full meshes (no low-poly proxies): " + l);
	}
	gProxyRenames = UnusedProxies(datafiles::ReadFile(xml));
	for (auto& [from, to] : gProxyRenames)
		AddLog(3, "proxy '" + to + "' is not used by the XML" + (from.empty() ? std::string(" (nothing in it to point at it)") : ": it names '" + from + "' instead"));

	hdt::DefaultBBP::PhysicsFile_t file{ gItem.xmlNamed, {} };
	hdt::SkyrimSystemCreator creator;
	creator.m_weightThresholdScale = 1.0f - gS.waistCollision;
	creator.m_ground = gS.ground;
	gItem.builtWaist = gS.waistCollision;
	auto system = creator.createOrUpdateSystem(gItem.scene.skeleton.actor, gItem.scene.model, &file, {}, nullptr);
	if (!system) {
		AddLog(4, "no physics built from " + xml + " (see the messages above)");
		return;
	}
	gItem.system = system;
	world->addSkinnedMeshSystem(system.get());
	for (auto& b : system->getBones()) gItem.simulatedBones += !b->m_rig.isStaticOrKinematicObject();
	gItem.constraintCount = (int)system->constraints().size();
	for (auto& g : system->constraintGroups()) gItem.constraintCount += (int)g->m_constraints.size();
	for (auto& body : system->meshes())
		for (auto& s : gItem.shapes)
			if (s.info.geometry->m_name && body->m_name == RE::BSFixedString(s.info.geometry->m_name))
				s.collider = body->m_shape && body->m_shape->asPerTriangleShape() ? 2 : 1;
	AddLog(2, "physics: " + std::to_string(system->getBones().size()) + " bones (" + std::to_string(gItem.simulatedBones) + " simulated), " +
		std::to_string(gItem.constraintCount) + " constraints, " + std::to_string(system->meshes().size()) + " collision bodies");
}

// --- motion ---------------------------------------------------------------------------------------------
//
// Two ways to move the body. With the game's skeleton and its locomotion animations found (loose .kf files
// in the Data folders), the presets play them: crossfaded as the game blends, the body carried by each
// clip's own root motion so the feet stay planted. Without, a procedural stride on the legs.

static const char* kSwayTip = "How far the constraint swings to the sides (its y limit): -1 not at all, 0 as its template has it, 1 two and a half "
	"times as far (at most 1.5 rad). Leaves its forward-back swing and twist as they are.";

static const char* kPresets[] = { "Stand", "Walk", "Run", "Walk in circles", "Spin in place", "Start and stop", "Jumping", "Sneak", "Sneak walk", "Drive (WASD)", "Animation file" };
enum { kStand, kWalk, kRun, kCircles, kSpin, kStartStop, kJumping, kSneak, kSneakWalk, kDrive, kCustom };

// Weapon stances: the locomotion file name prefix the game uses for each.
static const char* kStances[] = { "Unarmed", "Pistol", "Rifle", "Automatic rifle", "One-hand melee", "Two-hand melee" };
static const char* kStancePrefix[] = { "mt", "1hp", "2hr", "2ha", "1hm", "2hm" };

struct ClipPlay
{
	const kf::Clip* clip = nullptr;
	std::string key, path;
	std::vector<RE::NiNode*> nodes;   // per track: the skeleton node it drives, or null
	float time = 0;
	V3 velocity;                      // root motion, actor space, units per second
};

struct Anim
{
	bool use = true;                  // play the game's animations when they are found
	bool female = false;
	bool sneak = false;               // the sneak (crouched) set
	int stance = 0;
	std::string skeletonPath;
	std::map<std::string, RE::NiTransform> rest;   // every skeleton node's rest local
	std::unordered_map<std::string, kf::Clip> cache;   // by file path
	std::map<std::string, std::string> found;         // action ("forward") -> file, for the current stance
	std::set<std::string> borrowed;                   // actions the stance lacks, played from unarmed
	ClipPlay current, previous;
	float blend = 1;                  // 0 at a switch .. 1 when the new clip has taken over
	std::string customPath;
	bool wasAnimating = false;
} gAnim;

struct Motion
{
	V3 pos;
	float yaw = 0, speed = 0, yawRate = 0, phase = 0, bob = 0, jump = -1, time = 0;
	V3 velocity;                      // actor space (animated mode)
	// A jump with the game's animations: take-off, in the air, landing, then back to the locomotion.
	enum { kGround, kTakeOff, kAir, kLanding } jumpStage = kGround;
	float stageTime = 0, groundTime = 0;
} gMotion;

static RE::NiNode* Bone(const char* name)
{
	auto it = gItem.scene.skeleton.bones.find(name);
	return it == gItem.scene.skeleton.bones.end() ? nullptr : it->second;
}

static nif::Mat3 ToNif(const RE::NiMatrix3& m)
{
	nif::Mat3 r;
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = m.entry[i][j];
	return r;
}
static RE::NiMatrix3 ToRE(const nif::Mat3& m)
{
	RE::NiMatrix3 r;
	for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.entry[i][j] = m.m[i][j];
	return r;
}

static const kf::Clip* LoadClip(const std::string& path)
{
	if (auto it = gAnim.cache.find(path); it != gAnim.cache.end()) return &it->second;
	try {
		auto& c = gAnim.cache[path] = kf::Load(path);
		return &c;
	} catch (const std::exception& e) {
		AddLog(4, "cannot read animation " + path + ": " + e.what());
		gAnim.cache.erase(path);
		return nullptr;
	}
}

// The file the game would play for an action ("forward", "fastforward", "idle"...) with a stance prefix
// ("mt", "1hp"...): the per-gender (kNVSE) and vanilla paths in every Data folder by priority, then, last,
// AnimGroupOverride folders (many of those are for one weapon or one NPC). Loose files only.
static std::string FindAnimation(const std::string& action, const char* prefix)
{
	std::string file = std::string(prefix) + action + ".kf";
	std::string gender = gAnim.female ? "female" : "male";
	// Jumps, aims and sneaks sit in characters\_male itself, the rest of the locomotion in its locomotion
	// folder in mods; vanilla keeps much of that in characters\_male too. Both are tried, the usual one first.
	bool top = action.rfind("jump", 0) == 0 || action == "aim" || std::string(prefix).rfind("sneak", 0) == 0;
	std::error_code ec;
	// Loose files, then the archives (datafiles::Resolve), the per-gender path first.
	const std::string male = "meshes\\characters\\_male\\", loco = "meshes\\characters\\_male\\locomotion\\";
	const std::string& first = top ? male : loco;
	const std::string& second = top ? loco : male;
	for (const std::string& rel : { first + gender + "\\" + file, first + file, second + gender + "\\" + file, second + file })
		if (std::string p = datafiles::Resolve(gItem.roots, rel); !p.empty()) return p;
	for (auto& root : gItem.roots.folders) {
		fs::path over = fs::path(root) / "meshes" / "AnimGroupOverride";
		if (!fs::is_directory(over, ec)) continue;
		for (auto& d : fs::directory_iterator(over, ec)) {
			fs::path p = top ? d.path() / "_male" / file : d.path() / "_male" / "locomotion" / file;
			if (fs::exists(p, ec)) return p.string();
		}
	}
	return {};
}

static const char* kActions[] = { "idle", "forward", "fastforward", "backward", "fastbackward", "left", "right", "fastleft", "fastright", "turnleft", "turnright",
	"jumpstart", "jumploop", "jumpland", "jumploopforward", "jumplandforward" };

// Each action for the current stance. A weapon stance's standing pose is its aim when it has no idle; what a
// stance lacks loose is borrowed from unarmed (noted in gAnim.borrowed), as the game would show something.
static void FindAnimations()
{
	gAnim.found.clear();
	gAnim.borrowed.clear();
	// Sneaking: the stance's sneak set ("sneak1hp..."), else unarmed sneak, else the stance standing up.
	std::string stance = kStancePrefix[gAnim.stance];
	std::vector<std::string> prefixes;
	if (gAnim.sneak) { prefixes.push_back("sneak" + stance); if (gAnim.stance != 0) prefixes.push_back("sneakmt"); }
	prefixes.push_back(stance);
	if (gAnim.stance != 0) prefixes.push_back("mt");
	for (const char* a : kActions) {
		std::string p;
		for (size_t i = 0; i < prefixes.size() && p.empty(); i++) {
			p = FindAnimation(a, prefixes[i].c_str());
			// A weapon stance's standing pose is its aim when it has no idle.
			if (p.empty() && std::string(a) == "idle" && prefixes[i] != "mt" && prefixes[i] != "sneakmt") p = FindAnimation("aim", prefixes[i].c_str());
			if (!p.empty() && i > 0) gAnim.borrowed.insert(a);
		}
		if (!p.empty()) gAnim.found[a] = p;
	}
}

static void Play(ClipPlay& play, const std::string& key, const std::string& path)
{
	play = ClipPlay();
	play.clip = LoadClip(path);
	if (!play.clip) return;
	play.key = key;
	play.path = path;
	for (auto& t : play.clip->tracks) {
		auto it = gItem.scene.skeleton.bones.find(t.node);
		play.nodes.push_back(it == gItem.scene.skeleton.bones.end() ? nullptr : it->second);
	}
	// Root motion: how far the accumulation root travels over one cycle.
	for (auto& t : play.clip->tracks) {
		if (t.node != play.clip->accumRoot) continue;
		kf::Pose a = kf::Sample(t, play.clip->start), b = kf::Sample(t, play.clip->stop);
		float d = play.clip->Duration();
		if (a.hasTranslation && b.hasTranslation && d > 0)
			play.velocity = V3(b.translation.x - a.translation.x, b.translation.y - a.translation.y, 0) * (1.0f / d);
	}
	play.time = play.clip->start;
}

// Switches to the clip for a key (an action, or the custom file), crossfading from the one playing.
static bool Want(const std::string& key)
{
	std::string path = key == "custom" ? gAnim.customPath : (gAnim.found.count(key) ? gAnim.found[key] : std::string());
	if (path.empty()) return false;
	// The same action from another file (crouching mid-walk, a new stance) crossfades too.
	if (gAnim.current.clip && gAnim.current.key == key && gAnim.current.path == path) return true;
	ClipPlay next;
	Play(next, key, path);
	if (!next.clip) return false;
	gAnim.previous = gAnim.current;
	gAnim.current = next;
	gAnim.blend = gAnim.previous.clip ? 0.0f : 1.0f;
	return true;
}

static void Advance(ClipPlay& p, float dt)
{
	if (!p.clip) return;
	p.time += dt;
	float d = p.clip->Duration();
	if (d <= 0) { p.time = p.clip->start; return; }
	if (p.clip->loops) p.time = p.clip->start + fmodf(p.time - p.clip->start, d);
	else p.time = std::min(p.time, p.clip->stop);
}

// Poses the skeleton: every node at rest, the accumulation root at the identity (its rotation and height
// are in NonAccum's keys; its travel moves the actor), then the clips, crossfaded.
static void ApplyClips()
{
	for (auto& [name, local] : gAnim.rest)
		if (auto* n = Bone(name.c_str())) n->local = local;
	struct P { kf::Quat q; nif::Vec3 t; float s; };
	std::unordered_map<RE::NiNode*, P> pose;
	auto start = [&](RE::NiNode* n) -> P& {
		auto it = pose.find(n);
		if (it != pose.end()) return it->second;
		return pose[n] = { kf::FromMatrix(ToNif(n->local.rotate)), { n->local.translate.x, n->local.translate.y, n->local.translate.z }, n->local.scale };
	};
	auto apply = [&](const ClipPlay& play, float w) {
		if (!play.clip) return;
		for (size_t i = 0; i < play.clip->tracks.size(); i++) {
			RE::NiNode* n = play.nodes[i];
			if (!n || play.clip->tracks[i].node == play.clip->accumRoot) continue;
			kf::Pose s = kf::Sample(play.clip->tracks[i], play.time);
			P& p = start(n);
			if (s.hasRotation) p.q = w >= 1 ? s.rotation : kf::Slerp(p.q, s.rotation, w);
			if (s.hasTranslation) p.t = { p.t.x + (s.translation.x - p.t.x) * w, p.t.y + (s.translation.y - p.t.y) * w, p.t.z + (s.translation.z - p.t.z) * w };
			if (s.hasScale) p.s = p.s + (s.scale - p.s) * w;
		}
	};
	const kf::Clip* c = gAnim.current.clip;
	if (c && !c->accumRoot.empty())
		if (auto* root = Bone(c->accumRoot.c_str())) root->local = RE::NiTransform();
	if (gAnim.blend < 1 && gAnim.previous.clip) apply(gAnim.previous, 1.0f);
	apply(gAnim.current, gAnim.blend < 1 && gAnim.previous.clip ? gAnim.blend : 1.0f);
	for (auto& [n, p] : pose) {
		n->local.rotate = ToRE(kf::ToMatrix(p.q));
		n->local.translate = { p.t.x, p.t.y, p.t.z };
		n->local.scale = p.s;
	}
}

static bool Animated()
{
	return gAnim.use && !gAnim.skeletonPath.empty() && (gAnim.found.count("idle") || (gS.preset == kCustom && !gAnim.customPath.empty()));
}

static void UpdateMotion(float dt)
{
	auto& m = gMotion;
	m.time += dt;
	ImGuiIO& io = ImGui::GetIO();
	bool animated = Animated();
	if (animated != gAnim.wasAnimating) {
		// Switching modes: every node back to rest, and the clips start over.
		for (auto& [name, local] : gAnim.rest)
			if (auto* n = Bone(name.c_str())) n->local = local;
		gAnim.current = gAnim.previous = ClipPlay();
		gAnim.wasAnimating = animated;
	}

	float targetSpeed = 0, targetYawRate = 0, strafe = 0;
	bool jumpNow = false;
	std::string want = "idle";
	bool run = false;
	switch (gS.preset) {
	case kWalk: targetSpeed = 150; want = "forward"; break;
	case kSneak: break;   // the idle, from the sneak set (the preset turns sneaking on)
	case kSneakWalk: targetSpeed = 150; want = "forward"; break;
	case kRun: targetSpeed = 400; want = "fastforward"; break;
	case kCircles: targetSpeed = 150; targetYawRate = 150.0f / 220.0f; want = "forward"; break;
	case kSpin: targetYawRate = 3.0f; want = gAnim.found.count("turnleft") ? "turnleft" : "idle"; break;
	case kStartStop: run = fmodf(m.time, 5.0f) < 2.5f; targetSpeed = run ? 350.0f : 0.0f; want = run ? "fastforward" : "idle"; break;
	case kJumping: jumpNow = m.groundTime > 1.4f; break;
	case kDrive:
		if (!io.WantCaptureKeyboard) {
			run = ImGui::IsKeyDown(ImGuiKey_LeftShift);
			if (ImGui::IsKeyDown(ImGuiKey_W)) { targetSpeed = run ? 400.0f : 150.0f; want = run ? "fastforward" : "forward"; }
			else if (ImGui::IsKeyDown(ImGuiKey_S)) { targetSpeed = -100.0f; want = run ? "fastbackward" : "backward"; }
			else if (ImGui::IsKeyDown(ImGuiKey_Q)) { strafe = run ? -300.0f : -120.0f; want = run ? "fastleft" : "left"; }
			else if (ImGui::IsKeyDown(ImGuiKey_E)) { strafe = run ? 300.0f : 120.0f; want = run ? "fastright" : "right"; }
			if (ImGui::IsKeyDown(ImGuiKey_A)) targetYawRate = 2.5f;
			if (ImGui::IsKeyDown(ImGuiKey_D)) targetYawRate = -2.5f;
			if (want == "idle" && targetYawRate != 0) want = targetYawRate > 0 ? "turnleft" : "turnright";
			jumpNow = ImGui::IsKeyPressed(ImGuiKey_Space);
			// C crouches and stands, as the game's sneak key does (Ctrl is the body drag here).
			if (ImGui::IsKeyPressed(ImGuiKey_C, false)) { gAnim.sneak = !gAnim.sneak; FindAnimations(); }
		}
		break;
	case kCustom: want = "custom"; break;
	}
	bool onGround = m.jump < 0 && m.jumpStage == Motion::kGround;
	jumpNow = jumpNow && onGround;
	m.groundTime = onGround && !jumpNow ? m.groundTime + dt : 0.0f;

	float k = 1.0f - std::exp(-dt * 8.0f);
	V3 velocity;   // actor space
	if (animated) {
		// A jump: the take-off clip (if installed loose), the in-air loop for the flight, the landing clip,
		// then the locomotion again. Moving, the forward variants where there are.
		bool moving = std::fabs(m.velocity.y) > 60.0f;
		auto has = [](const char* a) { return gAnim.found.count(a) > 0; };
		auto clipDone = [&]() { return gAnim.current.clip && m.stageTime >= gAnim.current.clip->Duration() / std::max(0.05f, gS.speedScale); };
		if (jumpNow) {
			m.jumpStage = has("jumpstart") ? Motion::kTakeOff : Motion::kAir;
			m.stageTime = 0;
			if (m.jumpStage == Motion::kAir) m.jump = 0;
			jumpNow = false;   // the flight starts when the take-off ends
		}
		m.stageTime += dt;
		switch (m.jumpStage) {
		case Motion::kTakeOff:
			want = "jumpstart";
			if (gAnim.current.key == "jumpstart" && clipDone()) { m.jumpStage = Motion::kAir; m.stageTime = 0; m.jump = 0; }
			break;
		case Motion::kAir:
			want = moving && has("jumploopforward") ? "jumploopforward" : "jumploop";
			if (m.jump < 0) { m.jumpStage = Motion::kLanding; m.stageTime = 0; }
			break;
		case Motion::kLanding:
			want = moving && has("jumplandforward") ? "jumplandforward" : "jumpland";
			if (!has(want.c_str()) || (gAnim.current.key == want && clipDone())) m.jumpStage = Motion::kGround;
			break;
		default:
			break;
		}
		bool airborne = m.jumpStage == Motion::kTakeOff || m.jumpStage == Motion::kAir;
		// Fall back along the way the game would (turns and strafes to idle, runs to walks).
		if (!Want(want)) {
			std::string alt = want.rfind("fast", 0) == 0 ? want.substr(4) : (want.find("turn") == 0 ? "idle" : "idle");
			if (!Want(alt)) Want("idle");
		}
		float rate = gS.speedScale;
		Advance(gAnim.current, dt * rate);
		Advance(gAnim.previous, dt * rate);
		gAnim.blend = std::min(1.0f, gAnim.blend + dt / 0.25f);
		ApplyClips();
		// The body travels as the clip does; the preset's speed only where a clip has no root motion.
		V3 clipV = gAnim.current.velocity, prevV = gAnim.previous.clip ? gAnim.previous.velocity : clipV;
		V3 v = prevV + (clipV - prevV) * gAnim.blend;
		bool hasRootMotion = std::fabs(clipV.x) + std::fabs(clipV.y) > 1.0f;
		velocity = hasRootMotion ? v * rate : V3(strafe, targetSpeed, 0) * gS.speedScale;
		if (!airborne) m.velocity = m.velocity + (velocity - m.velocity) * k;   // in the air, the momentum carries
		velocity = m.velocity;
	} else {
		targetSpeed *= gS.speedScale;
		m.speed += (targetSpeed - m.speed) * k;
		velocity = V3(strafe * gS.speedScale, m.speed, 0);
	}
	m.yawRate += (targetYawRate - m.yawRate) * k;
	m.yaw += m.yawRate * dt;
	V3 fwd(-std::sin(m.yaw), std::cos(m.yaw), 0), right(std::cos(m.yaw), std::sin(m.yaw), 0);
	m.pos = m.pos + fwd * (velocity.y * dt) + right * (velocity.x * dt);

	float z = 0;
	if (jumpNow) m.jump = 0;
	if (m.jump >= 0) {
		m.jump += dt;
		const float T = 0.75f;
		if (m.jump > T) m.jump = -1;
		else z = 4.0f * 55.0f * (m.jump / T) * (1 - m.jump / T);
	}

	float newBob = 0;

	if (!animated) {
		// Legs: a stride cycle by distance, wider and quicker with speed; the calves bend on the back swing.
		float v = std::fabs(m.speed);
		m.phase += dt * 2.0f * 3.14159f * (0.5f + v / 300.0f) * (v > 1 ? 1.0f : 0.0f);
		float stride = std::min(0.8f, 0.49f * v / 150.0f) * (m.speed < 0 ? -1.0f : 1.0f);
		float swing = stride * std::sin(m.phase);
		float tuck = m.jump >= 0 ? 0.6f * std::sin(3.14159f * m.jump / 0.75f) : 0.0f;
		newBob = 2.0f * std::sin(m.phase * 2.0f) * std::min(1.0f, v / 150.0f);
		if (gItem.lThigh) gItem.lThigh->local.rotate = gItem.lThigh0.rotate * scene::RotX(swing - tuck);
		if (gItem.rThigh) gItem.rThigh->local.rotate = gItem.rThigh0.rotate * scene::RotX(-swing - tuck);
		if (gItem.lCalf) gItem.lCalf->local.rotate = gItem.lCalf0.rotate * scene::RotX(std::max(0.0f, -swing) * 1.2f + tuck * 1.6f);
		if (gItem.rCalf) gItem.rCalf->local.rotate = gItem.rCalf0.rotate * scene::RotX(std::max(0.0f, swing) * 1.2f + tuck * 1.6f);

	}

	auto* actor = gItem.scene.skeleton.actor;
	actor->local.rotate = scene::RotZ(m.yaw);
	V3 at = m.pos;
	actor->local.translate = { at.x, at.y, z + newBob };
	actor->world = actor->local;
	hdt::updateTransformUpDown(actor);
}

static void OpenAnimationDialog()
{
	char file[MAX_PATH] = {};
	OPENFILENAMEA ofn{ sizeof(ofn) };
	ofn.hwndOwner = gWnd;
	ofn.lpstrFilter = "Animations (*.kf)\0*.kf\0All files\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	std::string dir = gAnim.customPath.empty() ? (gAnim.found.count("idle") ? fs::path(gAnim.found["idle"]).parent_path().string() : "") : fs::path(gAnim.customPath).parent_path().string();
	ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
	if (GetOpenFileNameA(&ofn) && LoadClip(file)) {
		gAnim.customPath = file;
		gAnim.current = gAnim.previous = ClipPlay();   // replay from the start even if it was playing
		gS.preset = kCustom;
		AddLog(2, std::string("animation: ") + file);
	}
}



// Skinned shapes with bones the game's skeleton lacks but no "SMP Bones" list (an SMP NIF made outside these tools,
// a Skyrim body): New Vegas drops those bones on attach, so the plugin cannot build them. "Shape: bone, bone".
static std::vector<std::string> MissingBoneLists(const std::string& path, const scene::Skeleton& skeleton)
{
	std::vector<std::string> out;
	try {
		nif::File f;
		f.load(path);
		auto name = [&](int block) {
			if (block < 0 || block >= (int)f.blocks.size()) return std::string();
			nif::Reader r(f.blocks[block].data(), f.blocks[block].size());
			uint32_t s = r.u32();
			return s < f.strings.size() ? f.strings[s] : std::string();
		};
		for (size_t si = 0; si < f.blocks.size(); si++) {
			const std::string& type = f.typeOf(si);
			if (type != "NiTriShape" && type != "NiTriStrips") continue;
			nif::Geometry g;
			g.read(f.blocks[si]);
			if (g.skin < 0) continue;
			bool hasList = false;
			for (int e : g.av.extraData) {
				if (f.typeOf(e) != "NiStringExtraData") continue;
				nif::Reader r(f.blocks[e].data(), f.blocks[e].size());
				uint32_t k = r.u32();
				if (k < f.strings.size() && f.strings[k].rfind("SMP Bones", 0) == 0) hasList = true;
			}
			if (hasList) continue;
			nif::SkinInstance skin;
			skin.read(f.blocks[g.skin], f.typeOf(g.skin) == "BSDismemberSkinInstance");
			std::string missing;
			for (int b : skin.bones) {
				std::string n = name(b);
				bool known = false;
				for (auto& [k, node] : skeleton.bones) if (_stricmp(k.c_str(), n.c_str()) == 0) { known = true; break; }
				if (!known) missing += (missing.empty() ? "" : ", ") + n;
			}
			if (!missing.empty()) out.push_back(name((int)si) + ": " + missing);
		}
	} catch (...) {}
	return out;
}
static std::vector<std::string> gMissingBoneLists;

// A copy of the game's skeleton in the NIF (nifclean.h): what cleaning it would take out and move onto the root.
static nifclean::Report gCopiedSkeleton;

static std::set<std::string> XmlBoneNames()
{
	if (gItem.xmlPath.empty()) return {};
	return nifclean::xmlBoneNames(datafiles::ReadFile(gItem.xmlPath));
}

static void CheckCopiedSkeleton()
{
	gCopiedSkeleton = nifclean::Report();
	if (gAnim.skeletonPath.empty() || !gItem.loaded) return;
	try {
		nif::File f;
		f.load(gItem.nifPath);
		gCopiedSkeleton = nifclean::clean(f, nifclean::skeletonNames(gAnim.skeletonPath), true, XmlBoneNames());
		if (!gCopiedSkeleton.ok) AddLog(3, "copied skeleton check: " + gCopiedSkeleton.error);
		else if (!gCopiedSkeleton.removed.empty())
			AddLog(3, "the NIF carries " + std::to_string(gCopiedSkeleton.removed.size()) + " node(s) of a copied skeleton (in game they can catch the "
			          "item's bones and draw it off the body): 'Remove the copied skeleton' takes them out");
	} catch (const std::exception& e) {
		AddLog(3, std::string("copied skeleton check: ") + e.what());
	}
}

// Takes the copied skeleton out of the NIF (nifclean.h), keeping the old one as .bak, and reloads.
static void LoadItem(const std::string& path);
static void RemoveCopiedSkeleton()
{
	try {
		nif::File f;
		f.load(gItem.nifPath);
		auto rep = nifclean::clean(f, nifclean::skeletonNames(gAnim.skeletonPath), false, XmlBoneNames());
		if (!rep.ok) { AddLog(4, "could not clean the NIF: " + rep.error); return; }
		if (!rep.changes()) { AddLog(2, "nothing to clean"); return; }
		std::string err;
		std::string kept = backup::keep(gItem.nifPath, &err);
		if (!err.empty()) { AddLog(4, "could not back up " + gItem.nifPath + ": " + err + " (not cleaned)"); return; }
		f.save(gItem.nifPath);
		AddLog(2, "removed " + std::to_string(rep.removed.size()) + " node(s) of a copied skeleton, moved " + std::to_string(rep.flattened.size()) +
		          " skeleton bone(s) the skins use onto the root (" + std::to_string(rep.blocksBefore) + " blocks -> " + std::to_string(rep.blocksAfter) +
		          "); the NIF as it was: " + kept);
	} catch (const std::exception& e) {
		AddLog(4, std::string("could not clean the NIF: ") + e.what());
		return;
	}
	LoadItem(gItem.nifPath);
}

static void LoadItem(const std::string& path)
{
	auto* world = hdt::SkyrimPhysicsWorld::get();
	if (gItem.system) {
		world->removeSkinnedMeshSystem(gItem.system.get());
		gItem.system = nullptr;
	}
	AddLog(2, "loading " + path);
	try {
		Item next;
		next.nifPath = path;
		next.nifTime = TimeOf(path);
		next.roots = datafiles::Discover(path, gSources);
		// The game's skeleton, as the item will be on in game (and as the animations are made for).
		gAnim.skeletonPath = datafiles::Resolve(next.roots, "meshes\\characters\\_male\\skeleton.nif");
		try {
			next.scene = scene::Load(path, gAnim.skeletonPath);
		} catch (const std::exception& e) {
			if (gAnim.skeletonPath.empty()) throw;
			AddLog(3, "cannot read the skeleton " + gAnim.skeletonPath + " (" + e.what() + "): using the item's bind pose");
			gAnim.skeletonPath.clear();
			next.scene = scene::Load(path);
		}
		next.loaded = true;
		gItem = std::move(next);
	} catch (const std::exception& e) {
		AddLog(4, std::string("cannot read ") + path + ": " + e.what());
		return;
	}
	IniSet("LastFile", path);
	// Only against the game's skeleton (without it the item's own bones make up the skeleton) and for an SMP item.
	gMissingBoneLists.clear();
	if (!gAnim.skeletonPath.empty() && !gItem.scene.physicsFile.empty()) {
		gMissingBoneLists = MissingBoneLists(path, gItem.scene.skeleton);
		for (auto& m : gMissingBoneLists) AddLog(3, "no SMP Bones list, so the game drops these bones: " + m);
	}
	gMotion = Motion();
	gAnim.current = gAnim.previous = ClipPlay();
	gAnim.wasAnimating = false;
	gAnim.rest.clear();
	for (auto& [name, node] : gItem.scene.skeleton.bones) gAnim.rest[name] = node->local;
	{
		std::string file = fs::path(path).stem().string();
		for (auto& c : file) c = (char)tolower((unsigned char)c);
		gAnim.female = file.size() > 2 && (file.compare(file.size() - 2, 2, "_f") == 0 || file.find("female") != std::string::npos);
	}
	FindAnimations();
	if (gAnim.skeletonPath.empty()) AddLog(3, "no skeleton.nif found loose in the Data folders: the procedural motion is used");
	else AddLog(2, "skeleton: " + gAnim.skeletonPath + "; " + std::to_string(gAnim.found.size()) + " locomotion animations found (" + (gAnim.female ? "female" : "male") + ")");
	gItem.pelvis = Bone("Bip01 Pelvis");
	gItem.lThigh = Bone("Bip01 L Thigh"); gItem.rThigh = Bone("Bip01 R Thigh");
	gItem.lCalf = Bone("Bip01 L Calf"); gItem.rCalf = Bone("Bip01 R Calf");
	if (gItem.pelvis) gItem.pelvis0 = gItem.pelvis->local;
	if (gItem.lThigh) gItem.lThigh0 = gItem.lThigh->local;
	if (gItem.rThigh) gItem.rThigh0 = gItem.rThigh->local;
	if (gItem.lCalf) gItem.lCalf0 = gItem.lCalf->local;
	if (gItem.rCalf) gItem.rCalf0 = gItem.rCalf->local;

	for (auto& info : gItem.scene.shapes) {
		ShapeDraw d;
		d.info = info;
		d.weights = scene::WeightsOf(info.geometry);
		auto* td = (RE::NiTriShapeData*)info.geometry->data;
		d.indices.assign(td->triList, td->triList + td->triListLength);
		d.verts.resize(td->vertexCount);
		if (!info.diffuse.empty()) {
			d.texturePath = datafiles::Resolve(gItem.roots, info.diffuse);
			if (d.texturePath.empty()) d.textureNote = "not found in any Data folder or archive";
			else if (auto it = gTextureCache.find(d.texturePath); it != gTextureCache.end()) d.texture = it->second;
			else {
				std::string err;
				d.texture = datafiles::LoadDDS(gDev, d.texturePath, &err);
				d.textureNote = err;
				gTextureCache[d.texturePath] = d.texture;
			}
		}
		// Gore caps (the stumps a dismemberment shows) are hidden in game until a limb comes off.
		{
			std::string n = info.geometry->m_name ? info.geometry->m_name : "";
			for (auto& c : n) c = (char)tolower((unsigned char)c);
			if (n.find("cap") != std::string::npos || n.find("meathead") != std::string::npos) d.visible = false;
			// smpdress --proxy collision stand-ins are app-culled: never drawn in game.
			if (n.rfind("smp proxy:", 0) == 0) d.visible = false;
		}
		gItem.shapes.push_back(std::move(d));
	}
	AddLog(2, std::to_string(gItem.shapes.size()) + " shapes, skeleton of " + std::to_string(gItem.scene.skeleton.bones.size()) + " bones; " +
		std::to_string(gItem.roots.folders.size()) + " Data folders and " + std::to_string(gItem.roots.archives.size()) + " archives searched");
	auto& r = gItem.roots;
	AddLog(2, r.mo2Instance.empty() ? std::string("MO2: none found") : "MO2: " + r.mo2Instance + ", profile '" + r.mo2Profile + "' (" + r.mo2How + ")");
	AddLog(2, r.gameData.empty() ? std::string("game: not found") : "game: " + r.gameData + " (" + r.gameHow + ")");
	for (auto& n : r.notes) AddLog(3, n);
	BuildSystem();
	CheckCopiedSkeleton();
}

static void OpenDialog()
{
	char file[MAX_PATH] = {};
	OPENFILENAMEA ofn{ sizeof(ofn) };
	ofn.hwndOwner = gWnd;
	ofn.lpstrFilter = "NIF meshes (*.nif)\0*.nif\0All files\0*.*\0";
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	std::string last = IniGet("LastFile");
	std::string dir = last.empty() ? "" : fs::path(last).parent_path().string();
	ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
	if (GetOpenFileNameA(&ofn)) LoadItem(file);
}

// A file picked with the system dialog (filter as "Name\0pattern\0"), or empty.
static std::string PickFile(const char* filter, const std::string& startIn)
{
	char file[MAX_PATH] = {};
	OPENFILENAMEA ofn{ sizeof(ofn) };
	ofn.hwndOwner = gWnd;
	ofn.lpstrFilter = filter;
	ofn.lpstrFile = file;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	ofn.lpstrInitialDir = startIn.empty() ? nullptr : startIn.c_str();
	return GetOpenFileNameA(&ofn) ? std::string(file) : std::string();
}

// A folder picked with the system dialog, or empty.
static std::string PickFolder()
{
	std::string result;
	IFileOpenDialog* dlg = nullptr;
	if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
		DWORD opts = 0;
		dlg->GetOptions(&opts);
		dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
		IShellItem* item = nullptr;
		if (SUCCEEDED(dlg->Show(gWnd)) && SUCCEEDED(dlg->GetResult(&item))) {
			PWSTR path = nullptr;
			if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
				char buf[MAX_PATH * 2];
				WideCharToMultiByte(CP_ACP, 0, path, -1, buf, sizeof buf, nullptr, nullptr);
				result = buf;
				CoTaskMemFree(path);
			}
			item->Release();
		}
		dlg->Release();
	}
	return result;
}

static void UpdateWind(float t)
{
	auto* world = hdt::SkyrimPhysicsWorld::get();
	if (!gS.wind) { world->setWind(btVector3(0, 0, 0)); return; }
	float a = gS.windDirection * 3.14159f / 180.0f;
	float gust = gS.windGusts ? 0.55f + 0.45f * (0.6f * std::sin(t * 0.9f) + 0.4f * std::sin(t * 2.3f + 1.0f)) : 1.0f;
	world->setWind(btVector3(-std::sin(a), std::cos(a), 0) * (gS.windStrength * hdt::scaleSkyrim * gust));
}

// --- camera -----------------------------------------------------------------------------------------------

struct Camera
{
	float yaw = 0.5f, pitch = 0.12f, dist = 250.0f;
	V3 pan;
	V3 target;
	bool init = false;
} gCam;

static V3 FocusPoint()
{
	if (gItem.pelvis) return V3(gItem.pelvis->world.translate) - V3(0, 0, 22);
	return V3(gItem.scene.skeleton.actor ? gItem.scene.skeleton.actor->world.translate : RE::NiPoint3{}) + V3(0, 0, 70);
}

static void UpdateCamera(float dt)
{
	ImGuiIO& io = ImGui::GetIO();
	V3 focus = FocusPoint();
	if (!gCam.init) { gCam.target = focus; gCam.init = true; }
	float k = 1.0f - std::exp(-dt * 10.0f);
	gCam.target = gCam.target + (focus - gCam.target) * k;
	if (!io.WantCaptureMouse) {
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)) {
			gCam.yaw -= io.MouseDelta.x * 0.008f;
			gCam.pitch = std::clamp(gCam.pitch + io.MouseDelta.y * 0.008f, -1.4f, 1.4f);
		}
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) {
			V3 right(std::cos(gCam.yaw), std::sin(gCam.yaw), 0);
			gCam.pan = gCam.pan + right * (-io.MouseDelta.x * gCam.dist * 0.0015f) + V3(0, 0, io.MouseDelta.y * gCam.dist * 0.0015f);
		}
		if (io.MouseWheel != 0) gCam.dist = std::clamp(gCam.dist * std::pow(0.88f, io.MouseWheel), 20.0f, 2000.0f);
		// Ctrl + left drag: drag the body around (the cloth feels it like any other move).
		if (io.KeyCtrl && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
			V3 right(std::cos(gCam.yaw), std::sin(gCam.yaw), 0), fwd(-std::sin(gCam.yaw), std::cos(gCam.yaw), 0);
			float s = gCam.dist * 0.0022f;
			gMotion.pos = gMotion.pos + right * (io.MouseDelta.x * s) + fwd * (-io.MouseDelta.y * s);
		}
	}
	if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_F)) { gCam.pan = V3(); gCam.dist = 250.0f; }
}

static V3 CameraEye()
{
	V3 at = gCam.target + gCam.pan;
	// yaw 0 looks at the body's front (+y): the eye sits on +y.
	V3 dir(-std::sin(gCam.yaw) * std::cos(gCam.pitch), std::cos(gCam.yaw) * std::cos(gCam.pitch), std::sin(gCam.pitch));
	return at + dir * gCam.dist;
}

// --- drawing ----------------------------------------------------------------------------------------------

// The Maker paints on the NIF's own pose (set each frame while it paints): every shape as it was modelled, the skin's
// frame at its root parent (rootParent's world x NiSkinData::rootParentToSkin inverted), so it stays with the actor.
static bool gNifPose = false;

static RE::NiTransform NifPoseTransform(RE::NiGeometry* g)
{
	auto* skin = g->skinInstance;
	if (!skin || !skin->skinData) return g->world;
	return (skin->rootParent ? skin->rootParent->world : g->world) * skin->skinData->rootParentToSkin.Invert();
}

static void SkinShape(ShapeDraw& s)
{
	auto* g = s.info.geometry;
	int n = (int)s.verts.size();
	bool hasNormals = (int)s.info.normals.size() == n;
	const bool skinned = g->skinInstance && !gNifPose;
	const RE::NiTransform pose = gNifPose ? NifPoseTransform(g) : g->world;
	for (int v = 0; v < n; v++) {
		RE::NiPoint3 p = skinned ? scene::SkinVertex(g, v, s.weights) : pose * g->data->vertices[v];
		auto& o = s.verts[v];
		o.px = p.x; o.py = p.y; o.pz = p.z;
		if (hasNormals) {
			RE::NiPoint3 nn = skinned ? scene::SkinNormal(g, s.info.normals[v], v, s.weights)
			                          : pose.rotate * RE::NiPoint3{ s.info.normals[v].x, s.info.normals[v].y, s.info.normals[v].z };
			o.nx = nn.x; o.ny = nn.y; o.nz = nn.z;
		}
		if (v < (int)s.info.uvs.size()) { o.u = s.info.uvs[v].first; o.v = s.info.uvs[v].second; }
	}
	if (!hasNormals) {
		for (auto& o : s.verts) o.nx = o.ny = o.nz = 0;
		for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
			auto &a = s.verts[s.indices[t]], &b = s.verts[s.indices[t + 1]], &c = s.verts[s.indices[t + 2]];
			V3 fn = Cross(V3(b.px - a.px, b.py - a.py, b.pz - a.pz), V3(c.px - a.px, c.py - a.py, c.pz - a.pz));
			for (auto* o : { &a, &b, &c }) { o->nx += fn.x; o->ny += fn.y; o->nz += fn.z; }
		}
	}
}

static void SetPS(const float color[4], float textured, float alphaTest, float unlit)
{
	V3 key = Norm(V3(0.35f, 0.6f, 0.7f)), fill = Norm(V3(-0.6f, -0.4f, 0.2f));
	float c[16] = { key.x, key.y, key.z, 0, color[0], color[1], color[2], color[3], textured, alphaTest, unlit, 0, fill.x, fill.y, fill.z, 0 };
	gDev->SetPixelShaderConstantF(0, c, 4);
}

static void DrawLines(const std::vector<LineVertex>& lines, D3DPRIMITIVETYPE type, bool onTop)
{
	if (lines.empty()) return;
	gDev->SetVertexShader(nullptr);
	gDev->SetPixelShader(nullptr);
	gDev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	gDev->SetTexture(0, nullptr);
	gDev->SetRenderState(D3DRS_LIGHTING, FALSE);
	gDev->SetRenderState(D3DRS_ZENABLE, onTop ? FALSE : TRUE);
	gDev->SetRenderState(D3DRS_POINTSIZE_MIN, 0);
	float ps = 6.0f;
	gDev->SetRenderState(D3DRS_POINTSIZE, *(DWORD*)&ps);
	UINT count = type == D3DPT_LINELIST ? (UINT)lines.size() / 2 : (UINT)lines.size();
	gDev->DrawPrimitiveUP(type, count, lines.data(), sizeof(LineVertex));
	gDev->SetRenderState(D3DRS_ZENABLE, TRUE);
}

// Weights heat map (as the rig previewer's): dark blue at 0, through blue, cyan, green and yellow to red at 1.
static D3DCOLOR Heat(float w, float shade = 1.0f)
{
	w = std::clamp(w, 0.0f, 1.0f);
	float r, g, b;
	if (w <= 0.0f) { r = 18 / 255.0f; g = 22 / 255.0f; b = 70 / 255.0f; }
	else {
		const float stops[][3] = { { 0.10f, 0.15f, 0.75f }, { 0.0f, 0.75f, 0.95f }, { 0.15f, 0.85f, 0.25f }, { 0.95f, 0.90f, 0.15f }, { 0.95f, 0.15f, 0.10f } };
		float x = w * 4.0f;
		int i = std::min(3, (int)x);
		float t = x - i;
		r = stops[i][0] + (stops[i + 1][0] - stops[i][0]) * t; g = stops[i][1] + (stops[i + 1][1] - stops[i][1]) * t; b = stops[i][2] + (stops[i + 1][2] - stops[i][2]) * t;
	}
	auto c = [&](float v) { return (int)std::clamp(v * shade * 255.0f, 0.0f, 255.0f); };
	return D3DCOLOR_ARGB(255, c(r), c(g), c(b));
}

static const char* SkinBoneName(const ShapeDraw& s, int slot)
{
	auto* skin = s.info.geometry->skinInstance;
	if (!skin || slot < 0 || slot >= (int)skin->boneCount || !skin->bones[slot]) return nullptr;
	return skin->bones[slot]->m_name;
}

// The chain being edited in Bone values (hovered, dragged, or pinned with its show button): its bones (lower case)
// and constraints (bodyA, bodyB), highlighted in the view in place of the heat-map bone. Set by the panel each frame.
static std::set<std::string> gHighlightBones;
static std::vector<std::pair<std::string, std::string>> gHighlightLinks;
static int gPinChain = -1;   // --chain N: the chain pinned at start (for screenshots)
static bool gRealtime = false;   // --realtime: measuring (--jitter, --clip...) at the real frame time instead of a fixed 120 Hz
static float gMeasureStep = 1.0f / 120.0f;   // --step s: the fixed frame time when measuring

// A shape's weight on the heat-map bone, or on the highlighted chain's bones together, per vertex (several skin
// slots can name the same bone).
static float HeatWeight(const ShapeDraw& s, int v)
{
	float w = 0;
	for (auto& [slot, wt] : s.weights[v]) {
		const char* n = SkinBoneName(s, slot);
		if (!n) continue;
		if (!gHighlightBones.empty()) {
			std::string l = n;
			for (auto& c : l) c = (char)tolower((unsigned char)c);
			if (gHighlightBones.count(l)) w += wt;
		} else if (_stricmp(n, gS.heatBone.c_str()) == 0) w += wt;
	}
	return w;
}

// Every bone the item's shapes are skinned to, sorted (SMP bones last).
static std::vector<std::string> SkinBoneNames()
{
	std::set<std::string> names;
	for (auto& s : gItem.shapes) {
		auto* skin = s.info.geometry->skinInstance;
		if (!skin) continue;
		for (uint32_t b = 0; b < skin->boneCount; b++) if (const char* n = SkinBoneName(s, (int)b)) names.insert(n);
	}
	std::vector<std::string> out(names.begin(), names.end());
	std::stable_partition(out.begin(), out.end(), [](const std::string& n) { return _strnicmp(n.c_str(), "SMP", 3) != 0; });
	return out;
}

// A shape drawn as the heat map of the chosen bone's weights, lit by the key light (no texture).
static void DrawHeat(const ShapeDraw& s)
{
	V3 key = Norm(V3(0.35f, 0.6f, 0.7f));
	std::vector<LineVertex> v(s.verts.size());
	for (size_t i = 0; i < s.verts.size(); i++) {
		auto& m = s.verts[i];
		V3 n(m.nx, m.ny, m.nz);
		float l = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		float shade = l > 1e-6f ? 0.55f + 0.45f * std::fabs((n.x * key.x + n.y * key.y + n.z * key.z) / l) : 1.0f;
		v[i] = { m.px, m.py, m.pz, Heat(i < s.weights.size() ? HeatWeight(s, (int)i) : 0.0f, shade) };
	}
	gDev->SetVertexShader(nullptr);
	gDev->SetPixelShader(nullptr);
	gDev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	gDev->SetTexture(0, nullptr);
	gDev->SetRenderState(D3DRS_LIGHTING, FALSE);
	gDev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)v.size(), (UINT)s.indices.size() / 3, s.indices.data(), D3DFMT_INDEX16, v.data(), sizeof(LineVertex));
	// Back to the item's shaders for the next shape.
	gDev->SetVertexShader(gVS);
	gDev->SetPixelShader(gPS);
	gDev->SetVertexDeclaration(gDecl);
}

// The Make SMP window (maker.h, included before the panels): the shape hovered in its lists, its paint overlay.
static std::string gHighlightShape;
namespace maker { static bool DrawOverlay(const ShapeDraw& s); static void DrawPaths(V3 eye); }

static void DrawScene(const M4& viewProj, const M4& view, const M4& proj)
{
	gDev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	gDev->SetRenderState(D3DRS_ZENABLE, TRUE);
	gDev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	gDev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	gDev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
	M4 id;
	gDev->SetTransform(D3DTS_WORLD, (D3DMATRIX*)&id);
	gDev->SetTransform(D3DTS_VIEW, (D3DMATRIX*)&view);
	gDev->SetTransform(D3DTS_PROJECTION, (D3DMATRIX*)&proj);

	// The floor: a grid around the body.
	if (gS.showFloor) {
		std::vector<LineVertex> grid;
		const float cell = 32.0f;
		const int n = 20;
		float cx = std::floor(gMotion.pos.x / cell) * cell, cy = std::floor(gMotion.pos.y / cell) * cell;
		for (int i = -n; i <= n; i++) {
			D3DCOLOR c = i == 0 ? D3DCOLOR_ARGB(255, 90, 95, 105) : D3DCOLOR_ARGB(255, 62, 66, 74);
			grid.push_back({ cx + i * cell, cy - n * cell, 0, c });
			grid.push_back({ cx + i * cell, cy + n * cell, 0, c });
			grid.push_back({ cx - n * cell, cy + i * cell, 0, c });
			grid.push_back({ cx + n * cell, cy + i * cell, 0, c });
		}
		DrawLines(grid, D3DPT_LINELIST, false);
	}

	// The item.
	gDev->SetVertexShader(gVS);
	gDev->SetPixelShader(gPS);
	gDev->SetVertexDeclaration(gDecl);
	gDev->SetVertexShaderConstantF(0, (float*)&viewProj, 4);
	gDev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
	gDev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	gDev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	gDev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 8);
	// Shapes without a usable texture: quiet greys, a little apart so neighbouring shapes read.
	static const float kPalette[][4] = { { 0.46f, 0.44f, 0.42f, 1 }, { 0.40f, 0.41f, 0.43f, 1 }, { 0.44f, 0.42f, 0.40f, 1 }, { 0.38f, 0.39f, 0.41f, 1 }, { 0.48f, 0.46f, 0.44f, 1 }, { 0.42f, 0.43f, 0.45f, 1 } };
	for (size_t i = 0; i < gItem.shapes.size(); i++) {
		auto& s = gItem.shapes[i];
		if (!s.visible || s.indices.empty()) continue;
		SkinShape(s);
		if (maker::DrawOverlay(s)) continue;
		if (!gS.heatBone.empty() || !gHighlightBones.empty()) { DrawHeat(s); continue; }
		bool tex = gS.textures && s.texture && !s.info.uvs.empty();
		const float white[4] = { 1, 1, 1, 1 };
		SetPS(tex ? white : kPalette[i % 6], tex ? 1.0f : 0.0f, s.info.alpha && tex ? 1.0f : 0.0f, 0);
		gDev->SetTexture(0, tex ? s.texture : nullptr);
		gDev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)s.verts.size(), (UINT)s.indices.size() / 3, s.indices.data(), D3DFMT_INDEX16, s.verts.data(), sizeof(MeshVertex));
	}
	// Wireframe and collision shapes on top of the surfaces.
	gDev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
	gDev->SetRenderState(D3DRS_DEPTHBIAS, 0);
	gDev->SetTexture(0, nullptr);
	for (auto& s : gItem.shapes) {
		if (s.indices.empty()) continue;
		// A proxy is hidden (never drawn in game), so it is skinned here when Proxies shows it.
		const char* name = s.info.geometry->m_name;
		const bool proxy = gS.showProxies && name && _strnicmp(name, "SMP Proxy:", 10) == 0;
		if (!s.visible && !proxy) continue;
		if (!s.visible) SkinShape(s);
		bool collider = gS.showColliders && s.collider;
		if (!gS.wireframe && !collider && !proxy) continue;
		const float cloth[4] = { 1.0f, 0.62f, 0.15f, 1 }, body[4] = { 0.25f, 0.85f, 1.0f, 1 }, wire[4] = { 0.05f, 0.05f, 0.05f, 1 }, stand[4] = { 1.0f, 0.35f, 0.85f, 1 };
		SetPS(proxy ? stand : collider ? (s.collider == 1 ? cloth : body) : wire, 0, 0, 1);
		gDev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)s.verts.size(), (UINT)s.indices.size() / 3, s.indices.data(), D3DFMT_INDEX16, s.verts.data(), sizeof(MeshVertex));
	}
	gDev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);

	// Bones and constraints, through everything.
	if (gItem.system && (gS.showBones || gS.showConstraints)) {
		std::vector<LineVertex> lines, points;
		auto P = [](const RE::NiPoint3& p) { return V3(p); };
		auto add = [&](V3 a, V3 b, D3DCOLOR c) { lines.push_back({ a.x, a.y, a.z, c }); lines.push_back({ b.x, b.y, b.z, c }); };
		if (gS.showConstraints) {
			auto addC = [&](hdt::BoneScaleConstraint* c, D3DCOLOR col) {
				auto a = c->m_boneA->m_rig.getWorldTransform().getOrigin(), b = c->m_boneB->m_rig.getWorldTransform().getOrigin();
				add(V3(a.x(), a.y(), a.z()), V3(b.x(), b.y(), b.z()), col);
			};
			for (auto& c : gItem.system->constraints()) addC(c.get(), D3DCOLOR_ARGB(255, 120, 230, 120));
			int gi = 0;
			for (auto& g : gItem.system->constraintGroups()) {
				D3DCOLOR col = (gi++ & 1) ? D3DCOLOR_ARGB(255, 90, 200, 255) : D3DCOLOR_ARGB(255, 120, 230, 120);
				for (auto& c : g->m_constraints) addC(c.get(), col);
			}
		}
		if (gS.showBones) {
			for (auto& b : gItem.system->getBones()) {
				auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
				bool dyn = !bone->m_rig.isStaticOrKinematicObject();
				D3DCOLOR c = dyn ? D3DCOLOR_ARGB(255, 255, 170, 40) : D3DCOLOR_ARGB(255, 150, 160, 180);
				if (!gS.heatBone.empty() && _stricmp(bone->m_name.c_str(), gS.heatBone.c_str()) == 0) c = D3DCOLOR_ARGB(255, 255, 255, 255);   // the heat-map bone
				V3 p = P(bone->m_node->world.translate);
				points.push_back({ p.x, p.y, p.z, c });
				RE::NiAVObject* parent = bone->m_anchor ? (RE::NiAVObject*)bone->m_anchor.get() : (RE::NiAVObject*)bone->m_node->parent;
				if (parent && parent != gItem.scene.skeleton.actor) add(P(parent->world.translate), p, dyn ? D3DCOLOR_ARGB(255, 200, 130, 30) : D3DCOLOR_ARGB(255, 100, 108, 125));
			}
		}
		DrawLines(lines, D3DPT_LINELIST, true);
		DrawLines(points, D3DPT_POINTLIST, true);
	}

	// The chain being edited: its links and bones in white, through everything (Bones on or off).
	if (gItem.system && !gHighlightLinks.empty()) {
		std::map<std::string, V3> at;
		for (auto& b : gItem.system->getBones()) {
			auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
			std::string n = bone->m_name.c_str();
			for (auto& c : n) c = (char)tolower((unsigned char)c);
			at[n] = V3(bone->m_node->world.translate);
		}
		std::vector<LineVertex> lines, points;
		const D3DCOLOR white = D3DCOLOR_ARGB(255, 255, 255, 255);
		for (auto& [a, b] : gHighlightLinks) {
			auto ia = at.find(a), ib = at.find(b);
			if (ia == at.end() || ib == at.end()) continue;
			lines.push_back({ ia->second.x, ia->second.y, ia->second.z, white });
			lines.push_back({ ib->second.x, ib->second.y, ib->second.z, white });
			points.push_back({ ia->second.x, ia->second.y, ia->second.z, white });
			points.push_back({ ib->second.x, ib->second.y, ib->second.z, white });
		}
		DrawLines(lines, D3DPT_LINELIST, true);
		DrawLines(points, D3DPT_POINTLIST, true);
	}
	maker::DrawPaths(CameraEye());
}

// --- the panels -------------------------------------------------------------------------------------------

static void Help(const char* text)
{
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	if (ImGui::BeginItemTooltip()) {
		ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
	}
}

// The settings as the previewer starts (the undo buttons' "default").
static const Settings kDefaults;

static std::string Shown(float v) { char b[32]; snprintf(b, sizeof b, "%g", v); return b; }
static std::string Shown(int v) { return std::to_string(v); }
static std::string Shown(bool v) { return v ? "on" : "off"; }
static std::string Shown(const std::string& v) { return v.empty() ? "(off)" : v; }

// An undo button after the last item: greyed out while the value is as it was, else puts it back. True when clicked.
static bool UndoButton(const char* id, bool differs, const std::string& was)
{
	ImGui::SameLine();
	ImGui::BeginDisabled(!differs);
	ImGui::PushID(id);
	bool clicked = ImGui::SmallButton("undo");
	ImGui::PopID();
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(differs ? "back to %s" : "as it was (%s)", was.c_str());
	return clicked;
}

template <class T>
static bool Undo(const char* id, T& v, const T& was)
{
	if (!UndoButton(id, !(v == was), Shown(was))) return false;
	v = was;
	return true;
}

#include "maker.h"

// Rewrites the physics path the NIF names (on its root and any shape) to gXmlFixPath, keeping the NIF first, and reloads.
static void FixXmlPath()
{
	try {
		nif::File f;
		f.load(gItem.nifPath);
		int n = 0;
		for (auto& s : f.strings) if (_stricmp(s.c_str(), gItem.xmlNamed.c_str()) == 0) { s = gXmlFixPath; ++n; }
		if (!n) { AddLog(4, "the NIF's physics path '" + gItem.xmlNamed + "' was not found in it"); return; }
		maker::Backup(gItem.nifPath);
		f.save(gItem.nifPath);
		AddLog(2, "the NIF now names its physics file " + gXmlFixPath + " (was " + gItem.xmlNamed + ")");
	} catch (std::exception& e) {
		AddLog(4, std::string("could not change the NIF's physics path: ") + e.what());
		return;
	}
	LoadItem(gItem.nifPath);
}

static void Panels(float fps)
{
	gHighlightBones.clear();   // set again below while a chain is hovered, dragged or pinned
	gHighlightLinks.clear();
	gHighlightShape.clear();
	auto* world = hdt::SkyrimPhysicsWorld::get();
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(440, io.DisplaySize.y - 16), ImGuiCond_FirstUseEver);
	ImGui::Begin("NV SMP Previewer");

	if (ImGui::Button("Open NIF...")) OpenDialog();
	ImGui::SameLine();
	ImGui::BeginDisabled(!gItem.loaded);
	if (ImGui::Button("Reload")) LoadItem(gItem.nifPath);
	ImGui::SameLine();
	if (ImGui::Button("Edit XML") && !gItem.xmlPath.empty()) ShellExecuteA(gWnd, "open", gItem.xmlPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	ImGui::SameLine();
	if (ImGui::Button("Folder")) ShellExecuteA(gWnd, "open", fs::path(gItem.nifPath).parent_path().string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Screenshot")) gShotPending = true;
	ImGui::SameLine();
	if (ImGui::Button("Make SMP...")) maker::gM.open = true;
	Help("Paint chains on a plain armour, or set up cloth and collisions, and build it with smpdress, smpify or smprepair without the command line.");
	ImGui::Checkbox("Reload when the files change", &gS.autoReload);
	if (gItem.loaded) {
		ImGui::TextWrapped("NIF: %s", gItem.nifPath.c_str());
		if (gItem.xmlPath.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "XML: %s (not found)", gItem.xmlNamed.empty() ? "none named" : gItem.xmlNamed.c_str());
		else ImGui::TextWrapped("XML: %s", gItem.xmlPath.c_str());
		if (!gXmlFixPath.empty()) {
			ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "The NIF names %s, which the game will not find", gItem.xmlNamed.c_str());
			ImGui::BeginDisabled(maker::gM.running);
			if (ImGui::Button("Point the NIF at this XML")) FixXmlPath();
			ImGui::EndDisabled();
			Help("Changes the physics path the NIF names to the XML next to it, as the game sees it from Data (the folder holding Meshes, or the mod folder in Mod Organizer). The old NIF is kept as .bak; then it reloads.");
		}
		if (gCopiedSkeleton.ok && !gCopiedSkeleton.removed.empty()) {
			ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "The NIF carries a copied skeleton (%zu node(s) nothing uses)", gCopiedSkeleton.removed.size());
			std::string l;
			for (size_t i = 0; i < gCopiedSkeleton.removed.size() && i < 8; i++) l += (i ? ", " : "") + gCopiedSkeleton.removed[i];
			if (gCopiedSkeleton.removed.size() > 8) l += ", ...";
			ImGui::TextDisabled("  %s", l.c_str());
			ImGui::BeginDisabled(maker::gM.running);
			if (ImGui::Button("Remove the copied skeleton")) RemoveCopiedSkeleton();
			ImGui::EndDisabled();
			Help("Takes out the skeleton nodes nothing in the item uses (in game they end up in the actor beside the real skeleton, can catch the "
			     "item's bones by name and draw it off the body). The skeleton bones the skins use stay, moved onto the NIF's root at their place; "
			     "the item's own bones, shapes and colliders stay as they are. The old NIF is kept as .bak; then it reloads.");
		}
		if (!gMissingBoneLists.empty()) {
			ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%zu shape(s) have bones the game drops (no SMP Bones list):", gMissingBoneLists.size());
			for (auto& m : gMissingBoneLists) ImGui::TextDisabled("  %s", m.c_str());
			ImGui::BeginDisabled(maker::gM.running);
			if (ImGui::Button("Add SMP Bones list")) maker::AddBoneLists(gItem.nifPath, gAnim.skeletonPath);
			ImGui::EndDisabled();
			Help("Writes the list into this NIF (the old one is kept as .bak) with smprepair --list, against the game's skeleton found in your Data folders, and reloads it.");
		}
		if (!gMissingProxies.empty()) {
			std::string l;
			for (auto& m : gMissingProxies) l += (l.empty() ? "" : ", ") + m;
			ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "No collision proxies: collides with the full meshes");
			ImGui::TextDisabled("  %s", l.c_str());
			ImGui::BeginDisabled(maker::gM.running || gXmlArchived);
			if (ImGui::Button("Generate collision proxies")) maker::GenerateProxies(gItem.nifPath, gItem.xmlPath, gMissingProxies, 3.0f);
			ImGui::EndDisabled();
			Help(gXmlArchived ? "The XML is inside a BSA archive: extract it to a mod folder first (it is rewritten)."
			                  : "Adds a hidden low-poly stand-in for each of these meshes to the NIF (smpdress --proxy 3) and points the XML's collision at them: far cheaper in game, about the same collisions. Both files are kept first; then it reloads.");
		}
		if (!gProxyRenames.empty()) {
			bool fixable = false;
			ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "Collision proxies the XML does not use:");
			for (auto& [from, to] : gProxyRenames) {
				if (from.empty()) ImGui::TextDisabled("  %s (nothing in the XML to point at it)", to.c_str());
				else ImGui::TextDisabled("  %s  ->  %s", from.c_str(), to.c_str());
				fixable |= !from.empty();
			}
			if (fixable) {
				ImGui::BeginDisabled(maker::gM.running || gXmlArchived);
				if (ImGui::Button("Point the XML at them")) UseProxiesInXml();
				ImGui::EndDisabled();
				Help(gXmlArchived ? "The XML is inside a BSA archive: extract it to a mod folder first (it is rewritten)."
				                  : "Renames those collision shapes in the XML to the NIF's proxies (the old file is kept as .bak) and rebuilds. Unsaved Bone values edits are dropped.");
			}
		}
		if (gItem.system)
			ImGui::Text("%zu bones (%d simulated), %d constraints, %zu bodies", gItem.system->getBones().size(), gItem.simulatedBones, gItem.constraintCount, gItem.system->meshes().size());
		ImGui::Text("physics %.2f ms/step   %.0f fps", world->m_averageStepMs, fps);
	} else {
		ImGui::TextWrapped("Open an SMP NIF (or drop one on the window).");
	}

	if (ImGui::CollapsingHeader("Motion", gOpenValues ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Combo("Motion##preset", &gS.preset, kPresets, IM_ARRAYSIZE(kPresets))) {   // own ID: the header is "Motion" too
			// The sneak presets crouch; leaving them stands up.
			bool sneakPreset = gS.preset == kSneak || gS.preset == kSneakWalk;
			if (sneakPreset != gAnim.sneak && (sneakPreset || gS.preset != kDrive)) { gAnim.sneak = sneakPreset; FindAnimations(); }
		}
		if (gS.preset == kDrive) ImGui::TextDisabled("W/S move, Q/E strafe, A/D turn, Shift runs, Space jumps, C sneaks");
		bool canAnimate = !gAnim.skeletonPath.empty();
		ImGui::BeginDisabled(!canAnimate);
		ImGui::Checkbox("Game animations", &gAnim.use);
		ImGui::EndDisabled();
		Help(canAnimate ? "The game's skeleton and the locomotion animations installed loose (MO2 mods, Data), as the game would pick them. Off: a procedural stride."
		                : "No skeleton.nif found loose in the Data folders: the procedural stride is used.");
		if (canAnimate && gAnim.use) {
			bool rescan = ImGui::Combo("Stance", &gAnim.stance, kStances, IM_ARRAYSIZE(kStances));
			ImGui::SameLine();
			rescan |= ImGui::Checkbox("Female", &gAnim.female);
			ImGui::SameLine();
			rescan |= ImGui::Checkbox("Sneak", &gAnim.sneak);
			if (rescan) { FindAnimations(); gAnim.current = gAnim.previous = ClipPlay(); }
			if (ImGui::Button("Play animation file...")) OpenAnimationDialog();
			if (gAnim.current.clip) {
				ImGui::TextDisabled("%s  %.2f / %.2f s", fs::path(gAnim.current.path).filename().string().c_str(), gAnim.current.time - gAnim.current.clip->start, gAnim.current.clip->Duration());
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", gAnim.current.path.c_str());
			}
			if (ImGui::TreeNode("Animations found", "Animations found (%zu of %zu)", gAnim.found.size(), IM_ARRAYSIZE(kActions))) {
				for (const char* a : kActions) {
					auto it = gAnim.found.find(a);
					if (it == gAnim.found.end()) ImGui::TextDisabled("%s%s: not installed", kStancePrefix[gAnim.stance], a);
					else {
						std::string shown = fs::path(it->second).stem().string();
						if (gAnim.borrowed.count(a)) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s%s: borrowed %s (none for this stance)", kStancePrefix[gAnim.stance], a, shown.c_str());
						else ImGui::Text("%s%s%s", kStancePrefix[gAnim.stance], a, shown.find("aim") != std::string::npos && std::string(a) == "idle" ? " (aim)" : "");
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it->second.c_str());
					}
				}
				ImGui::TreePop();
			}
		}
		ImGui::PushItemWidth(-ImGui::GetFontSize() * 7);
		ImGui::SliderFloat("Speed", &gS.speedScale, 0.1f, 2.0f, "%.2f x");
		Undo("speed", gS.speedScale, kDefaults.speedScale);
		ImGui::SliderFloat("Time", &gS.timeScale, 0.05f, 1.0f, "%.2f x");
		Undo("time", gS.timeScale, kDefaults.timeScale);
		Help("Slow motion: the body and the physics both slow down.");
		ImGui::PopItemWidth();
		if (ImGui::Button("Back to start")) { gMotion = Motion(); gAnim.current = gAnim.previous = ClipPlay(); world->resetSystems(); }
		ImGui::TextDisabled("Ctrl + drag: move the body by hand");
	}

	if (ImGui::CollapsingHeader("Physics", gOpenValues ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
		bool changed = false;
		if (ImGui::Button("Reset physics")) { AddLog(2, "physics reset"); BuildSystem(); }
		Help("As the game's F7 menu: throws the physics away and builds it again from the XML, the cloth at rest on the body as it is now.");
		ImGui::SameLine();
		if (ImGui::Button("Reset pose")) world->resetSystems();
		Help("Only puts the cloth back at rest, keeping the physics as built.");
		ImGui::Checkbox("Simulate", &gS.physics);
		Undo("simulate", gS.physics, kDefaults.physics);
		ImGui::PushItemWidth(-ImGui::GetFontSize() * 13);   // room for the labels and the undo buttons
		changed |= ImGui::SliderFloat("Gravity", &gS.gravity, 0.0f, 3.0f, "%.2f x");
		changed |= Undo("gravity", gS.gravity, kDefaults.gravity);
		changed |= ImGui::SliderInt("Rate (min fps)", &gS.minFps, 30, 240);
		changed |= Undo("rate", gS.minFps, kDefaults.minFps);
		Help("Physics steps per second at least (FSMP's min-fps).");
		changed |= ImGui::SliderInt("Max substeps", &gS.maxSubSteps, 1, 16);
		changed |= Undo("substeps", gS.maxSubSteps, kDefaults.maxSubSteps);
		changed |= ImGui::SliderInt("Solver iterations", &gS.solverIterations, 1, 64);
		changed |= Undo("iterations", gS.solverIterations, kDefaults.solverIterations);
		changed |= ImGui::Checkbox("Collisions", &gS.collisions);
		changed |= Undo("collisions", gS.collisions, kDefaults.collisions);
		changed |= ImGui::Checkbox("Collide once per frame", &gS.collideOncePerFrame);
		changed |= Undo("collideonce", gS.collideOncePerFrame, kDefaults.collideOncePerFrame);
		bool rebuild = ImGui::Checkbox("Ground collision", &gS.ground);
		rebuild |= Undo("ground", gS.ground, kDefaults.ground);
		Help("As the game's F7 menu: a ground plane at the feet the cloth rests on when crouching or sitting (the floor grid is at its height).");
		ImGui::SliderFloat("Waist collision", &gS.waistCollision, 0.0f, 1.0f, "%.2f");
		rebuild |= ImGui::IsItemDeactivatedAfterEdit();
		rebuild |= Undo("waist", gS.waistCollision, kDefaults.waistCollision);
		Help("As in the game's F7 menu: the rows the physics file keeps out of collision. 0 = they follow the body, 1 = they collide. Rebuilds on release.");
		if (rebuild && gItem.loaded) BuildSystem();
		changed |= ImGui::Checkbox("Jitter filter", &gS.jitterFilter);
		changed |= Undo("filter", gS.jitterFilter, kDefaults.jitterFilter);
		Help("As in the game's F7 menu: smooths the small fast shaking of the cloth (what is drawn, not the physics). The Chains readouts in Bone values show the jitter left.");
		if (gS.jitterFilter) {
			changed |= ImGui::SliderFloat("Jitter smoothing", &gS.jitterSmoothing, 0.0f, 1.0f, "%.2f");
			changed |= Undo("smoothing", gS.jitterSmoothing, kDefaults.jitterSmoothing);
		}
		changed |= ImGui::Checkbox("Wind", &gS.wind);
		changed |= Undo("wind", gS.wind, kDefaults.wind);
		if (gS.wind) {
			changed |= ImGui::SliderFloat("Wind strength", &gS.windStrength, 0.0f, 10.0f, "%.2f");
			changed |= Undo("windstrength", gS.windStrength, kDefaults.windStrength);
			ImGui::SliderFloat("Wind from", &gS.windDirection, -180.0f, 180.0f, "%.0f deg");
			Undo("winddir", gS.windDirection, kDefaults.windDirection);
			ImGui::Checkbox("Gusts", &gS.windGusts);
			Undo("gusts", gS.windGusts, kDefaults.windGusts);
		}
		ImGui::PopItemWidth();
		if (changed) ApplyWorldSettings();
	}

	if (gOpenValues) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
	if (ImGui::CollapsingHeader("Bone values")) {
		static bool live = false;
		if (gXml.blocks.empty()) ImGui::TextDisabled(gItem.xmlPath.empty() ? "No physics file loaded." : "Nothing to edit in the physics file.");
		else {
			bool rebuild = false;
			static float stiff = 0, heavy = 0, lively = 0;   // the master sliders (below), at the middle for each file read
			static int forVersion = -1;
			if (forVersion != gXml.version) { stiff = heavy = lively = 0; forVersion = gXml.version; }
			ImGui::BeginDisabled(!gXml.Edited());
			if (ImGui::Button("Save to XML")) {
				// A backup of the file as it was before the first save from here.
				std::string bak = gItem.xmlPath + ".bak";
				if (!fs::exists(bak)) { std::error_code ec; fs::copy_file(gItem.xmlPath, bak, ec); }
				std::ofstream o(gItem.xmlPath, std::ios::binary);
				if (o) {
					o << gXml.Compose();
					o.close();
					AddLog(2, "saved the bone values to " + gItem.xmlPath + " (the file as it was: " + bak + ")");
					rebuild = true;   // reads the saved file afresh: its values become the new originals
				} else AddLog(4, "could not write " + gItem.xmlPath);
			}
			ImGui::SameLine();
			if (ImGui::Button("Reset values")) { gXml.Reset(); stiff = heavy = lively = 0; rebuild = true; }
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::Checkbox("Rebuild while dragging", &live);
			Help("The physics is rebuilt from the file with these values when a slider is let go (the cloth starts again at rest). "
			     "While dragging: rebuilt every change. Nothing is written to the file until Save. Ctrl + click a slider to type a value.");

			// The master sliders: the whole cloth at once (xmledit::ApplyMasters). The middle is the file.
			auto master = [&](const char* id, const char* left, const char* right, float& v, const char* help) {
				ImGui::PushID(id);
				ImGui::TextUnformatted(left);
				ImGui::SameLine(ImGui::GetFontSize() * 4.5f);
				ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 7.5f);
				bool changed = ImGui::SliderFloat("##v", &v, -1.0f, 1.0f, v == 0 ? "as the file" : "%+.2f");
				bool released = ImGui::IsItemDeactivatedAfterEdit();
				if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { v = 0; changed = released = true; }   // right click: back to the file
				ImGui::SameLine();
				ImGui::TextUnformatted(right);
				if (UndoButton("undo", v != 0, "as the file")) { v = 0; changed = released = true; }
				Help(help);
				ImGui::PopID();
				if (changed) { xmledit::ApplyMasters(gXml, stiff, heavy); xmledit::ApplyCalm(gXml, lively); }
				if (released || (live && changed)) rebuild = true;
			};
			master("stiff", "Soft", "Stiff", stiff,
				"How firmly the cloth keeps its shape. Stiff: it springs back to the shape it was modelled in and bends less. "
				"Soft: it relaxes and hangs, folding freely. Sets every cloth joint's springs (angularStiffness) and swing limits. Right click: back to the file.");
			master("heavy", "Flowy", "Heavy", heavy,
				"How heavy the cloth feels. Heavy: it moves slowly and settles, as thick wool or leather. Flowy: light and lively, it flutters "
				"and swings. Sets the cloth bones' damping. Right click: back to the file.");
			master("lively", "Calm", "Lively", lively,
				"How much the cloth's joints may jitter. Calm: each joint's swing is braked, so small shakes die out at once (too calm feels "
				"sluggish). Lively: little braking, so it reacts to every jolt. Sets the cloth joints' damping (angularDamping). Right click: back to the file.");

			// Jitter: the plugin's own measure (SkyrimBone::m_jitter, on what is drawn; m_rawJitter, the physics before
			// the jitter filter), averaged over a chain's bones.
			auto lower = [](std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; };
			std::map<std::string, hdt::SkyrimBone*> boneByName;
			if (gItem.system)
				for (auto& b : gItem.system->getBones()) boneByName[lower(b->m_name.c_str())] = static_cast<hdt::SkyrimBone*>(b.get());
			auto chainJitterOf = [&](const xmledit::Chain& c, bool raw) {
				float s = 0; int k = 0;
				for (auto& l : c.links) {
					auto it = boneByName.find(lower(l.bodyA));
					if (it == boneByName.end() || it->second->m_rig.isStaticOrKinematicObject() || it->second->m_jitterSamples < 3) continue;
					s += raw ? it->second->m_rawJitter : it->second->m_jitter; k++;
				}
				return k ? s / k : -1.0f;
			};
			auto chainJitter = [&](const xmledit::Chain& c) { return chainJitterOf(c, false); };

			// Chains: each cloth column and rope on its own.
			if (gOpenValues) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
			if (ImGui::TreeNode("Chains", "Chains (%zu)", gXml.chains.size())) {
				Help("Each chain (a cloth column from the collar down, a rope) on its own: Soft-Stiff and Calm-Lively as the master sliders, "
				     "for that chain only, on top of the templates. Jitter: the plugin's measure of the zig-zag shaking left in what is drawn (units per physics step; green under 0.005, yellow under 0.02, red above). Hover it for the physics before the jitter filter (Physics > Jitter filter). "
				     "show: the chain's middle bone in the weights heat map, to see which part of the cloth it is. Right click a slider: back to the template.");
				float meanJ = 0;
				{ int k = 0; for (auto& c : gXml.chains) { float j = chainJitter(c); if (j >= 0) { meanJ += j; k++; } } if (k) meanJ /= k; }
				if (meanJ > 0) ImGui::TextDisabled("average jitter %.4f (physics before the filter %.4f)", meanJ, [&] { float s = 0; int k = 0; for (auto& c : gXml.chains) { float r = chainJitterOf(c, true); if (r >= 0) { s += r; k++; } } return k ? s / k : 0.0f; }());
				// The chain being edited: the one hovered or dragged, else the one pinned with its show button.
				static int pinned = -1, pinnedFor = -1;
				if (pinnedFor != gXml.version) { pinned = gPinChain; pinnedFor = gXml.version; }
				int focus = -1;
				static int shown = -1;   // last frame's highlighted chain, to colour its row
				for (size_t ci = 0; ci < gXml.chains.size(); ci++) {
					auto& c = gXml.chains[ci];
					ImGui::PushID((int)ci);
					ImGui::BeginGroup();
					int removed = 0;
					for (auto& l : c.links) removed += l.removed;
					bool edited = removed > 0;
					for (auto& l : c.links) edited |= l.stiff != l.origStiff || l.lively != l.origLively || l.sway != l.origSway;
					float j = chainJitter(c);
					ImVec4 jc = j < 0 ? ImVec4(0.6f, 0.6f, 0.6f, 1) : j < 0.005f ? ImVec4(0.5f, 0.9f, 0.5f, 1) : j < 0.02f ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(1, 0.45f, 0.4f, 1);
					bool lit = (int)ci == shown;
					// Physics on or off for the whole chain (ticked only while every constraint is on): off, its constraints are removed and its
					// bones follow the animation as modelled.
					{
						bool on = removed == 0;
						if (ImGui::Checkbox("##physics", &on)) { for (auto& l : c.links) l.removed = !on; rebuild = true; }
						if (ImGui::IsItemHovered()) ImGui::SetTooltip(on ? "physics on: untick to remove this chain's constraints (its bones then follow the animation)" : "physics off (or partly): tick to put every constraint back");
						ImGui::SameLine();
					}
					if (lit || edited) ImGui::PushStyleColor(ImGuiCol_Text, lit ? ImVec4(0.45f, 0.9f, 1, 1) : ImVec4(1, 0.8f, 0.4f, 1));
					ImGui::TextUnformatted(c.label.c_str());
					if (lit || edited) ImGui::PopStyleColor();
					ImGui::SameLine();
					if (j >= 0) {
						ImGui::TextColored(jc, "jitter %.4f", j);
						if (ImGui::IsItemHovered()) ImGui::SetTooltip("before the jitter filter: %.4f", chainJitterOf(c, true));
					}
					ImGui::SameLine();
					if (ImGui::SmallButton(pinned == (int)ci ? "hide" : "show")) pinned = pinned == (int)ci ? -1 : (int)ci;
					if (UndoButton("physics", removed > 0, "every constraint on")) { for (auto& l : c.links) l.removed = false; rebuild = true; }
					// Two sliders side by side, each with its undo (back to the value the file has, or the template's).
					const float undoW = ImGui::CalcTextSize("undo").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
					const float sliderW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f - undoW;
					// A slider with its undo; soft-stiff goes to 2 (past 1 the swing narrows, to none at 2: locked).
					// Returns 1 when moved, 2 when undone (back to was).
					auto slider = [&](const char* id, const char* fmtL, float& v, float hi, bool differs, const std::string& wasText, const char* tip = nullptr) {
						ImGui::SetNextItemWidth(sliderW);
						char fmt[48];
						if (v >= 2.0f) snprintf(fmt, sizeof fmt, "locked");
						else snprintf(fmt, sizeof fmt, v == 0 ? "%s" : "%s %%+.2f", fmtL);
						bool ch = ImGui::SliderFloat(id, &v, -1.0f, hi, fmt);
						if (ImGui::IsItemActive()) focus = (int)ci;
						if (hi > 1 && ImGui::IsItemHovered()) ImGui::SetTooltip("-1 soft .. 1 stiff; past 1 the joint swings less and less, at 2 it is locked: the link holds its pose on the bone it hangs from (static at a knot).");
						if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
						bool rel = ImGui::IsItemDeactivatedAfterEdit();
						if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { v = 0; ch = rel = true; }
						int result = ch || rel ? 1 : 0;
						if (UndoButton(id, differs, wasText)) result = 2;
						if (rel || result == 2 || (live && ch)) rebuild = true;
						return result;
					};
					// The chain's sliders set every constraint of the chain; undo puts each back as the file has it.
					{
						bool stiffDiffers = false, livelyDiffers = false;
						for (auto& l : c.links) { stiffDiffers |= l.stiff != l.origStiff; livelyDiffers |= l.lively != l.origLively; }
						int r = slider("##stiff", "soft-stiff", c.stiff, 2.0f, stiffDiffers, "each constraint as in the file");
						if (r == 1) for (auto& l : c.links) l.stiff = c.stiff;
						if (r == 2) { for (auto& l : c.links) l.stiff = l.origStiff; c.stiff = c.origStiff; }
						ImGui::SameLine();
						r = slider("##lively", "calm-lively", c.lively, 1.0f, livelyDiffers, "each constraint as in the file");
						if (r == 1) for (auto& l : c.links) l.lively = c.lively;
						if (r == 2) { for (auto& l : c.links) l.lively = l.origLively; c.lively = c.origLively; }
						bool swayDiffers = false;
						for (auto& l : c.links) swayDiffers |= l.sway != l.origSway;
						r = slider("##sway", "side sway", c.sway, 1.0f, swayDiffers, "each constraint as in the file", kSwayTip);
						if (r == 1) for (auto& l : c.links) l.sway = c.sway;
						if (r == 2) { for (auto& l : c.links) l.sway = l.origSway; c.sway = c.origSway; }
					}
					// Each constraint on its own: unticked, it is removed and its bone (the first named) follows the animation.
					if (ImGui::TreeNode("##links", "constraints (%d of %zu on)", (int)c.links.size() - removed, c.links.size())) {
						for (size_t li = 0; li < c.links.size(); li++) {
							auto& l = c.links[li];
							ImGui::PushID((int)li);
							bool on = !l.removed;
							std::string label = l.bodyA + "  ->  " + l.bodyB;
							if (ImGui::Checkbox(label.c_str(), &on)) { l.removed = !on; rebuild = true; }
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s hangs from %s (template %s). Untick: the constraint is removed and %s follows the animation.", l.bodyA.c_str(), l.bodyB.c_str(), l.tmpl.c_str(), l.bodyA.c_str());
							if (UndoButton("link", l.removed, "on")) { l.removed = false; rebuild = true; }
							// This constraint's own sliders (the chain's set them all).
							if (!l.removed) {
								ImGui::Indent();
								auto was = [](float v) { return v == 0 ? std::string("the template") : Shown(v); };
								if (slider("##lstiff", "soft-stiff", l.stiff, 2.0f, l.stiff != l.origStiff, was(l.origStiff)) == 2) l.stiff = l.origStiff;
								ImGui::SameLine();
								if (slider("##llively", "calm-lively", l.lively, 1.0f, l.lively != l.origLively, was(l.origLively)) == 2) l.lively = l.origLively;
								if (slider("##lsway", "side sway", l.sway, 1.0f, l.sway != l.origSway, was(l.origSway), kSwayTip) == 2) l.sway = l.origSway;
								ImGui::Unindent();
							}
							ImGui::PopID();
						}
						ImGui::TreePop();
					}
					ImGui::EndGroup();
					if (focus < 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) focus = (int)ci;
					ImGui::PopID();
				}
				shown = focus >= 0 ? focus : pinned;
				if (shown >= 0 && shown < (int)gXml.chains.size()) {
					auto lower = [](std::string s) { for (auto& ch : s) ch = (char)tolower((unsigned char)ch); return s; };
					for (auto& l : gXml.chains[shown].links) {
						gHighlightBones.insert(lower(l.bodyA));
						gHighlightBones.insert(lower(l.bodyB));
						gHighlightLinks.push_back({ lower(l.bodyA), lower(l.bodyB) });
					}
				}
				ImGui::TreePop();
			}
			ImGui::Separator();
			ImGui::PushItemWidth(-ImGui::GetFontSize() * 14);   // room for the longer labels (angularLowerLimit) and the undo
			for (size_t b = 0; b < gXml.blocks.size(); b++) {
				auto& block = gXml.blocks[b];
				bool blockEdited = false;
				for (auto& f : block.fields) for (int s : f.slots) blockEdited |= gXml.slots[s].value != gXml.slots[s].original;
				ImGui::PushID((int)b);
				if (blockEdited) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.8f, 0.4f, 1));
				bool open = ImGui::TreeNode("##block", "%s%s", block.label.c_str(), blockEdited ? "  *" : "");
				if (blockEdited) ImGui::PopStyleColor();
				if (open) {
					for (size_t fi = 0; fi < block.fields.size(); fi++) {
						auto& f = block.fields[fi];
						ImGui::PushID((int)fi);
						float lo, hi;
						xmledit::Range(f.label, gXml.slots[f.slots[0]].value, lo, hi);
						bool edited = false;
						for (int s : f.slots) edited |= gXml.slots[s].value != gXml.slots[s].original;
						bool changedNow = false;
						if (f.slots.size() == 3) {
							float v[3] = { gXml.slots[f.slots[0]].value, gXml.slots[f.slots[1]].value, gXml.slots[f.slots[2]].value };
							for (float x : v) { if (x < lo) lo = x * 2; if (x > hi) hi = x * 2; }
							changedNow = ImGui::SliderFloat3(f.label.c_str(), v, lo, hi, "%.3g");
							if (changedNow) for (int k = 0; k < 3; k++) gXml.slots[f.slots[k]].value = v[k];
						} else {
							for (int s : f.slots) { ImGui::PushID(s); changedNow |= ImGui::SliderFloat(f.label.c_str(), &gXml.slots[s].value, lo, hi, "%.3g"); ImGui::PopID(); }
						}
						bool released = ImGui::IsItemDeactivatedAfterEdit();
						{
							std::string was;
							for (int s : f.slots) { char t[32]; snprintf(t, sizeof t, "%s%g", was.empty() ? "" : " ", gXml.slots[s].original); was += t; }
							if (UndoButton("undo", edited, "the file's " + was)) { for (int s : f.slots) gXml.slots[s].value = gXml.slots[s].original; rebuild = true; }
						}
						if (released || (live && changedNow)) rebuild = true;
						ImGui::PopID();
					}
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			ImGui::PopItemWidth();
			if (rebuild && gItem.loaded) BuildSystem();
		}
	}

	if (ImGui::CollapsingHeader("Display", gOpenValues ? 0 : ImGuiTreeNodeFlags_DefaultOpen)) {
		// Two toggles a line, each with its undo.
		auto toggle = [](const char* label, bool& v, const bool& was, bool sameLine) {
			if (sameLine) ImGui::SameLine(ImGui::GetContentRegionMax().x * 0.5f);
			ImGui::Checkbox(label, &v);
			Undo(label, v, was);
		};
		toggle("Textures", gS.textures, kDefaults.textures, false);
		toggle("Wireframe", gS.wireframe, kDefaults.wireframe, true);
		toggle("Floor", gS.showFloor, kDefaults.showFloor, false);
		toggle("Bones", gS.showBones, kDefaults.showBones, true);
		toggle("Constraints", gS.showConstraints, kDefaults.showConstraints, false);
		toggle("Collision", gS.showColliders, kDefaults.showColliders, true);
		toggle("Proxies", gS.showProxies, kDefaults.showProxies, false);
		Help("Bones: orange simulated, grey animated. Constraints: the links between bones. Collision: orange = cloth (per-vertex), blue = body (per-triangle). Proxies: the hidden low-poly collision stand-ins ('SMP Proxy:'), magenta.");
		ImGui::ColorEdit3("Background", gS.background, ImGuiColorEditFlags_NoInputs);
		{
			bool differs = memcmp(gS.background, kDefaults.background, sizeof gS.background) != 0;
			if (UndoButton("background", differs, "the default grey")) memcpy(gS.background, kDefaults.background, sizeof gS.background);
		}
		if (gItem.loaded) {
			// Weights: one bone's skin weights as a heat map on the item.
			static char filter[64] = "";
			ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 9);   // room for the label and the undo
			if (ImGui::BeginCombo("Weights", gS.heatBone.empty() ? "(off)" : gS.heatBone.c_str(), ImGuiComboFlags_HeightLarge)) {
				if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
				ImGui::InputTextWithHint("##filter", "filter", filter, sizeof filter);
				if (ImGui::Selectable("(off)", gS.heatBone.empty())) gS.heatBone.clear();
				for (auto& n : SkinBoneNames()) {
					if (filter[0]) {
						std::string a = n, b = filter;
						for (auto& c : a) c = (char)tolower((unsigned char)c);
						for (auto& c : b) c = (char)tolower((unsigned char)c);
						if (a.find(b) == std::string::npos) continue;
					}
					bool sel = _stricmp(n.c_str(), gS.heatBone.c_str()) == 0;
					if (ImGui::Selectable(n.c_str(), sel)) gS.heatBone = n;
					if (sel && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY();
				}
				ImGui::EndCombo();
			}
			Undo("weights", gS.heatBone, kDefaults.heatBone);
			Help("Shows how much each vertex follows the chosen bone (its skin weight): dark blue 0, through blue, cyan, green and yellow, to red 1. "
			     "Click a bone's dot in the view (Bones on) to pick it. The chosen bone's dot turns white.");
			if (!gS.heatBone.empty()) {
				ImDrawList* dl = ImGui::GetWindowDrawList();
				ImVec2 p = ImGui::GetCursorScreenPos();
				float w = ImGui::GetContentRegionAvail().x, h = 10;
				for (int i = 0; i < 64; i++) {
					D3DCOLOR c = Heat(i / 63.0f);
					dl->AddRectFilled(ImVec2(p.x + w * i / 64, p.y), ImVec2(p.x + w * (i + 1) / 64 + 1, p.y + h), IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255));
				}
				ImGui::Dummy(ImVec2(w, h));
				// How many vertices the bone moves, over all shapes.
				int moved = 0, full = 0;
				for (auto& s : gItem.shapes)
					for (int v = 0; v < (int)s.weights.size(); v++) { float wt = HeatWeight(s, v); moved += wt > 0; full += wt > 0.999f; }
				ImGui::TextDisabled("0          0.5          1     %d vertices weighted, %d fully", moved, full);
			}
		}
		if (gItem.loaded && ImGui::TreeNode("Shapes")) {
			// Per shape, a pick for "Generate proxies for the picked shapes" below (skinned shapes only: a proxy is skinned).
			static std::string pickedFor;
			static std::set<std::string> picked;
			static int proxyMaxTris = 1000;
			if (pickedFor != gItem.nifPath) { picked.clear(); pickedFor = gItem.nifPath; }
			for (size_t i = 0; i < gItem.shapes.size(); i++) {
				auto& s = gItem.shapes[i];
				ImGui::PushID((int)i);   // shapes can share a name
				const char* name = s.info.geometry->m_name;
				if (name && s.info.geometry->skinInstance) {
					bool pick = picked.count(name) != 0;
					if (ImGui::Checkbox("##proxy", &pick)) { if (pick) picked.insert(name); else picked.erase(name); }
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(_strnicmp(name, "SMP Proxy:", 10) == 0 ? "Rebuild this proxy low-poly (%zu triangles now), under the same name"
						                                                        : "Make a collision proxy for this shape (%zu triangles)", s.indices.size() / 3);
					ImGui::SameLine();
				}
				ImGui::Checkbox(s.info.geometry->m_name ? s.info.geometry->m_name : "(unnamed)", &s.visible);
				if (s.collider) { ImGui::SameLine(); ImGui::TextColored(s.collider == 1 ? ImVec4(1, 0.62f, 0.15f, 1) : ImVec4(0.25f, 0.85f, 1, 1), s.collider == 1 ? "cloth" : "body"); }
				if (!s.textureNote.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", s.info.diffuse.c_str(), s.textureNote.c_str());
				{ bool shown = true; Undo("visible", s.visible, shown); }
				ImGui::PopID();
			}
			std::vector<std::string> shapes;
			for (auto& s : gItem.shapes)
				if (s.info.geometry->m_name && picked.count(s.info.geometry->m_name) &&
				    std::find(shapes.begin(), shapes.end(), s.info.geometry->m_name) == shapes.end())
					shapes.push_back(s.info.geometry->m_name);
			ImGui::SetNextItemWidth(120);
			if (ImGui::InputInt("Max triangles", &proxyMaxTris, 100, 1000)) proxyMaxTris = std::clamp(proxyMaxTris, 50, 65535);
			Help("Each proxy is made as fine as it can be within this many triangles (its vertices merged in cells, the cells grown until it fits). Fewer is cheaper for the physics and rougher.");
			ImGui::BeginDisabled(shapes.empty() || maker::gM.running || gXmlArchived);
			if (ImGui::Button(shapes.empty() ? "Generate proxies (tick shapes above)" : ("Generate proxies for " + std::to_string(shapes.size()) + " shape(s)").c_str())) {
				maker::GenerateProxies(gItem.nifPath, gItem.xmlPath, shapes, 1.0f, proxyMaxTris);
				picked.clear();
			}
			ImGui::EndDisabled();
			Help("For each ticked shape, a hidden low-poly stand-in in the NIF ('SMP Proxy:<shape>': its own properties, a never-pass alpha, "
			     "not culled; the plugin hides it in game) and the XML's collision against that shape pointed at it. A ticked proxy is rebuilt "
			     "low-poly in place; a shape's existing proxy is replaced. Both files are kept first; then it reloads.");
			ImGui::TreePop();
		}
	}

	if (ImGui::CollapsingHeader("Data sources")) {
		auto& r = gItem.roots;
		bool changed = false;
		ImGui::TextDisabled("Mod Organizer 2");
		if (r.mo2Instance.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "none found");
		else { ImGui::TextWrapped("%s", r.mo2Instance.c_str()); ImGui::TextDisabled("(%s)", r.mo2How.c_str()); }
		if (ImGui::Button("Choose instance...")) {
			std::string f = PickFile("MO2 settings (ModOrganizer.ini)\0ModOrganizer.ini\0", r.mo2Instance);
			if (!f.empty()) { gSources.mo2Instance = fs::path(f).parent_path().string(); gSources.mo2Profile.clear(); changed = true; }
		}
		Help("A portable instance's folder, or a global one's (%LOCALAPPDATA%\\ModOrganizer\\<name>): pick its ModOrganizer.ini.");
		ImGui::SameLine();
		ImGui::BeginDisabled(gSources.mo2Instance.empty());
		if (ImGui::Button("Auto##mo2")) { gSources.mo2Instance.clear(); gSources.mo2Profile.clear(); changed = true; }
		ImGui::EndDisabled();
		if (!r.mo2Profiles.empty() && ImGui::BeginCombo("Profile", r.mo2Profile.c_str())) {
			for (auto& p : r.mo2Profiles)
				if (ImGui::Selectable(p.c_str(), p == r.mo2Profile)) { gSources.mo2Profile = p; changed = true; }
			ImGui::EndCombo();
		}

		ImGui::Spacing();
		ImGui::TextDisabled("Game");
		if (r.gameData.empty()) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "not found");
		else { ImGui::TextWrapped("%s", r.gameData.c_str()); ImGui::TextDisabled("(%s)", r.gameHow.c_str()); }
		if (ImGui::Button("Choose game...")) {
			std::string f = PickFile("Fallout New Vegas (FalloutNV.exe)\0FalloutNV.exe\0", r.gameData);
			if (!f.empty()) { gSources.gameFolder = fs::path(f).parent_path().string(); changed = true; }
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(gSources.gameFolder.empty());
		if (ImGui::Button("Auto##game")) { gSources.gameFolder.clear(); changed = true; }
		ImGui::EndDisabled();

		ImGui::Spacing();
		char label[64];
		snprintf(label, sizeof label, "Read BSA archives (%zu)", r.archives.size());
		changed |= ImGui::Checkbox(label, &gSources.useArchives);
		Help("The vanilla and DLC archives and the mods' own: animations, skeletons and textures that are not installed loose. Loose files win, as in game.");

		ImGui::TextDisabled("Extra folders");
		Help("Searched after the mods, before the game: a Data folder of a manual install, a mod being made outside MO2...");
		for (size_t i = 0; i < gSources.extraFolders.size(); i++) {
			ImGui::PushID((int)i);
			bool remove = ImGui::SmallButton("x");
			ImGui::SameLine();
			ImGui::TextUnformatted(gSources.extraFolders[i].c_str());
			ImGui::PopID();
			if (remove) { gSources.extraFolders.erase(gSources.extraFolders.begin() + i); changed = true; break; }
		}
		if (ImGui::Button("Add folder...")) {
			std::string f = PickFolder();
			if (!f.empty()) { gSources.extraFolders.push_back(f); changed = true; }
		}

		for (auto& n : r.notes) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "%s", n.c_str());
		if (ImGui::TreeNode("Search order", "Search order (%zu folders, %zu archives)", r.folders.size(), r.archives.size())) {
			for (auto& f : r.folders) ImGui::TextUnformatted(f.c_str());
			for (auto& a : r.archives) ImGui::TextDisabled("%s (%zu files)", a->Path().c_str(), a->FileCount());
			ImGui::TreePop();
		}
		if (changed) {
			SaveSources();
			if (gItem.loaded) LoadItem(gItem.nifPath);
		}
	}
	ImGui::TextDisabled("Right drag: orbit   Middle drag: pan   Wheel: zoom   F: recentre");
	ImGui::End();

	// The log.
	ImGui::SetNextWindowPos(ImVec2(376, io.DisplaySize.y - 168), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - 384, 160), ImGuiCond_FirstUseEver);
	ImGui::Begin("Log");
	if (ImGui::SmallButton("Clear")) gLog.clear();
	ImGui::BeginChild("lines");
	for (auto& e : gLog) {
		ImVec4 c = e.level >= 4 ? ImVec4(1, 0.45f, 0.4f, 1) : e.level == 3 ? ImVec4(1, 0.8f, 0.4f, 1) : ImVec4(0.8f, 0.82f, 0.86f, 1);
		ImGui::TextColored(c, "%s", e.text.c_str());
	}
	if (gLogScroll) { ImGui::SetScrollHereY(1.0f); gLogScroll = false; }
	ImGui::EndChild();
	ImGui::End();

	maker::Window();
}

// --- the window -----------------------------------------------------------------------------------------

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
	switch (msg) {
	case WM_SIZE:
		if (wParam != SIZE_MINIMIZED) { gResizeW = LOWORD(lParam); gResizeH = HIWORD(lParam); }
		return 0;
	case WM_DROPFILES: {
		char file[MAX_PATH];
		if (DragQueryFileA((HDROP)wParam, 0, file, MAX_PATH)) LoadItem(file);
		DragFinish((HDROP)wParam);
		return 0;
	}
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
		break;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
	char exe[MAX_PATH];
	GetModuleFileNameA(nullptr, exe, MAX_PATH);
#ifdef NVSMP_MAKER
	// NV SMP Maker: the same program, opening on the Make SMP window (its own settings file).
	gIniPath = (fs::path(exe).parent_path() / "NVSMPMaker.ini").string();
	const wchar_t* kTitle = L"NV SMP Maker";
	maker::gM.open = true;
	gS.preset = 0;   // Stand: the body holds an upright pose to paint on
#else
	gIniPath = (fs::path(exe).parent_path() / "NVSMPPreviewer.ini").string();
	const wchar_t* kTitle = L"NV SMP Previewer";
#endif

	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"NVSMPPreviewer", nullptr };
	RegisterClassExW(&wc);
	gWnd = CreateWindowW(wc.lpszClassName, kTitle, WS_OVERLAPPEDWINDOW, 100, 60, 1600, 960, nullptr, nullptr, inst, nullptr);
	if (!CreateDevice()) {
		MessageBoxA(nullptr, "Could not create a Direct3D 9 device.", "NV SMP Previewer", MB_ICONERROR);
		return 1;
	}
	DragAcceptFiles(gWnd, TRUE);
	ShowWindow(gWnd, SW_SHOWDEFAULT);
	UpdateWindow(gWnd);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	ImGui::StyleColorsDark();
	ImGui::GetStyle().WindowRounding = 4.0f;
	ImGui_ImplWin32_Init(gWnd);
	ImGui_ImplDX9_Init(gDev);
	CompileShaders();
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // the folder picker
	LoadSources();

	ApplyWorldSettings();
	AddLog(2, "NV SMP Previewer: FSMP physics core, Bullet " + std::to_string(btGetVersion()));

	// Command line: [item.nif] and, for automated captures, --motion N --shots prefix --every frames
	// --count n --show bones,constraints,collision,proxies,wire --yaw deg --pitch deg --dist units --noui --exit.
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) {
		char buf[1024];
		WideCharToMultiByte(CP_ACP, 0, argv[i], -1, buf, sizeof buf, nullptr, nullptr);
		args.push_back(buf);
	}
	LocalFree(argv);
	std::string start, shots, jitterOut, clipOut, logOut;   // --jitter / --clip file: how much the cloth shakes / clips (see below); --log file: the log, written on exit
	std::string clipMeshOut;   // --clipmesh file: how much the cloth shapes sink into the body collision shapes (see below)
	std::string recipe;   // --recipe file: the Make SMP window loads it (with --maker-build: and builds it, for testing)
	bool addBoneLists = false;
	bool genProxies = false, useProxies = false, fixXmlPath = false, removeSkeleton = false;
	std::vector<std::string> proxyShapes;   // --proxy-shape: the Shapes list's Generate proxies button, for these, pressed once
	float stroke[4] = { -1, 0, 0, 0 };   // --paint-stroke x0 y0 x1 y1 (window fractions): a simulated brush drag from frame 60 to 120, for testing
	bool buildAfterStroke = false;
	bool weightsTest = false;   // --weights-test: after the stroke, smooth the chain and grow / fade it at point 2, logging the paint, for testing
	int shotEvery = 20, shotCount = 0;
	bool noUi = false, exitAfter = false;
	for (size_t i = 0; i < args.size(); i++) {
		const std::string& a = args[i];
		auto next = [&]() { return i + 1 < args.size() ? args[++i] : std::string(); };
		if (a == "--motion") gS.preset = std::clamp(atoi(next().c_str()), 0, IM_ARRAYSIZE(kPresets) - 1);
		else if (a == "--shots") shots = next();
		else if (a == "--every") shotEvery = std::max(1, atoi(next().c_str()));
		else if (a == "--count") shotCount = atoi(next().c_str());
		else if (a == "--yaw") gCam.yaw = (float)atof(next().c_str()) * 3.14159f / 180.0f;
		else if (a == "--pitch") gCam.pitch = (float)atof(next().c_str()) * 3.14159f / 180.0f;
		else if (a == "--dist") gCam.dist = (float)atof(next().c_str());
		else if (a == "--panz") gCam.pan = V3(0, 0, (float)atof(next().c_str()));
		else if (a == "--noui") noUi = true;
		else if (a == "--values") gOpenValues = true;
		else if (a == "--heat") gS.heatBone = next();
		else if (a == "--chain") gPinChain = atoi(next().c_str());
		else if (a == "--realtime") gRealtime = true;
		else if (a == "--rate") gS.minFps = atoi(next().c_str());
		else if (a == "--step") gMeasureStep = (float)atof(next().c_str());
		else if (a == "--filter") gS.jitterFilter = atoi(next().c_str()) != 0;
		else if (a == "--smoothing") gS.jitterSmoothing = (float)atof(next().c_str());
		else if (a == "--beta") gS.jitterBeta = (float)atof(next().c_str());
		else if (a == "--noanim") gAnim.use = false;
		else if (a == "--sneak") gAnim.sneak = true;
		else if (a == "--noground") gS.ground = false;
		else if (a == "--nocollisions") gS.collisions = false;
		else if (a == "--collide-once") gS.collideOncePerFrame = true;
		else if (a == "--iterations") gS.solverIterations = atoi(next().c_str());
		else if (a == "--threads") gS.collisionThreads = atoi(next().c_str());
		else if (a == "--jitter") jitterOut = next();
		else if (a == "--clip") clipOut = next();
		else if (a == "--clipmesh") clipMeshOut = next();
		else if (a == "--stance") gAnim.stance = std::clamp(atoi(next().c_str()), 0, IM_ARRAYSIZE(kStances) - 1);
		else if (a == "--kf") { gAnim.customPath = next(); gS.preset = kCustom; }
		else if (a == "--exit") exitAfter = true;
		else if (a == "--log") logOut = next();   // testing: the log, written to this file on exit
		else if (a == "--maker") maker::gM.open = true;
		else if (a == "--recipe") recipe = next();
		else if (a == "--maker-build") maker::gM.buildOnOpen = true;
		else if (a == "--add-bone-lists") addBoneLists = true;   // testing: the Add SMP Bones list button, pressed once
		else if (a == "--gen-proxies") genProxies = true;   // testing: the Generate collision proxies button, pressed once
		else if (a == "--proxy-shape") proxyShapes.push_back(next());   // testing: Generate proxies for the picked shapes
		else if (a == "--use-proxies") useProxies = true;   // testing: the Point the XML at them button, pressed once
		else if (a == "--fix-xml-path") fixXmlPath = true;   // testing: the Point the NIF at this XML button, pressed once
		else if (a == "--remove-skeleton") removeSkeleton = true;   // testing: the Remove the copied skeleton button, pressed once
		else if (a == "--async") gAsyncStep = true;
		else if (a == "--paint-stroke") for (int k = 0; k < 4; k++) stroke[k] = (float)atof(next().c_str());
		else if (a == "--weights-test") weightsTest = true;
		else if (a == "--show") {
			std::string s = next();
			gS.showBones = s.find("bones") != std::string::npos;
			gS.showConstraints = s.find("constraints") != std::string::npos;
			gS.showColliders = s.find("collision") != std::string::npos;
			gS.showProxies = s.find("proxies") != std::string::npos;
			gS.wireframe = s.find("wire") != std::string::npos;
		} else if (a.rfind("--", 0) != 0) start = a;
	}
	ApplyWorldSettings();   // again: the command line may have changed them
	if (start.empty()) start = IniGet("LastFile");
	if (!start.empty() && fs::exists(start)) LoadItem(start);
	if (!recipe.empty()) { maker::gM.open = true; maker::LoadRecipe(recipe); }
	if (addBoneLists && !gMissingBoneLists.empty()) maker::AddBoneLists(gItem.nifPath, gAnim.skeletonPath);
	if (useProxies && gItem.loaded) UseProxiesInXml();
	if (fixXmlPath && gItem.loaded && !gXmlFixPath.empty()) FixXmlPath();
	if (removeSkeleton && gItem.loaded && gCopiedSkeleton.ok && !gCopiedSkeleton.removed.empty()) RemoveCopiedSkeleton();
	if (genProxies && !gMissingProxies.empty()) maker::GenerateProxies(gItem.nifPath, gItem.xmlPath, gMissingProxies, 3.0f);
	if (!proxyShapes.empty() && gItem.loaded) maker::GenerateProxies(gItem.nifPath, gItem.xmlPath, proxyShapes, 1.0f, 1000);
	if (stroke[0] >= 0) { maker::gM.open = true; buildAfterStroke = maker::gM.buildOnOpen; maker::gM.buildOnOpen = false; }
	int frameNo = 0, shotsTaken = 0;

	LARGE_INTEGER freq, last, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&last);
	float fps = 60, reloadCheck = 0, wallTime = 0;
	bool done = false;
	while (!done) {
		MSG msg;
		while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessage(&msg);
			if (msg.message == WM_QUIT) done = true;
		}
		if (done) break;
		if (gResizeW && gResizeH) {
			gPP.BackBufferWidth = gResizeW;
			gPP.BackBufferHeight = gResizeH;
			gResizeW = gResizeH = 0;
			ResetDevice();
		}
		QueryPerformanceCounter(&now);
		float dt = std::min(0.1f, (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart);
		last = now;
		wallTime += dt;
		fps += (1.0f / std::max(dt, 1e-4f) - fps) * 0.05f;

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		// Edited files: the XML rebuilds the physics, the NIF reloads everything.
		reloadCheck += dt;
		if (gItem.loaded && gS.autoReload && reloadCheck > 0.5f) {
			reloadCheck = 0;
			if (TimeOf(gItem.nifPath) != gItem.nifTime) LoadItem(gItem.nifPath);
			else if (!gItem.xmlPath.empty() && TimeOf(gItem.xmlPath) != gItem.xmlTime) { AddLog(2, "the XML changed: rebuilding"); BuildSystem(); }
		}

		if (gItem.loaded) {
			float simDt = (jitterOut.empty() && clipOut.empty() && clipMeshOut.empty()) || gRealtime ? dt * gS.timeScale : gMeasureStep;   // measuring: a fixed step, comparable runs (--realtime: the real one)
			const bool painting = maker::Painting();   // the body holds still under the brush
			gNifPose = painting && maker::gM.nifPose;  // and is shown in the NIF's own pose
			UpdateMotion(painting ? 0.0f : simDt);
			UpdateWind(wallTime);
			if (gAsyncStep) {
				// As the plugin does with bAsyncStep: last frame's step written on the pose as it is now, this frame's
				// step started on the physics thread while the frame is drawn.
				hdt::SkyrimPhysicsWorld::get()->finishUpdate();
				if (gS.physics && gItem.system && !painting) hdt::SkyrimPhysicsWorld::get()->beginUpdate(simDt, true);
			} else if (gS.physics && gItem.system && !painting) hdt::SkyrimPhysicsWorld::get()->doUpdate(simDt);

			// --jitter: the simulated bones' frame-to-frame shake in the actor's frame, as the second difference
			// of their positions (smooth swinging leaves it near 0; fighting contacts make it large), over 4 s
			// after 2 s of settling, then written out with the worst bones and the program closes.
			// --clip: how much of the cloth gets inside the legs, against the leg shape fitted from the "Legs"
			// mesh (tapered: a lower-quartile radius at each end of each thigh and shin), overall and for the
			// top two cloth rows (hip height), over the same 4 s.
			if (!clipOut.empty() && gItem.system && frameNo >= 240) {
				struct Seg { RE::NiAVObject* a; RE::NiAVObject* b; float ra, rb; };
				static std::vector<Seg> segs;
				static ShapeDraw* cloth = nullptr;
				static std::vector<int> skirt;
				static std::set<int> upper;
				static int samples = 0, inside = 0, upperSamples = 0, upperInside = 0, frames = 0;
				static float deepest = 0, upperDeepest = 0;
				auto dist = [](const Seg& g, const RE::NiPoint3& p, float& t) {
					RE::NiPoint3 a = g.a->world.translate, ab = g.b->world.translate - a, ap = p - a;
					float len2 = std::max(ab.x * ab.x + ab.y * ab.y + ab.z * ab.z, 1e-6f);
					t = std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2, 0.0f, 1.0f);
					return (p - (a + ab * t)).Length();
				};
				if (segs.empty()) {
					for (const char* side : { "L", "R" }) {
						auto* th = Bone((std::string("Bip01 ") + side + " Thigh").c_str());
						auto* ca = Bone((std::string("Bip01 ") + side + " Calf").c_str());
						auto* fo = Bone((std::string("Bip01 ") + side + " Foot").c_str());
						if (th && ca) segs.push_back({ th, ca, 0, 0 });
						if (ca && fo) segs.push_back({ ca, fo, 0, 0 });
					}
					ShapeDraw* legs = nullptr;
					for (auto& s : gItem.shapes) {
						if (s.info.geometry->m_name && _stricmp(s.info.geometry->m_name, "Legs") == 0) legs = &s;
						if (s.collider == 1 && !cloth) cloth = &s;
					}
					if (legs) {
						std::vector<std::vector<float>> all(segs.size()), nearA(segs.size()), nearB(segs.size());
						for (int v = 0; v < (int)legs->weights.size(); v++) {
							RE::NiPoint3 q = scene::SkinVertex(legs->info.geometry, v, legs->weights);
							int best = -1;
							float bd = 1e9f, bt = 0;
							for (size_t i = 0; i < segs.size(); i++) { float t, d = dist(segs[i], q, t); if (d < bd) { bd = d; bt = t; best = (int)i; } }
							if (best < 0) continue;
							all[best].push_back(bd);
							if (bt < 0.33f) nearA[best].push_back(bd);
							if (bt > 0.67f) nearB[best].push_back(bd);
						}
						auto quartile = [](std::vector<float> v) { std::sort(v.begin(), v.end()); return v[v.size() / 4]; };
						for (size_t i = 0; i < segs.size(); i++) {
							if (all[i].empty()) continue;
							float whole = quartile(all[i]);
							segs[i].ra = nearA[i].size() >= 10 ? quartile(nearA[i]) : whole;
							segs[i].rb = nearB[i].size() >= 10 ? quartile(nearB[i]) : whole;
						}
					}
					if (cloth)
						for (int v = 0; v < (int)cloth->weights.size(); v++)
							for (auto& [slot, w] : cloth->weights[v]) {
								std::string n = cloth->info.geometry->skinInstance->bones[slot]->m_name ? cloth->info.geometry->skinInstance->bones[slot]->m_name : "";
								if (w <= 0.3f || _strnicmp(n.c_str(), "SMP", 3) != 0) continue;
								skirt.push_back(v);
								if (n.size() > 3 && (n.compare(n.size() - 3, 3, " R0") == 0 || n.compare(n.size() - 3, 3, " R1") == 0)) upper.insert(v);
								break;
							}
				}
				if (cloth && frameNo % 2 == 0)
					for (int v : skirt) {
						RE::NiPoint3 p = scene::SkinVertex(cloth->info.geometry, v, cloth->weights);
						float depth = -1e9f;
						for (auto& g : segs) {
							if (g.ra <= 0) continue;
							float t, d = dist(g, p, t);
							depth = std::max(depth, g.ra + (g.rb - g.ra) * t - d);
						}
						bool in = depth > 0.5f;
						samples++; inside += in;
						if (in) deepest = std::max(deepest, depth);
						if (upper.count(v)) { upperSamples++; upperInside += in; if (in) upperDeepest = std::max(upperDeepest, depth); }
					}
				if (++frames == 480) {
					FILE* f = fopen(clipOut.c_str(), "w");
					if (f) {
						fprintf(f, "cloth inside the legs: %.2f%% of samples, deepest %.1f units\n", samples ? 100.0 * inside / samples : 0.0, deepest);
						fprintf(f, "  at hip height (top rows): %.2f%% of samples, deepest %.1f units\n", upperSamples ? 100.0 * upperInside / upperSamples : 0.0, upperDeepest);
						for (auto& g : segs) fprintf(f, "  leg %s: radius %.1f to %.1f\n", g.a->m_name, g.ra, g.rb);
						fclose(f);
					}
					done = true;
				}
			}

			// --clipmesh: the cloth shapes' vertices that hang on SMP bones, against the body collision shapes as the
			// physics sees them (per-triangle shapes, skinned this frame): a vertex counts as inside when the nearest
			// triangle is less than 4 units away and the vertex is behind it by more than 0.3. Every 4th frame of 4 s.
			if (!clipMeshOut.empty() && gItem.system && frameNo >= 240) {
				static int frames = 0;
				struct Stat { long samples = 0, inside = 0; float deepest = 0; };
				static std::map<std::string, Stat> stats;
				if (frames % 4 == 0) {
					struct Tri { RE::NiPoint3 a, b, c, n; };
					std::vector<Tri> tris;
					std::unordered_map<long long, std::vector<int>> grid;
					const float cell = 4.0f;
					auto key = [](int x, int y, int z) { return ((long long)(x & 0xFFFFF) << 40) | ((long long)(y & 0xFFFFF) << 20) | (long long)(z & 0xFFFFF); };
					// A collider that is an smpdress --proxy stand-in is measured by the render mesh it stands for (what shows).
					std::vector<const ShapeDraw*> bodies;
					for (auto& s : gItem.shapes) {
						if (s.collider != 2) continue;
						std::string n = s.info.geometry->m_name ? s.info.geometry->m_name : "";
						const ShapeDraw* use = &s;
						if (n.rfind("SMP Proxy:", 0) == 0)
							for (auto& o : gItem.shapes) if (o.info.geometry->m_name && n.substr(10) == o.info.geometry->m_name) use = &o;
						bodies.push_back(use);
					}
					for (auto* sp : bodies) {
						auto& s = *sp;
						std::vector<RE::NiPoint3> p(s.weights.size());
						for (int v = 0; v < (int)p.size(); v++) p[v] = scene::SkinVertex(s.info.geometry, v, s.weights);
						for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
							Tri tr{ p[s.indices[t]], p[s.indices[t + 1]], p[s.indices[t + 2]] };
							RE::NiPoint3 e1 = tr.b - tr.a, e2 = tr.c - tr.a;
							RE::NiPoint3 n(e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x);
							float l = n.Length();
							if (l < 1e-8f) continue;
							tr.n = n * (1.0f / l);
							int id = (int)tris.size();
							tris.push_back(tr);
							float lo[3] = { std::min({ tr.a.x, tr.b.x, tr.c.x }), std::min({ tr.a.y, tr.b.y, tr.c.y }), std::min({ tr.a.z, tr.b.z, tr.c.z }) };
							float hi[3] = { std::max({ tr.a.x, tr.b.x, tr.c.x }), std::max({ tr.a.y, tr.b.y, tr.c.y }), std::max({ tr.a.z, tr.b.z, tr.c.z }) };
							for (int x = (int)std::floor(lo[0] / cell); x <= (int)std::floor(hi[0] / cell); x++)
								for (int y = (int)std::floor(lo[1] / cell); y <= (int)std::floor(hi[1] / cell); y++)
									for (int z = (int)std::floor(lo[2] / cell); z <= (int)std::floor(hi[2] / cell); z++) grid[key(x, y, z)].push_back(id);
						}
					}
					// The closest point on a triangle (Ericson, Real-Time Collision Detection 5.1.5).
					auto closest = [](const Tri& t, const RE::NiPoint3& p) {
						auto dot = [](const RE::NiPoint3& u, const RE::NiPoint3& v) { return u.x * v.x + u.y * v.y + u.z * v.z; };
						RE::NiPoint3 ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
						float d1 = dot(ab, ap), d2 = dot(ac, ap);
						if (d1 <= 0 && d2 <= 0) return t.a;
						RE::NiPoint3 bp = p - t.b; float d3 = dot(ab, bp), d4 = dot(ac, bp);
						if (d3 >= 0 && d4 <= d3) return t.b;
						float vc = d1 * d4 - d3 * d2;
						if (vc <= 0 && d1 >= 0 && d3 <= 0) return t.a + ab * (d1 / (d1 - d3));
						RE::NiPoint3 cp = p - t.c; float d5 = dot(ab, cp), d6 = dot(ac, cp);
						if (d6 >= 0 && d5 <= d6) return t.c;
						float vb = d5 * d2 - d1 * d6;
						if (vb <= 0 && d2 >= 0 && d6 <= 0) return t.a + ac * (d2 / (d2 - d6));
						float va = d3 * d6 - d5 * d4;
						if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
						float den = 1.0f / (va + vb + vc);
						return t.a + ab * (vb * den) + ac * (vc * den);
					};
					for (auto& s : gItem.shapes) {
						if (s.collider != 1) continue;
						std::string shapeName = s.info.geometry->m_name ? s.info.geometry->m_name : "?";
						for (int v = 0; v < (int)s.weights.size(); v++) {
							bool onSmp = false;
							const char* main = nullptr; float mainW = 0;
							for (auto& [slot, w] : s.weights[v]) {
								const char* n = s.info.geometry->skinInstance->bones[slot]->m_name;
								if (w > 0.3f && n && _strnicmp(n, "SMP", 3) == 0) onSmp = true;
								if (w > mainW) { mainW = w; main = n; }
							}
							if (!onSmp) continue;
							// Ropes per link (the last space-separated part of the main bone's name: L1, L2 ...).
							std::string statKey = shapeName;
							if (main && strstr(main, "Rope")) { const char* sp = strrchr(main, ' '); statKey += std::string(" ") + (sp ? sp + 1 : main); }
							Stat& st = stats[statKey];
							RE::NiPoint3 p = scene::SkinVertex(s.info.geometry, v, s.weights);
							int cx = (int)std::floor(p.x / cell), cy = (int)std::floor(p.y / cell), cz = (int)std::floor(p.z / cell);
							float best = 1e9f, signedD = 0;
							for (int x = cx - 1; x <= cx + 1; x++) for (int y = cy - 1; y <= cy + 1; y++) for (int z = cz - 1; z <= cz + 1; z++) {
								auto it = grid.find(key(x, y, z));
								if (it == grid.end()) continue;
								for (int id : it->second) {
									const Tri& t = tris[id];
									RE::NiPoint3 q = closest(t, p), d = p - q;
									float len = d.Length();
									if (len < best) { best = len; signedD = d.x * t.n.x + d.y * t.n.y + d.z * t.n.z; }
								}
							}
							st.samples++;
							if (best < 4.0f && signedD < -0.3f) { st.inside++; st.deepest = std::max(st.deepest, -signedD); }
						}
					}
					// Rigid pieces (vertices wholly on one SMP rope bone) against each other: one piece's vertices inside
					// another piece's surface.
					for (auto& s : gItem.shapes) {
						if (!s.collider) continue;
						std::map<int, std::vector<int>> pieces;   // bone slot -> vertices
						for (int v = 0; v < (int)s.weights.size(); v++) {
							if (s.weights[v].size() != 1 || s.weights[v][0].second < 0.999f) continue;
							const char* n = s.info.geometry->skinInstance->bones[s.weights[v][0].first]->m_name;
							if (n && strstr(n, "SMP") && strstr(n, "Rope")) pieces[s.weights[v][0].first].push_back(v);
						}
						// Only each bone's biggest connected part (the rigid piece itself, not the rope end tied to it).
						{
							std::vector<int> up(s.weights.size());
							std::iota(up.begin(), up.end(), 0);
							auto root = [&](int x) { while (up[x] != x) x = up[x] = up[up[x]]; return x; };
							std::map<int, int> boneOf;
							for (auto& [slot, vs] : pieces) for (int v : vs) boneOf[v] = slot;
							for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
								int a = s.indices[t], b = s.indices[t + 1], c = s.indices[t + 2];
								auto ia = boneOf.find(a), ib = boneOf.find(b), ic = boneOf.find(c);
								if (ia == boneOf.end() || ib == boneOf.end() || ic == boneOf.end() || ia->second != ib->second || ia->second != ic->second) continue;
								up[root(b)] = root(a); up[root(c)] = root(a);
							}
							// Vertices at the same place count as one (UV seams split them).
							std::map<std::tuple<int, int, int, int>, int> at;
							for (auto& [v, slot] : boneOf) {
								RE::NiPoint3 q = scene::SkinVertex(s.info.geometry, v, s.weights);
								auto key = std::make_tuple(slot, (int)std::lround(q.x * 100), (int)std::lround(q.y * 100), (int)std::lround(q.z * 100));
								auto it = at.find(key);
								if (it == at.end()) at[key] = v;
								else up[root(v)] = root(it->second);
							}
							for (auto& [slot, vs] : pieces) {
								std::map<int, int> size;
								for (int v : vs) size[root(v)]++;
								int big = -1, bigN = 0;
								for (auto& [r, n] : size) if (n > bigN) { bigN = n; big = r; }
								std::vector<int> keep;
								for (int v : vs) if (root(v) == big) keep.push_back(v);
								vs = keep;
							}
						}
						if (pieces.size() < 2) continue;
						std::vector<RE::NiPoint3> p(s.weights.size());
						for (auto& [slot, vs] : pieces) for (int v : vs) p[v] = scene::SkinVertex(s.info.geometry, v, s.weights);
						// Each piece as a cylinder (centre, long axis from its spread, radius 3/4 of its mean radial
						// distance: a broken bottle's jagged rim and neck stay out); the other pieces' vertices inside it.
						Stat& st = stats[std::string("piece vs piece (frames with overlap)")];
						bool overlap = false;
						for (auto& [slotB, vsB] : pieces) {
							RE::NiPoint3 c(0, 0, 0);
							for (int v : vsB) c = c + p[v];
							c = c * (1.0f / vsB.size());
							RE::NiPoint3 ax(0, 0, 1);
							for (int it = 0; it < 12; it++) {
								RE::NiPoint3 n(0, 0, 0);
								for (int v : vsB) { RE::NiPoint3 d = p[v] - c; float k = d.x * ax.x + d.y * ax.y + d.z * ax.z; n = n + d * k; }
								float l = n.Length();
								if (l < 1e-8f) break;
								ax = n * (1.0f / l);
							}
							float t0 = 1e9f, t1 = -1e9f, rs = 0;
							for (int v : vsB) {
								RE::NiPoint3 d = p[v] - c; float t = d.x * ax.x + d.y * ax.y + d.z * ax.z;
								t0 = std::min(t0, t); t1 = std::max(t1, t); rs += (d - ax * t).Length();
							}
							float r = 0.75f * rs / vsB.size();
							for (auto& [slotA, vsA] : pieces) {
								if (slotA == slotB) continue;
								int in = 0;
								for (int v : vsA) {
									RE::NiPoint3 d = p[v] - c; float t = d.x * ax.x + d.y * ax.y + d.z * ax.z;
									if (t < t0 + 0.5f || t > t1 - 0.5f) continue;
									float rad = (d - ax * t).Length();
									if (rad < r) { in++; st.deepest = std::max(st.deepest, r - rad); }
								}
								if (in >= 3) overlap = true;
							}
						}
						st.samples++;
						if (overlap) st.inside++;
					}
				}
				if (++frames == 480) {
					FILE* f = fopen(clipMeshOut.c_str(), "w");
					if (f) {
						for (auto& [name, st] : stats)
							fprintf(f, "%s: %.2f%% of samples inside the body, deepest %.2f units\n", name.c_str(), st.samples ? 100.0 * st.inside / st.samples : 0.0, st.deepest);
						fclose(f);
					}
					done = true;
				}
			}

			static std::map<std::string, std::vector<RE::NiPoint3>> track;
			static int measured = 0;
			if (!jitterOut.empty() && gItem.system && frameNo >= 240) {
				RE::NiTransform toActor = gItem.scene.skeleton.actor->world.Invert();
				// The cape's sway is measured in the upper spine's frame (what it hangs from), not the actor's.
				RE::NiTransform toSpine = toActor;
				for (auto& b : gItem.system->getBones())
					if (_stricmp(b->m_name.c_str(), "Bip01 Spine2") == 0)
						toSpine = static_cast<hdt::SkyrimBone*>(b.get())->m_node->world.Invert();
				for (auto& b : gItem.system->getBones()) {
					if (b->m_rig.isStaticOrKinematicObject()) continue;
					auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
					track[bone->m_name.c_str()].push_back(toActor * bone->m_node->world.translate);
					if (strstr(bone->m_name.c_str(), "Cape")) track[std::string("~spine ") + bone->m_name.c_str()].push_back(toSpine * bone->m_node->world.translate);
					track[std::string("~chest ") + bone->m_name.c_str()].push_back(toSpine * bone->m_node->world.translate);
				}
				if (++measured == 480) {
					std::vector<std::pair<double, std::string>> perBone;
					double total = 0;
					int count = 0;
					for (auto& [name, pts] : track) {
						if (name[0] == '~') continue;
						double sum = 0;
						for (size_t i = 2; i < pts.size(); i++) sum += (pts[i] - pts[i - 1] * 2.0f + pts[i - 2]).Length();
						double avg = sum / std::max<size_t>(1, pts.size() - 2);
						perBone.push_back({ avg, name });
						total += avg;
						count++;
					}
					std::sort(perBone.rbegin(), perBone.rend());
					FILE* f = fopen(jitterOut.c_str(), "w");
					if (f) {
						fprintf(f, "shake: average %.4f units per frame over %d bones\n", count ? total / count : 0.0, count);
						fprintf(f, "  physics %.3f ms per frame (%.3f ms on the frame's thread)\n", hdt::SkyrimPhysicsWorld::get()->m_averageStepMs, hdt::SkyrimPhysicsWorld::get()->m_averageMainMs);
#ifdef NVSMP_PROFILE
						// A profiling build: where the physics' time went over the measured frames.
						for (int s = 0; s < nvsmp::prof::Count; s++) fprintf(f, "  prof %-24s %8.1f ms total\n", nvsmp::prof::kNames[s], nvsmp::prof::gTotal[s]);
#endif
						for (size_t i = 0; i < perBone.size() && i < 6; i++) fprintf(f, "  %-18s %.4f\n", perBone[i].second.c_str(), perBone[i].first);
						// Sway: how far the cloth bones wander from their mean place (in the actor's frame), on average.
						{
							double sway = 0; int n = 0;
							for (auto& [name, pts] : track) {
								if (name.rfind("~spine ", 0) != 0 || pts.empty()) continue;
								RE::NiPoint3 m(0, 0, 0);
								for (auto& q : pts) m = m + q;
								m = m * (1.0f / pts.size());
								double s = 0;
								for (auto& q : pts) s += (q - m).Length();
								sway += s / pts.size(); n++;
							}
							fprintf(f, "  cape sway: %.3f units from the mean place on average\n", n ? sway / n : 0.0);
						}
						// Movement against the chest (Spine2), every simulated bone: on average and at most from its mean place.
						for (auto& [name, pts] : track) {
							if (name.rfind("~chest ", 0) != 0 || pts.empty()) continue;
							RE::NiPoint3 m(0, 0, 0);
							for (auto& q : pts) m = m + q;
							m = m * (1.0f / pts.size());
							double s = 0, most = 0;
							for (auto& q : pts) { double d = (q - m).Length(); s += d; most = std::max(most, d); }
							fprintf(f, "  vs chest %-18s %.3f on average, %.3f at most\n", name.c_str() + 7, s / pts.size(), most);
						}
						// Per chain (the bones' names without the last word: "SMP Cape0 C06"): shake and sway.
						{
							std::map<std::string, std::pair<double, int>> sh;
							for (auto& [score, name] : perBone) {
								size_t sp = name.rfind(' ');
								if (name.rfind("SMP ", 0) != 0 || sp == std::string::npos) continue;
								auto& e = sh[name.substr(0, sp)];
								e.first += score; e.second++;
							}
							std::map<std::string, std::pair<double, int>> sw;
							for (auto& [name, pts] : track) {
								if (name.rfind("~spine ", 0) != 0 || pts.empty()) continue;
								std::string bone = name.substr(7);
								size_t sp = bone.rfind(' ');
								if (sp == std::string::npos) continue;
								RE::NiPoint3 m(0, 0, 0);
								for (auto& q : pts) m = m + q;
								m = m * (1.0f / pts.size());
								double s = 0;
								for (auto& q : pts) s += (q - m).Length();
								auto& e = sw[bone.substr(0, sp)];
								e.first += s / pts.size(); e.second++;
							}
							for (auto& [chain, e] : sh) {
								auto it = sw.find(chain);
								if (it != sw.end()) fprintf(f, "  chain %-16s shake %.4f  sway %.3f\n", chain.c_str(), e.first / e.second, it->second.first / it->second.second);
								else fprintf(f, "  chain %-16s shake %.4f\n", chain.c_str(), e.first / e.second);
							}
						// The plugin's jitter measure (SkyrimBone::m_jitter, drawn; m_rawJitter, before the jitter filter),
						// per chain and the worst chain.
						{
							std::map<std::string, std::array<double, 4>> pj;   // drawn, raw, bones, lag
							for (auto& b : gItem.system->getBones()) {
								auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
								if (bone->m_rig.isStaticOrKinematicObject() || bone->m_jitterSamples < 3) continue;
								std::string name = bone->m_name.c_str();
								size_t sp = name.rfind(' ');
								auto& e = pj[sp == std::string::npos ? name : name.substr(0, sp)];
								e[0] += bone->m_jitter; e[1] += bone->m_rawJitter; e[2] += 1; e[3] += bone->m_filterLag;
							}
							double worst = 0, all = 0, allRaw = 0, lag = 0; std::string worstName;
							for (auto& [chain, e] : pj) {
								double d = e[0] / e[2], r = e[1] / e[2];
								all += d; allRaw += r; lag += e[3] / e[2];
								if (d > worst) { worst = d; worstName = chain; }
								fprintf(f, "  jitter %-16s %.4f  (before the filter %.4f)\n", chain.c_str(), d, r);
							}
							if (!pj.empty()) fprintf(f, "  jitter average %.4f (before the filter %.4f), worst %.4f (%s), filter lag %.3f units\n", all / pj.size(), allRaw / pj.size(), worst, worstName.c_str(), lag / pj.size());
						}
						}
						// Rope ends (the rigid pieces): how close each pair comes, to see whether they knock or pass through.
						std::vector<std::string> ends;
						for (auto& [name, pts] : track) if (name.find("Rope") != std::string::npos && !track.count(name.substr(0, name.size() - 1) + char(name.back() + 1)) && name.back() != '0') ends.push_back(name);
						for (size_t i = 0; i < ends.size(); i++) for (size_t j = i + 1; j < ends.size(); j++) {
							auto& a = track[ends[i]]; auto& b = track[ends[j]];
							std::vector<float> d;
							for (size_t k = 0; k < a.size() && k < b.size(); k++) d.push_back((a[k] - b[k]).Length());
							std::sort(d.begin(), d.end());
							if (!d.empty()) fprintf(f, "  %s - %s: start %.2f, min %.2f, 5%% %.2f, median %.2f\n", ends[i].c_str(), ends[j].c_str(),
								(a[0] - b[0]).Length(), d.front(), d[d.size() / 20], d[d.size() / 2]);
						}
						fclose(f);
					}
					done = true;
				}
			}
		}
		UpdateCamera(dt);
		Panels(fps);
		ImGui::EndFrame();

		RECT rc;
		GetClientRect(gWnd, &rc);
		float aspect = (float)std::max(1L, rc.right) / (float)std::max(1L, rc.bottom);
		M4 view = LookAtRH(CameraEye(), gCam.target + gCam.pan, V3(0, 0, 1));
		M4 proj = PerspectiveRH(0.75f, aspect, 2.0f, 20000.0f);
		M4 viewProj = Mul(view, proj);

		// The Make SMP brush: the ray under the mouse (the camera's frame, as LookAtRH builds it; fov 0.75 rad).
		{
			ImGuiIO& io = ImGui::GetIO();
			V3 eye = CameraEye(), at = gCam.target + gCam.pan;
			V3 z = Norm(eye - at), x = Norm(Cross(V3(0, 0, 1), z)), y = Cross(z, x);
			float th = std::tan(0.75f / 2);
			float mx = io.MousePos.x / std::max(1L, rc.right), my = io.MousePos.y / std::max(1L, rc.bottom);
			int forced = -1;
			if (weightsTest && frameNo == 130) maker::WeightsTest();
			if (stroke[0] >= 0 && frameNo >= 60 && frameNo <= 121) {
				float t = std::min(1.0f, (frameNo - 60) / 60.0f);
				mx = stroke[0] + (stroke[2] - stroke[0]) * t; my = stroke[1] + (stroke[3] - stroke[1]) * t;
				forced = frameNo <= 120 ? 1 : 0;
				if (frameNo == 121 && buildAfterStroke) maker::gM.buildOnOpen = true;
			}
			float nx = mx * 2 - 1, ny = 1 - my * 2;
			maker::Mouse(eye, Norm(z * -1.0f + x * (nx * th * aspect) + y * (ny * th)), forced);
		}

		// A click on a bone's dot (Bones shown) picks it for the weights heat map.
		{
			ImGuiIO& io = ImGui::GetIO();
			if (gItem.system && gS.showBones && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.WantCaptureMouse && !io.KeyCtrl) {
				float best = 12.0f * 12.0f;
				std::string picked;
				for (auto& b : gItem.system->getBones()) {
					auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
					RE::NiPoint3 p = bone->m_node->world.translate;
					float c[4];
					for (int j = 0; j < 4; j++) c[j] = p.x * viewProj.m[0][j] + p.y * viewProj.m[1][j] + p.z * viewProj.m[2][j] + viewProj.m[3][j];
					if (c[3] <= 0) continue;
					float sx = (c[0] / c[3] * 0.5f + 0.5f) * rc.right, sy = (0.5f - c[1] / c[3] * 0.5f) * rc.bottom;
					float d = (sx - io.MousePos.x) * (sx - io.MousePos.x) + (sy - io.MousePos.y) * (sy - io.MousePos.y);
					if (d < best) { best = d; picked = bone->m_name.c_str(); }
				}
				if (!picked.empty()) { gS.heatBone = picked; AddLog(2, "weights: " + picked); }
			}
		}

		D3DCOLOR bg = D3DCOLOR_RGBA((int)(gS.background[0] * 255), (int)(gS.background[1] * 255), (int)(gS.background[2] * 255), 255);
		gDev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, bg, 1.0f, 0);
		if (SUCCEEDED(gDev->BeginScene())) {
			if (gItem.loaded) DrawScene(viewProj, view, proj);
			ImGui::Render();
			if (!noUi && !gShotPending) ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			gDev->EndScene();
		}
		frameNo++;
		if (gShotPending) {
			gShotPending = false;
			char name[64];
			SYSTEMTIME st;
			GetLocalTime(&st);
			snprintf(name, sizeof name, "NVSMPPreviewer %04d-%02d-%02d %02d%02d%02d.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			fs::path dir = fs::path(gIniPath).parent_path() / "screenshots";
			fs::create_directories(dir);
			std::string p = (dir / name).string();
			if (SaveScreenshot(p)) AddLog(2, "screenshot: " + p);
		}
		if (!shots.empty() && shotsTaken < shotCount && frameNo % shotEvery == 0) {
			char p[1024];
			snprintf(p, sizeof p, "%s%02d.bmp", shots.c_str(), shotsTaken++);
			SaveScreenshot(p);
			if (shotsTaken == shotCount && exitAfter) {
				done = true;
				if (!logOut.empty())
					if (FILE* lf = fopen(logOut.c_str(), "w")) {
						for (auto& e : gLog) fprintf(lf, "%s\n", e.text.c_str());
						fclose(lf);
					}
			}
		}
		HRESULT hr = gDev->Present(nullptr, nullptr, nullptr, nullptr);
		if (hr == D3DERR_DEVICELOST && gDev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) ResetDevice();
	}

	if (maker::gM.worker.joinable()) maker::gM.worker.join();   // a build still running finishes first
	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	return 0;
}
