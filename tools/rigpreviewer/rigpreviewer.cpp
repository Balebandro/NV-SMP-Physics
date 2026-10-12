// NV Rig Previewer: a skinned NIF (armor, clothing, a body) on the game's skeleton, outside the game, to
// test its rigging and bone weights. The mesh is laid out in memory as New Vegas holds it and skinned as
// the game skins it (tools/scene.cpp, from the skin partitions the game draws with), so a pose shows what
// the game would show.
//
//   NVRigPreviewer.exe [item.nif]
//
// What it shows: the weights of a bone as a heat map, each vertex's main bone, how many bones move each
// vertex, and the problems (unweighted vertices, weights not adding up to 1, more than 4 bones, weights
// the partitions dropped, bones the skeleton does not have). What it poses with: any .kf animation (loose
// or in the BSAs), stress poses that aim the limbs whatever the bind pose, a rotation for any bone, and a
// flex test that swings one bone back and forth. Click a vertex to see every bone that moves it.
//
// Files are found as the SMP previewer finds them (tools/previewer/datafiles.cpp): the NIF's own Data
// folder, Mod Organizer 2's enabled mods by priority, the game's Data folder, then the BSA archives.
#include "datafiles.h"
#include "kf.h"
#include "scene.h"

#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

#include <commdlg.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <filesystem>
#include <functional>
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
static bool gLogScroll = false;
static bool gShotPending = false;

static void AddLog(int level, const std::string& text)
{
	gLog.push_back({ level, text });
	while (gLog.size() > 2000) gLog.pop_front();
	gLogScroll = true;
	OutputDebugStringA((text + "\n").c_str());
}

// The physics core is linked (tools/scene.cpp builds on its types) but never run; its log goes here.
namespace nvsmp
{
	bool LogEnabled(LogLevel level) { return level >= LogLevel::Warn; }
	void LogLine(LogLevel level, const std::string& text) { AddLog((int)level, text); }
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
	RE::NiPoint3 Ni() const { return { x, y, z }; }
};
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float Len(V3 a) { return std::sqrt(Dot(a, a)); }
static V3 Norm(V3 a) { float l = Len(a); return l > 1e-8f ? a * (1.0f / l) : V3(0, 0, 1); }

// Row-vector matrices (v' = v * M), as the shaders (row_major) use.
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

// A rotation of angle (radians) about a unit axis, as the game's matrices (v' = M * v).
static RE::NiMatrix3 AxisAngle(V3 a, float angle)
{
	a = Norm(a);
	float c = std::cos(angle), s = std::sin(angle), t = 1 - c;
	RE::NiMatrix3 m;
	m.entry[0][0] = t * a.x * a.x + c;       m.entry[0][1] = t * a.x * a.y - s * a.z; m.entry[0][2] = t * a.x * a.z + s * a.y;
	m.entry[1][0] = t * a.x * a.y + s * a.z; m.entry[1][1] = t * a.y * a.y + c;       m.entry[1][2] = t * a.y * a.z - s * a.x;
	m.entry[2][0] = t * a.x * a.z - s * a.y; m.entry[2][1] = t * a.y * a.z + s * a.x; m.entry[2][2] = t * a.z * a.z + c;
	return m;
}

// The shortest rotation taking direction a to direction b, weighted (0 none .. 1 all the way).
static RE::NiMatrix3 RotationBetween(V3 a, V3 b, float amount)
{
	a = Norm(a);
	b = Norm(b);
	V3 axis = Cross(a, b);
	float s = Len(axis), c = std::clamp(Dot(a, b), -1.0f, 1.0f);
	if (s < 1e-5f) {
		if (c > 0) return RE::NiMatrix3();
		// Opposite: any axis across a.
		axis = std::fabs(a.z) < 0.9f ? Cross(a, V3(0, 0, 1)) : Cross(a, V3(1, 0, 0));
	}
	return AxisAngle(axis, std::atan2(s, c) * amount);
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
	D3DCOLOR color;
};
struct LineVertex
{
	float x, y, z;
	D3DCOLOR c;
};

static const char* kShader = R"(
row_major float4x4 gViewProj : register(c0);
struct VSIn { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct VSOut { float4 pos : POSITION; float3 nrm : TEXCOORD1; float2 uv : TEXCOORD0; float4 col : COLOR0; };
VSOut vs(VSIn i) { VSOut o; o.pos = mul(float4(i.pos, 1), gViewProj); o.nrm = i.nrm; o.uv = i.uv; o.col = i.col; return o; }

float4 gKey : register(c0);      // key light direction (toward the light)
float4 gColor : register(c1);    // tint
float4 gParams : register(c2);   // x: textured, y: alpha test, z: unlit, w: vertex colours (the weight views)
float4 gFill : register(c3);     // fill light direction
sampler2D gTex : register(s0);
float4 ps(VSOut i) : COLOR
{
	float4 base = gParams.x > 0.5 ? tex2D(gTex, i.uv) : float4(1, 1, 1, 1);
	if (gParams.y > 0.5) clip(base.a - 0.5);
	base *= gColor;
	if (gParams.w > 0.5) base = float4(i.col.rgb, 1);
	if (gParams.z > 0.5) return base;
	float3 n = normalize(i.nrm);
	float key = abs(dot(n, gKey.xyz)), fill = abs(dot(n, gFill.xyz));
	// The weight views stay readable: lit more evenly.
	float light = gParams.w > 0.5 ? 0.55 + 0.4 * key + 0.15 * fill : 0.28 + 0.62 * key + 0.22 * fill;
	return float4(base.rgb * light, 1);
}
)";

static bool CompileShaders()
{
	ID3DBlob *code = nullptr, *err = nullptr;
	if (FAILED(D3DCompile(kShader, strlen(kShader), "rig", nullptr, nullptr, "vs", "vs_3_0", 0, 0, &code, &err))) {
		AddLog(4, std::string("vertex shader: ") + (err ? (const char*)err->GetBufferPointer() : "?"));
		return false;
	}
	gDev->CreateVertexShader((DWORD*)code->GetBufferPointer(), &gVS);
	code->Release();
	if (FAILED(D3DCompile(kShader, strlen(kShader), "rig", nullptr, nullptr, "ps", "ps_3_0", 0, 0, &code, &err))) {
		AddLog(4, std::string("pixel shader: ") + (err ? (const char*)err->GetBufferPointer() : "?"));
		return false;
	}
	gDev->CreatePixelShader((DWORD*)code->GetBufferPointer(), &gPS);
	code->Release();
	D3DVERTEXELEMENT9 el[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 },
		{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
		{ 0, 32, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
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
				ih.biHeight = -(LONG)d.Height;
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

// --- settings (NVRigPreviewer.ini next to the exe) --------------------------------------------------------

static std::string gIniPath;

static std::string IniGet(const char* key, const char* def = "")
{
	char buf[1024];
	GetPrivateProfileStringA("RigPreviewer", key, def, buf, sizeof buf, gIniPath.c_str());
	return buf;
}
static void IniSet(const char* key, const std::string& value) { WritePrivateProfileStringA("RigPreviewer", key, value.c_str(), gIniPath.c_str()); }

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

// --- the loaded mesh ------------------------------------------------------------------------------------

// What can be wrong with a vertex's weights.
enum Problem : uint8_t
{
	kUnweighted = 1,     // no bone moves it: it stays at the bind pose while the body moves away
	kSumOff = 2,         // its weights do not add up to 1: it shrinks toward, or flies past, its bones
	kTooMany = 4,        // more than 4 bones in the skin data: the game keeps 4, the rest is lost
	kDropped = 8,        // the partitions the game draws with hold other weights than the skin data
	kUnknownBone = 16,   // weighted to a bone the skeleton does not have: it follows the skeleton's root
	kBadPartition = 32,  // its partition is broken (missing or out-of-range data) or another partition draws it with other weights
};
static const char* kProblemNames[] = { "Unweighted", "Weights do not add up to 1", "More than 4 bones", "Weights lost in the partitions", "Bone not in the skeleton", "Broken partition" };
static const D3DCOLOR kProblemColors[] = { D3DCOLOR_ARGB(255, 255, 40, 220), D3DCOLOR_ARGB(255, 255, 60, 50), D3DCOLOR_ARGB(255, 255, 150, 30), D3DCOLOR_ARGB(255, 240, 230, 40), D3DCOLOR_ARGB(255, 60, 220, 255), D3DCOLOR_ARGB(255, 150, 90, 255) };

using Influences = std::vector<std::pair<int, float>>;   // skin slot, weight

struct ShapeRig
{
	scene::ShapeInfo info;
	std::string name;
	IDirect3DTexture9* texture = nullptr;
	std::string texturePath, textureNote;
	bool visible = true;
	std::vector<Influences> weights;       // what the views and the skinning use: partWeights or dataWeights (gWeightsFromData)
	std::vector<Influences> partWeights;   // the partitions' (what the game skins with)
	std::vector<Influences> dataWeights;   // the skin data's (what the exporter wrote)
	std::vector<std::string> slotNames;    // per skin slot, the bone name the NIF gives
	std::vector<bool> slotKnown;           // that name is in the skeleton
	std::vector<uint8_t> problems;         // per vertex
	std::vector<uint8_t> badPartition;     // per vertex, set by ReadSkinRecords
	std::vector<std::string> partitionNotes;
	int problemCounts[6] = {};
	int partitions = 0, maxDataInfluences = 0, maxPartInfluences = 0;
	std::vector<MeshVertex> verts;
	std::vector<uint16_t> indices;
};

struct Item
{
	bool loaded = false;
	std::string nifPath;
	fs::file_time_type nifTime;
	datafiles::Roots roots;
	std::string skeletonPath;
	scene::Item scene;
	std::vector<ShapeRig> shapes;
	std::map<std::string, RE::NiTransform> rest;   // every skeleton node's rest local
	std::set<std::string> usedBones;               // skeleton bones some shape is weighted to
	std::set<std::string> unknownBones;            // bone names the skins give that the skeleton lacks
	std::set<std::string> itemBones;               // an SMP item's own bones, built from "SMP Bones" (BuildItemBones)
	// The body to check the fit against: drawn grey, posed as the item.
	scene::Item reference;
	std::vector<ShapeRig> referenceShapes;
	std::string referencePath;
} gItem;

static std::unordered_map<std::string, IDirect3DTexture9*> gTextureCache;

static fs::file_time_type TimeOf(const std::string& p)
{
	std::error_code ec;
	auto t = fs::last_write_time(p, ec);
	return ec ? fs::file_time_type{} : t;
}

static RE::NiNode* Bone(const char* name)
{
	auto it = gItem.scene.skeleton.bones.find(name);
	return it == gItem.scene.skeleton.bones.end() ? nullptr : it->second;
}

static std::string Lower(std::string s)
{
	for (auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

// The NIF's own record of each shape's skin: the bone names per slot and the skin data's weights (the
// partitions scene.cpp loads are what the game draws; the skin data is what the exporter wrote, and
// where the two differ the partitioning lost something). Shapes in file order, as scene::Load makes them.
static void ReadSkinRecords(const std::string& path, std::vector<ShapeRig>& shapes)
{
	nif::File f;
	f.load(path);
	auto strName = [&](int block) {
		nif::Reader r(f.blocks[block].data(), f.blocks[block].size());
		uint32_t s = r.u32();
		return s < f.strings.size() ? f.strings[s] : std::string();
	};
	size_t index = 0;
	for (size_t blk = 0; blk < f.blocks.size() && index < shapes.size(); blk++) {
		const std::string& t = f.typeOf(blk);
		if (t != "NiTriShape" && t != "NiTriStrips") continue;
		ShapeRig& s = shapes[index++];
		nif::Geometry g;
		g.read(f.blocks[blk]);
		if (g.skin < 0) continue;
		nif::SkinInstance si;
		si.read(f.blocks[g.skin], f.typeOf(g.skin) == "BSDismemberSkinInstance");
		nif::SkinData sd;
		sd.read(f.blocks[si.data]);
		for (int b : si.bones) s.slotNames.push_back(strName(b));
		s.dataWeights.assign(s.weights.size(), {});
		for (size_t b = 0; b < sd.bones.size(); b++)
			for (auto& vw : sd.bones[b].weights)
				if (vw.index < s.dataWeights.size() && vw.weight > 0) s.dataWeights[vw.index].push_back({ (int)b, vw.weight });
		nif::SkinPartition sp;
		sp.read(f.blocks[si.partition]);
		s.partitions = (int)sp.parts.size();

		// The partitions as written, before scene.cpp fills in what is missing: what an exporter left out or
		// got wrong, and vertices two partitions draw with different weights (the game skins each
		// partition's triangles with that partition's copy; the views show the first).
		const size_t nv = s.weights.size();
		s.badPartition.assign(nv, 0);
		s.partitionNotes.clear();
		std::vector<Influences> first(nv);
		std::vector<int> firstPart(nv, -1);
		for (size_t i = 0; i < sp.parts.size(); i++) {
			const auto& p = sp.parts[i];
			const size_t per = std::max<uint16_t>(1, p.numWeightsPerVertex);
			std::vector<std::string> notes;
			if (!p.hasVertexMap) notes.push_back("no vertex map");
			if (!p.hasVertexWeights) notes.push_back("no weights");
			if (!p.hasBoneIndices) notes.push_back("no bone indices");
			if (p.numWeightsPerVertex > 4) notes.push_back(std::to_string(p.numWeightsPerVertex) + " weights per vertex (the game reads 4)");
			int badBones = 0, badMap = 0, badIndex = 0, differing = 0;
			for (auto b : p.bones) if (b >= si.bones.size()) badBones++;
			for (uint16_t v = 0; v < p.numVertices; v++) {
				size_t vert = p.hasVertexMap ? p.vertexMap[v] : v;
				if (vert >= nv) { badMap++; continue; }
				bool bad = badBones > 0 || !p.hasVertexWeights || !p.hasBoneIndices;
				Influences inf;
				for (size_t k = 0; k < per && p.hasVertexWeights && p.hasBoneIndices; k++) {
					float w = p.weights[v * per + k];
					uint8_t idx = p.boneIndices[v * per + k];
					if (idx >= p.bones.size()) { if (w > 0) { bad = true; badIndex++; } continue; }
					if (w > 0) inf.push_back({ (int)p.bones[idx], w });
				}
				std::sort(inf.begin(), inf.end());
				if (firstPart[vert] < 0) { firstPart[vert] = (int)i; first[vert] = inf; }
				else {
					bool same = inf.size() == first[vert].size();
					for (size_t k = 0; same && k < inf.size(); k++)
						same = inf[k].first == first[vert][k].first && std::fabs(inf[k].second - first[vert][k].second) < 0.02f;
					if (!same) { bad = true; differing++; }
				}
				if (bad) s.badPartition[vert] = 1;
			}
			if (badBones) notes.push_back(std::to_string(badBones) + " bones past the skin's " + std::to_string(si.bones.size()));
			if (badMap) notes.push_back(std::to_string(badMap) + " vertex map entries past the shape's vertices");
			if (badIndex) notes.push_back(std::to_string(badIndex) + " weights on bone indices past the partition's " + std::to_string(p.bones.size()) + " bones");
			if (differing) notes.push_back(std::to_string(differing) + " vertices weighted differently than in an earlier partition");
			if (notes.empty()) continue;
			std::string line = "partition " + std::to_string(i) + ": ";
			for (size_t n = 0; n < notes.size(); n++) line += (n ? ", " : "") + notes[n];
			s.partitionNotes.push_back(line);
		}
	}
}

// An SMP item's own bones ("SMP Bones" on its shapes: "name|parent;..." in skin-slot order). New Vegas
// drops item bones on attach and the SMP plugin builds them back (src/nv/hdtSkyrimSystem.cpp,
// createExtraBoneNode); built the same way here, without the physics: each at its bind pose against the
// body, from a bone of the same skin the skeleton does have, carried rigidly on its parent. Without them
// their vertices would hang on the skeleton's root and not follow the body.
static void AddChildNode(RE::NiNode* parent, RE::NiAVObject* child)
{
	auto** arr = (RE::NiAVObject**)realloc(parent->children, sizeof(void*) * (parent->childFirstFree + 1));
	parent->children = arr;
	arr[parent->childFirstFree++] = child;
	parent->childCount = parent->childFirstFree;
	child->parent = parent;
}

static void BuildItemBones()
{
	struct Use { RE::NiGeometry* g; uint32_t slot; std::string name, parent; };
	std::map<std::string, std::vector<Use>> uses;    // lower-case name -> its slots
	std::map<RE::NiGeometry*, std::vector<std::string>> slotNames;
	for (auto& info : gItem.scene.shapes) {
		auto* g = info.geometry;
		const char* list = g->FindStringExtra("SMP Bones");
		if (!list || !g->skinInstance) continue;
		std::stringstream ss(list);
		std::string entry;
		uint32_t slot = 0;
		while (std::getline(ss, entry, ';') && slot < g->skinInstance->boneCount) {
			auto bar = entry.find('|');
			std::string name = bar == std::string::npos ? entry : entry.substr(0, bar);
			std::string parent = bar == std::string::npos ? std::string() : entry.substr(bar + 1);
			slotNames[g].push_back(name);
			if (!name.empty()) uses[Lower(name)].push_back({ g, slot, name, parent });
			slot++;
		}
	}
	auto& bones = gItem.scene.skeleton.bones;
	std::function<RE::NiNode*(const std::string&, int)> make = [&](const std::string& name, int depth) -> RE::NiNode* {
		if (auto it = bones.find(name); it != bones.end()) return it->second;
		auto u = uses.find(Lower(name));
		if (u == uses.end() || u->second.empty() || depth > 64) return nullptr;
		const Use& use = u->second.front();
		auto* skin = use.g->skinInstance;
		auto* data = skin->skinData;
		RE::NiNode* parent = use.parent.empty() ? nullptr : make(use.parent, depth + 1);
		// A slot of the same skin bound to a node the skeleton (or this builder) has.
		int reference = -1;
		auto& names = slotNames[use.g];
		for (uint32_t s = 0; s < skin->boneCount && reference < 0; s++)
			if (s != use.slot && parent && skin->bones[s] == parent) reference = (int)s;
		for (uint32_t s = 0; s < skin->boneCount && reference < 0; s++)
			if (s != use.slot && s < names.size() && bones.count(names[s])) reference = (int)s;
		if (reference < 0) return nullptr;
		RE::NiAVObject* refNode = bones.count(names[reference]) ? (RE::NiAVObject*)bones[names[reference]] : skin->bones[reference];
		RE::NiTransform skinToWorld = refNode->world * data->boneData[reference].skinToBone;
		RE::NiTransform nodeWorld = skinToWorld * data->boneData[use.slot].skinToBone.Invert();
		RE::NiNode* anchor = parent ? parent : static_cast<RE::NiNode*>(refNode->IsNode());
		if (!anchor) return nullptr;
		RE::NiNode* node = hdt::createNode(use.name.c_str());
		node->local = anchor->world.Invert() * nodeWorld;
		node->world = nodeWorld;
		AddChildNode(anchor, node);
		bones[use.name] = node;
		gItem.itemBones.insert(use.name);
		for (auto& other : u->second)
			if (other.g->skinInstance && other.slot < other.g->skinInstance->boneCount) other.g->skinInstance->bones[other.slot] = node;
		return node;
	};
	for (auto& [key, list] : uses) make(list.front().name, 0);
	if (!gItem.itemBones.empty()) AddLog(2, std::to_string(gItem.itemBones.size()) + " of the item's own bones built from \"SMP Bones\" (carried rigidly: no physics here)");
}

static void Analyse(ShapeRig& s)
{
	const auto& sk = gItem.scene.skeleton.bones;
	s.slotKnown.clear();
	for (auto& n : s.slotNames) {
		bool known = sk.count(n) > 0;
		s.slotKnown.push_back(known);
		if (known) gItem.usedBones.insert(n);
		else gItem.unknownBones.insert(n);
	}
	int n = (int)s.partWeights.size();
	s.problems.assign(n, 0);
	for (auto& c : s.problemCounts) c = 0;
	bool skinned = s.info.geometry->skinInstance != nullptr;
	if (!skinned) return;
	bool haveData = (int)s.dataWeights.size() == n;
	for (int v = 0; v < n; v++) {
		uint8_t p = 0;
		float sum = 0;
		for (auto& [slot, w] : s.partWeights[v]) {
			sum += w;
			if (slot < (int)s.slotKnown.size() && !s.slotKnown[slot] && w > 0.01f) p |= kUnknownBone;
		}
		if (s.partWeights[v].empty() || sum < 1e-4f) p |= kUnweighted;
		else if (std::fabs(sum - 1.0f) > 0.01f) p |= kSumOff;
		if (v < (int)s.badPartition.size() && s.badPartition[v]) p |= kBadPartition;
		s.maxPartInfluences = std::max(s.maxPartInfluences, (int)s.partWeights[v].size());
		if (haveData) {
			const auto& d = s.dataWeights[v];
			s.maxDataInfluences = std::max(s.maxDataInfluences, (int)d.size());
			if (d.size() > 4) p |= kTooMany;
			// Every skin data weight above a trace should be in the partitions at about its value (they
			// renormalise what they keep, so a small difference is expected where something was dropped).
			for (auto& [slot, w] : d) {
				if (w < 0.02f) continue;
				float inPart = 0;
				for (auto& [ps, pw] : s.partWeights[v]) if (ps == slot) inPart = pw;
				if (std::fabs(inPart - w) > 0.05f) { p |= kDropped; break; }
			}
		}
		s.problems[v] = p;
		for (int b = 0; b < 6; b++) if (p & (1 << b)) s.problemCounts[b]++;
	}
}

// Which weights the views and the skinning use: the partitions' (what the game draws) or the skin data's (what
// was exported). Where the two differ, switching shows whether the export or the partitioning went wrong.
static bool gWeightsFromData = false;

static void ApplyWeightSource()
{
	for (auto& s : gItem.shapes) {
		bool data = gWeightsFromData && s.dataWeights.size() == s.partWeights.size();
		s.weights = data ? s.dataWeights : s.partWeights;
	}
}

static void PrepareShape(ShapeRig& d, scene::ShapeInfo& info, bool texture)
{
	d.info = info;
	d.name = info.geometry->m_name ? info.geometry->m_name : "";
	d.partWeights = d.weights = scene::WeightsOf(info.geometry);
	auto* td = (RE::NiTriShapeData*)info.geometry->data;
	d.indices.assign(td->triList, td->triList + td->triListLength);
	d.verts.resize(td->vertexCount);
	if (texture && !info.diffuse.empty()) {
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
	std::string n = Lower(d.name);
	if (n.find("cap") != std::string::npos || n.find("meathead") != std::string::npos) d.visible = false;
}

// --- posing -----------------------------------------------------------------------------------------------
//
// Each frame the skeleton starts at rest, the animation (if any) sets its tracks, then, walking down the
// hierarchy, each bone gets the stress pose's aims and turns, the bone rotations set by hand and the flex
// test, all in world space (so "up" is up whatever the bone's own axes), before its children are placed.

static const char* kAxisNames[] = { "Side (X)", "Front (Y)", "Up (Z)" };
static const V3 kAxes[] = { V3(1, 0, 0), V3(0, 1, 0), V3(0, 0, 1) };

// Directions are in the body's own terms, (left, front, up), turned into world directions with the body
// frame read off the skeleton at load (which way its left clavicle and its toes lie), so the poses do not
// depend on how the skeleton is laid out. They name the left side; a right-side bone mirrors "left".
struct Aim
{
	const char* bone;
	const char* toward;  // the bone's direction: toward this descendant (its child, or further down)
	V3 target;           // (left, front, up)
};
struct Turn
{
	const char* bone;
	V3 from, to;         // turned so "from" moves toward "to", (left, front, up)
	float degrees;
};
struct StressPose
{
	const char* name;
	std::vector<Aim> aims;
	std::vector<Turn> turns;
};

static V3 gLeft(1, 0, 0), gFront(0, 1, 0);   // the body frame (BodyFrame)
static const V3 U(0, 0, 1), F(0, 1, 0), L(1, 0, 0);

// The body's heading as it walks and turns (the actor's rotation, set by the motion each frame).
static RE::NiMatrix3 gHeading;

static V3 ActorAxis(V3 a) { return V3(gHeading * a.Ni()); }

static V3 BodyToWorld(V3 b, bool mirrored)
{
	return ActorAxis(gLeft * (mirrored ? -b.x : b.x) + gFront * b.y + V3(0, 0, 1) * b.z);
}

static const std::vector<StressPose> kStressPoses = {
	{ "Rest (bind)", {}, {} },
	{ "Arms down", { { "Bip01 L UpperArm", "Bip01 L Forearm", V3(0.15f, 0, -1) }, { "Bip01 R UpperArm", "Bip01 R Forearm", V3(0.15f, 0, -1) } }, {} },
	{ "Arms forward", { { "Bip01 L UpperArm", "Bip01 L Forearm", V3(0.1f, 1, 0) }, { "Bip01 R UpperArm", "Bip01 R Forearm", V3(0.1f, 1, 0) } }, {} },
	{ "Arms up", { { "Bip01 L UpperArm", "Bip01 L Forearm", V3(0.15f, 0, 1) }, { "Bip01 R UpperArm", "Bip01 R Forearm", V3(0.15f, 0, 1) } }, {} },
	{ "Arms crossed", { { "Bip01 L UpperArm", "Bip01 L Forearm", V3(0.1f, 0.5f, -1) }, { "Bip01 R UpperArm", "Bip01 R Forearm", V3(0.1f, 0.5f, -1) },
		{ "Bip01 L Forearm", "Bip01 L Hand", V3(-1, 0.4f, 0.1f) }, { "Bip01 R Forearm", "Bip01 R Hand", V3(-1, 0.4f, 0.1f) } }, {} },
	{ "Elbows bent", { { "Bip01 L UpperArm", "Bip01 L Forearm", V3(0.15f, 0, -1) }, { "Bip01 R UpperArm", "Bip01 R Forearm", V3(0.15f, 0, -1) },
		{ "Bip01 L Forearm", "Bip01 L Hand", V3(0, 1, 0) }, { "Bip01 R Forearm", "Bip01 R Hand", V3(0, 1, 0) } }, {} },
	{ "Aiming a rifle", { { "Bip01 L UpperArm", "Bip01 L Forearm", V3(-0.3f, 0.8f, -0.5f) }, { "Bip01 R UpperArm", "Bip01 R Forearm", V3(0.6f, 0.2f, -0.8f) },
		{ "Bip01 L Forearm", "Bip01 L Hand", V3(-0.2f, 1, 0.1f) }, { "Bip01 R Forearm", "Bip01 R Hand", V3(-0.9f, 0.5f, 0.2f) } }, {} },
	{ "Squat", { { "Bip01 L Thigh", "Bip01 L Calf", V3(0.25f, 1, -0.15f) }, { "Bip01 R Thigh", "Bip01 R Calf", V3(0.25f, 1, -0.15f) },
		{ "Bip01 L Calf", "Bip01 L Foot", V3(0.05f, -0.35f, -1) }, { "Bip01 R Calf", "Bip01 R Foot", V3(0.05f, -0.35f, -1) } }, {} },
	{ "Kneel (left knee down)", { { "Bip01 L Thigh", "Bip01 L Calf", V3(0.1f, 0.2f, -1) }, { "Bip01 L Calf", "Bip01 L Foot", V3(0, -1, 0) },
		{ "Bip01 R Thigh", "Bip01 R Calf", V3(0.15f, 1, -0.1f) }, { "Bip01 R Calf", "Bip01 R Foot", V3(0, 0, -1) } }, {} },
	{ "Stride", { { "Bip01 L Thigh", "Bip01 L Calf", V3(0.05f, 0.8f, -1) }, { "Bip01 L Calf", "Bip01 L Foot", V3(0, 0.1f, -1) },
		{ "Bip01 R Thigh", "Bip01 R Calf", V3(0.05f, -0.6f, -1) }, { "Bip01 R Calf", "Bip01 R Foot", V3(0, -0.7f, -1) } }, {} },
	{ "Legs apart", { { "Bip01 L Thigh", "Bip01 L Calf", V3(0.7f, 0, -1) }, { "Bip01 R Thigh", "Bip01 R Calf", V3(0.7f, 0, -1) } }, {} },
	{ "Bend forward", {}, { { "Bip01 Spine", U, F, 25 }, { "Bip01 Spine1", U, F, 20 }, { "Bip01 Spine2", U, F, 15 } } },
	{ "Lean back", {}, { { "Bip01 Spine", U, F, -15 }, { "Bip01 Spine1", U, F, -12 }, { "Bip01 Spine2", U, F, -10 } } },
	{ "Twist torso", {}, { { "Bip01 Spine", F, L, 15 }, { "Bip01 Spine1", F, L, 15 }, { "Bip01 Spine2", F, L, 15 } } },
	{ "Side bend", {}, { { "Bip01 Spine", U, L, 12 }, { "Bip01 Spine1", U, L, 12 }, { "Bip01 Spine2", U, L, 10 } } },
	{ "Head turn", {}, { { "Bip01 Neck1", F, L, 30 }, { "Bip01 Head", F, L, 35 } } },
	{ "Head down", {}, { { "Bip01 Neck1", U, F, 25 }, { "Bip01 Head", U, F, 20 } } },
};

// The body frame from the skeleton at rest: left toward the left clavicle (or upper arm, or thigh), front
// toward the toes.
static void BodyFrame()
{
	auto pos = [](const char* n) -> V3 { auto* b = Bone(n); return b ? V3(b->world.translate) : V3(); };
	auto has = [](const char* n) { return Bone(n) != nullptr; };
	V3 left = has("Bip01 L Clavicle") && has("Bip01 R Clavicle") ? pos("Bip01 L Clavicle") - pos("Bip01 R Clavicle")
	        : has("Bip01 L Thigh") && has("Bip01 R Thigh") ? pos("Bip01 L Thigh") - pos("Bip01 R Thigh") : V3(1, 0, 0);
	left.z = 0;
	gLeft = Norm(Len(left) > 1e-3f ? left : V3(1, 0, 0));
	V3 front = has("Bip01 L Foot") && has("Bip01 L Toe0") ? pos("Bip01 L Toe0") - pos("Bip01 L Foot") : V3(0, 1, 0);
	front = front - gLeft * Dot(front, gLeft);
	front.z = 0;
	gFront = Norm(Len(front) > 1e-3f ? front : Cross(V3(0, 0, 1), gLeft));
}

struct Pose
{
	// Stress pose.
	int stress = 0;
	float amount = 1;
	bool cycle = false;          // swing between rest and the pose
	float cycleSpeed = 0.5f;
	// By hand: degrees about the world axes, per bone.
	std::map<std::string, V3> manual;
	// Flex test: the selected bone swung about an axis.
	bool flex = false;
	int flexAxis = 0;
	float flexDegrees = 60, flexSpeed = 1;
	float clock = 0;
} gPose;

struct Selection
{
	std::string bone;            // selected bone name (skeleton or not)
	int shape = -1, vertex = -1; // picked vertex
	std::string hoverBone;
} gSel;

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

// --- motion -----------------------------------------------------------------------------------------------
//
// As the SMP previewer moves its body: with the game's skeleton and its locomotion animations found (loose
// or in the archives), the presets play them, crossfaded as the game blends, the body carried by each clip's
// own root motion so the feet stay planted. "Animation file" plays any .kf (the browser, a file, a drop).

static const char* kPresets[] = { "Stand", "Walk", "Run", "Walk in circles", "Spin in place", "Start and stop", "Jumping", "Sneak", "Sneak walk", "Drive (WASD)", "Animation file" };
enum { kStand, kWalk, kRun, kCircles, kSpin, kStartStop, kJumping, kSneak, kSneakWalk, kDrive, kCustom };

// Weapon stances: the locomotion file name prefix the game uses for each.
static const char* kStances[] = { "Unarmed", "Pistol", "Rifle", "Automatic rifle", "One-hand melee", "Two-hand melee" };
static const char* kStancePrefix[] = { "mt", "1hp", "2hr", "2ha", "1hm", "2hm" };

static const char* kActions[] = { "idle", "forward", "fastforward", "backward", "fastbackward", "left", "right", "fastleft", "fastright", "turnleft", "turnright",
	"jumpstart", "jumploop", "jumpland", "jumploopforward", "jumplandforward" };

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
	bool female = false;
	bool sneak = false;               // the sneak (crouched) set
	int stance = 0;
	std::unordered_map<std::string, kf::Clip> cache;   // by file path
	std::map<std::string, std::string> found;         // action ("forward") -> file, for the current stance
	std::set<std::string> borrowed;                   // actions the stance lacks, played from unarmed
	ClipPlay current, previous;
	float blend = 1;                  // 0 at a switch .. 1 when the new clip has taken over
	std::string customPath;
	std::string customRel;             // its Data-relative path when picked from the list
	// Settings.
	int preset = kStand;
	float speedScale = 1.0f, timeScale = 1.0f;
	bool paused = false;
	bool inPlace = false;             // the body stays at the origin (the clips still play)
} gAnim;

struct Motion
{
	V3 pos;
	float yaw = 0, speed = 0, yawRate = 0, jump = -1, time = 0;
	V3 velocity;                      // actor space
	enum { kGround, kTakeOff, kAir, kLanding } jumpStage = kGround;
	float stageTime = 0, groundTime = 0;
} gMotion;

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
// ("mt", "1hp"...): the per-gender (kNVSE) and vanilla paths in every Data folder by priority, then the
// archives, then, last, AnimGroupOverride folders (many of those are for one weapon or one NPC).
static std::string FindAnimation(const std::string& action, const char* prefix)
{
	std::string file = std::string(prefix) + action + ".kf";
	std::string gender = gAnim.female ? "female" : "male";
	bool top = action.rfind("jump", 0) == 0 || action == "aim" || std::string(prefix).rfind("sneak", 0) == 0;
	std::error_code ec;
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

// Each action for the current stance. A weapon stance's standing pose is its aim when it has no idle; what a
// stance lacks is borrowed from unarmed (noted in gAnim.borrowed), as the game would show something.
static void FindAnimations()
{
	gAnim.found.clear();
	gAnim.borrowed.clear();
	if (!gItem.loaded) return;
	std::string stance = kStancePrefix[gAnim.stance];
	std::vector<std::string> prefixes;
	if (gAnim.sneak) { prefixes.push_back("sneak" + stance); if (gAnim.stance != 0) prefixes.push_back("sneakmt"); }
	prefixes.push_back(stance);
	if (gAnim.stance != 0) prefixes.push_back("mt");
	for (const char* a : kActions) {
		std::string p;
		for (size_t i = 0; i < prefixes.size() && p.empty(); i++) {
			p = FindAnimation(a, prefixes[i].c_str());
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
	for (auto& t : play.clip->tracks) play.nodes.push_back(Bone(t.node.c_str()));
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
	if (gAnim.current.clip && gAnim.current.key == key && gAnim.current.path == path) return true;
	ClipPlay next;
	Play(next, key, path);
	if (!next.clip) return false;
	gAnim.previous = gAnim.current;
	gAnim.current = next;
	gAnim.blend = gAnim.previous.clip ? 0.0f : 1.0f;
	return true;
}

// A file for the "Animation file" preset (the browser, the file dialog, a drop, --kf).
static void PlayFile(const std::string& path)
{
	if (!LoadClip(path)) return;
	gAnim.customPath = path;
	gAnim.customRel.clear();
	gAnim.current = gAnim.previous = ClipPlay();   // from the start even if it was playing
	gAnim.preset = kCustom;
	gAnim.paused = false;
	auto* c = LoadClip(path);
	AddLog(2, "animation: " + path + " (" + std::to_string(c->tracks.size()) + " tracks, " + std::to_string(c->Duration()).substr(0, 4) + " s)");
}

static void Advance(ClipPlay& p, float dt)
{
	if (!p.clip) return;
	p.time += dt;
	float d = p.clip->Duration();
	if (d <= 0) { p.time = p.clip->start; return; }
	if (p.clip->loops) p.time = p.clip->start + fmodf(std::max(0.0f, p.time - p.clip->start), d);
	else p.time = std::min(p.time, p.clip->stop);
}

// Poses the skeleton from the clips: the accumulation root at the identity (its rotation and height are in
// NonAccum's keys; its travel moves the actor), the clips crossfaded.
static void ApplyClips()
{
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
	return !gItem.skeletonPath.empty() && (gAnim.found.count("idle") || (gAnim.preset == kCustom && !gAnim.customPath.empty()));
}

static bool HasClip(const char* a) { return gAnim.found.count(a) > 0; }

// The body's motion and the clips for this moment; sets the actor's transform.
static void UpdateMotion(float dt)
{
	auto& m = gMotion;
	m.time += dt;
	ImGuiIO& io = ImGui::GetIO();
	bool animated = Animated();

	float targetSpeed = 0, targetYawRate = 0, strafe = 0;
	bool jumpNow = false, run = false;
	std::string want = "idle";
	switch (gAnim.preset) {
	case kWalk: targetSpeed = 150; want = "forward"; break;
	case kSneak: break;
	case kSneakWalk: targetSpeed = 150; want = "forward"; break;
	case kRun: targetSpeed = 400; want = "fastforward"; break;
	case kCircles: targetSpeed = 150; targetYawRate = 150.0f / 220.0f; want = "forward"; break;
	case kSpin: targetYawRate = 3.0f; want = HasClip("turnleft") ? "turnleft" : "idle"; break;
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
			if (ImGui::IsKeyPressed(ImGuiKey_C, false)) { gAnim.sneak = !gAnim.sneak; FindAnimations(); }
		}
		break;
	case kCustom: want = "custom"; break;
	}
	bool onGround = m.jump < 0 && m.jumpStage == Motion::kGround;
	jumpNow = jumpNow && onGround;
	m.groundTime = onGround && !jumpNow ? m.groundTime + dt : 0.0f;

	float k = 1.0f - std::exp(-dt * 8.0f);
	V3 velocity;
	if (animated) {
		// A jump: the take-off clip, the in-air loop for the flight, the landing clip, then the locomotion again.
		bool moving = std::fabs(m.velocity.y) > 60.0f;
		auto clipDone = [&]() { return gAnim.current.clip && m.stageTime >= gAnim.current.clip->Duration() / std::max(0.05f, gAnim.speedScale); };
		if (jumpNow) {
			m.jumpStage = HasClip("jumpstart") ? Motion::kTakeOff : Motion::kAir;
			m.stageTime = 0;
			if (m.jumpStage == Motion::kAir) m.jump = 0;
			jumpNow = false;
		}
		m.stageTime += dt;
		switch (m.jumpStage) {
		case Motion::kTakeOff:
			want = "jumpstart";
			if (gAnim.current.key == "jumpstart" && clipDone()) { m.jumpStage = Motion::kAir; m.stageTime = 0; m.jump = 0; }
			break;
		case Motion::kAir:
			want = moving && HasClip("jumploopforward") ? "jumploopforward" : "jumploop";
			if (m.jump < 0) { m.jumpStage = Motion::kLanding; m.stageTime = 0; }
			break;
		case Motion::kLanding:
			want = moving && HasClip("jumplandforward") ? "jumplandforward" : "jumpland";
			if (!HasClip(want.c_str()) || (gAnim.current.key == want && clipDone())) m.jumpStage = Motion::kGround;
			break;
		default:
			break;
		}
		bool airborne = m.jumpStage == Motion::kTakeOff || m.jumpStage == Motion::kAir;
		if (!Want(want)) {
			std::string alt = want.rfind("fast", 0) == 0 ? want.substr(4) : std::string("idle");
			if (!Want(alt)) Want("idle");
		}
		float rate = gAnim.speedScale;
		Advance(gAnim.current, dt * rate);
		Advance(gAnim.previous, dt * rate);
		gAnim.blend = std::min(1.0f, gAnim.blend + dt / 0.25f);
		ApplyClips();
		V3 clipV = gAnim.current.velocity, prevV = gAnim.previous.clip ? gAnim.previous.velocity : clipV;
		V3 v = prevV + (clipV - prevV) * gAnim.blend;
		bool hasRootMotion = std::fabs(clipV.x) + std::fabs(clipV.y) > 1.0f;
		velocity = hasRootMotion ? v * rate : V3(strafe, targetSpeed, 0) * gAnim.speedScale;
		if (!airborne) m.velocity = m.velocity + (velocity - m.velocity) * k;
		velocity = m.velocity;
	} else {
		targetSpeed *= gAnim.speedScale;
		m.speed += (targetSpeed - m.speed) * k;
		velocity = V3(strafe * gAnim.speedScale, m.speed, 0);
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
	if (gAnim.inPlace) { m.pos = V3(); m.yaw = 0; }

	auto* actor = gItem.scene.skeleton.actor;
	actor->local.rotate = scene::RotZ(m.yaw);
	actor->local.translate = { m.pos.x, m.pos.y, z };
}

static bool IsRight(const std::string& bone)
{
	return bone.find(" R ") != std::string::npos || bone.rfind("Bip01 R", 0) == 0 || bone.find(" R") == bone.size() - 2;
}

// The local-space operations of the stress pose, the hand rotations and the flex test, per bone (world
// rotations to apply before its children are placed).
struct BoneOps
{
	std::vector<std::pair<const Aim*, bool>> aims;   // aim, mirrored
	RE::NiMatrix3 turn;                              // world rotation (turns, hand, flex), applied after aims
	bool hasTurn = false;
};
static std::unordered_map<RE::NiAVObject*, BoneOps> gOps;

static void CollectOps()
{
	gOps.clear();
	float amount = gPose.amount;
	if (gPose.cycle) amount *= 0.5f - 0.5f * std::cos(gPose.clock * gPose.cycleSpeed * 2 * 3.14159f);
	const StressPose& sp = kStressPoses[std::clamp(gPose.stress, 0, (int)kStressPoses.size() - 1)];
	auto addTurn = [&](RE::NiAVObject* n, const RE::NiMatrix3& r) {
		auto& o = gOps[n];
		o.turn = o.hasTurn ? r * o.turn : r;
		o.hasTurn = true;
	};
	if (amount > 0) {
		for (auto& a : sp.aims)
			if (auto* n = Bone(a.bone)) gOps[n].aims.push_back({ &a, IsRight(a.bone) });
		for (auto& t : sp.turns)
			if (auto* n = Bone(t.bone)) {
				bool right = IsRight(t.bone);
				V3 axis = Cross(BodyToWorld(t.from, right), BodyToWorld(t.to, right));
				addTurn(n, AxisAngle(axis, t.degrees * amount * 3.14159f / 180.0f));
			}
	}
	for (auto& [name, deg] : gPose.manual) {
		auto* n = Bone(name.c_str());
		if (!n || (deg.x == 0 && deg.y == 0 && deg.z == 0)) continue;
		const float k = 3.14159f / 180.0f;
		addTurn(n, AxisAngle(ActorAxis(V3(0, 0, 1)), deg.z * k) * AxisAngle(ActorAxis(V3(0, 1, 0)), deg.y * k) * AxisAngle(ActorAxis(V3(1, 0, 0)), deg.x * k));
	}
	if (gPose.flex && !gSel.bone.empty())
		if (auto* n = Bone(gSel.bone.c_str()))
			addTurn(n, AxisAngle(ActorAxis(kAxes[gPose.flexAxis]), gPose.flexDegrees * std::sin(gPose.clock * gPose.flexSpeed * 2 * 3.14159f) * 3.14159f / 180.0f));
}

static float StressAmount()
{
	float amount = gPose.amount;
	if (gPose.cycle) amount *= 0.5f - 0.5f * std::cos(gPose.clock * gPose.cycleSpeed * 2 * 3.14159f);
	return amount;
}

// world = parent.world * local, down the tree; a bone with operations has its local changed first.
static void UpdateWorld(RE::NiAVObject* o, const RE::NiTransform& parentWorld)
{
	if (!o) return;
	if (auto it = gOps.find(o); it != gOps.end()) {
		RE::NiMatrix3 world = parentWorld.rotate * o->local.rotate;
		float amount = StressAmount();
		for (auto& [aim, mirrored] : it->second.aims) {
			// Where the descendant sits in this bone's space, through the locals between them (twist
			// bones and the like may sit in between).
			RE::NiAVObject* n = Bone(aim->toward);
			RE::NiTransform chain;
			while (n && n != o) { chain = n->local * chain; n = n->parent; }
			if (n != o || Len(V3(chain.translate)) < 1e-4f) continue;
			V3 dir = V3(world * chain.translate);
			world = RotationBetween(dir, BodyToWorld(aim->target, mirrored), amount) * world;
		}
		if (it->second.hasTurn) world = it->second.turn * world;
		o->local.rotate = parentWorld.rotate.Transpose() * world;
	}
	o->world = parentWorld * o->local;
	if (auto* n = static_cast<RE::NiNode*>(o->IsNode()))
		for (uint16_t i = 0; i < n->ChildEnd(); i++) UpdateWorld(n->children[i], o->world);
}

static void ApplyPose(float dt)
{
	if (!gItem.loaded) return;
	gPose.clock += dt;
	for (auto& [name, local] : gItem.rest)
		if (auto* n = Bone(name.c_str())) n->local = local;
	// The animations and the body's motion (paused: frozen where they are, the poses still move).
	UpdateMotion(gAnim.paused ? 0.0f : dt * gAnim.timeScale);
	auto* actor = gItem.scene.skeleton.actor;
	gHeading = actor->local.rotate;
	CollectOps();
	UpdateWorld(actor, RE::NiTransform());
	// The reference body follows: the same locals by name.
	if (gItem.reference.skeleton.actor) {
		for (auto& [name, node] : gItem.reference.skeleton.bones)
			if (auto* src = Bone(name.c_str())) node->local = src->local;
		gItem.reference.skeleton.actor->local = actor->local;
		gOps.clear();
		UpdateWorld(gItem.reference.skeleton.actor, RE::NiTransform());
	}
}

// --- loading ----------------------------------------------------------------------------------------------

static void LoadReference(const std::string& path)
{
	gItem.reference = scene::Item();
	gItem.referenceShapes.clear();
	gItem.referencePath.clear();
	if (path.empty() || !gItem.loaded) return;
	try {
		gItem.reference = scene::Load(path, gItem.skeletonPath);
	} catch (const std::exception& e) {
		AddLog(4, "cannot read the reference " + path + ": " + e.what());
		gItem.reference = scene::Item();
		return;
	}
	gItem.referencePath = path;
	for (auto& info : gItem.reference.shapes) {
		ShapeRig d;
		PrepareShape(d, info, false);
		gItem.referenceShapes.push_back(std::move(d));
	}
	IniSet("Reference", path);
	AddLog(2, "reference: " + path);
}

static std::string FindBody(bool female, const char* part)
{
	std::string rel = std::string("meshes\\characters\\_male\\") + (female ? "female" : "") + part + ".nif";
	return datafiles::Resolve(gItem.roots, rel);
}

static void LoadItem(const std::string& path)
{
	AddLog(2, "loading " + path);
	std::string keepBone = gSel.bone;
	try {
		Item next;
		next.nifPath = path;
		next.nifTime = TimeOf(path);
		next.roots = datafiles::Discover(path, gSources);
		next.skeletonPath = datafiles::Resolve(next.roots, "meshes\\characters\\_male\\skeleton.nif");
		try {
			next.scene = scene::Load(path, next.skeletonPath);
		} catch (const std::exception& e) {
			if (next.skeletonPath.empty()) throw;
			AddLog(3, "cannot read the skeleton " + next.skeletonPath + " (" + e.what() + "): using the mesh's bind pose");
			next.skeletonPath.clear();
			next.scene = scene::Load(path);
		}
		next.loaded = true;
		gItem = std::move(next);
	} catch (const std::exception& e) {
		AddLog(4, std::string("cannot read ") + path + ": " + e.what());
		return;
	}
	IniSet("LastFile", path);
	BuildItemBones();
	for (auto& [name, node] : gItem.scene.skeleton.bones) gItem.rest[name] = node->local;
	BodyFrame();
	for (auto& info : gItem.scene.shapes) {
		ShapeRig d;
		PrepareShape(d, info, true);
		gItem.shapes.push_back(std::move(d));
	}
	try {
		ReadSkinRecords(path, gItem.shapes);
	} catch (const std::exception& e) {
		AddLog(3, std::string("cannot read the skin data: ") + e.what());
	}
	for (auto& s : gItem.shapes) Analyse(s);
	for (auto& s : gItem.shapes) for (auto& note : s.partitionNotes) AddLog(3, s.name + ", " + note);
	ApplyWeightSource();
	// The clips' nodes are the new skeleton's: they start over; the locomotion set is the new mesh's gender.
	gMotion = Motion();
	gAnim.current = gAnim.previous = ClipPlay();
	{
		std::string file = Lower(fs::path(path).stem().string());
		gAnim.female = file.size() > 2 && (file.compare(file.size() - 2, 2, "_f") == 0 || file.find("female") != std::string::npos);
	}
	FindAnimations();
	if (!gItem.skeletonPath.empty())
		AddLog(2, std::to_string(gAnim.found.size()) + " locomotion animations found (" + (gAnim.female ? "female" : "male") + ")");
	gSel.shape = gSel.vertex = -1;
	gSel.bone = Bone(keepBone.c_str()) || gItem.unknownBones.count(keepBone) ? keepBone : std::string();
	if (gItem.skeletonPath.empty()) AddLog(3, "no skeleton.nif found in the Data folders or archives: the mesh's own bind pose is the skeleton");
	else AddLog(2, "skeleton: " + gItem.skeletonPath);
	int problems = 0;
	for (auto& s : gItem.shapes) for (int c : s.problemCounts) problems += c;
	AddLog(problems ? 3 : 2, std::to_string(gItem.shapes.size()) + " shapes, " + std::to_string(gItem.usedBones.size()) + " skeleton bones used" +
		(gItem.unknownBones.empty() ? std::string() : ", " + std::to_string(gItem.unknownBones.size()) + " not in the skeleton") +
		(problems ? ", " + std::to_string(problems) + " vertex problems found" : ", no weight problems found"));
	auto& r = gItem.roots;
	AddLog(2, r.mo2Instance.empty() ? std::string("MO2: none found") : "MO2: " + r.mo2Instance + ", profile '" + r.mo2Profile + "' (" + r.mo2How + ")");
	AddLog(2, r.gameData.empty() ? std::string("game: not found") : "game: " + r.gameData + " (" + r.gameHow + ")");
	for (auto& n : r.notes) AddLog(3, n);
	std::string ref = IniGet("Reference");
	if (IniGet("ShowReference", "0") == "1" && !ref.empty() && fs::exists(ref)) LoadReference(ref);
	ApplyPose(0);
}

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

// Every .kf under meshes\characters (loose in the Data folders, then in the archives), Data-relative, for
// the animation browser. Listed once per load (a few thousand names).
static std::vector<std::string> gAnimList;
static bool gAnimListed = false;

static void ListAnimations()
{
	gAnimList.clear();
	std::set<std::string> seen;
	std::error_code ec;
	for (auto& root : gItem.roots.folders) {
		fs::path base = fs::path(root) / "meshes" / "characters";
		if (!fs::is_directory(base, ec)) continue;
		for (auto it = fs::recursive_directory_iterator(base, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
			if (ec) break;
			if (!it->is_regular_file(ec) || Lower(it->path().extension().string()) != ".kf") continue;
			std::string rel = Lower(fs::relative(it->path(), root, ec).string());
			if (seen.insert(rel).second) gAnimList.push_back(rel);
		}
	}
	for (auto& a : gItem.roots.archives)
		a->ForEach([&](const std::string& p) {
			if (p.rfind("meshes\\characters\\", 0) == 0 && p.size() > 3 && p.compare(p.size() - 3, 3, ".kf") == 0 && seen.insert(p).second) gAnimList.push_back(p);
		});
	std::sort(gAnimList.begin(), gAnimList.end());
	gAnimListed = true;
	AddLog(2, std::to_string(gAnimList.size()) + " animations listed");
}

// --- weight colours ---------------------------------------------------------------------------------------

enum View { kTextured, kShaded, kBoneWeights, kMainBone, kInfluenceCount, kProblems };
static const char* kViews[] = { "Textured", "Shaded", "Selected bone's weights", "Main bone", "Number of bones", "Problems" };

struct Display
{
	int view = kBoneWeights;
	bool wireframe = false;
	bool skeleton = true;
	bool boneNames = false;
	bool onlyUsedBones = true;
	bool floor = true;
	bool referenceOn = false;
	float referenceAlpha = 0.45f;
	bool clickSelectsBone = true;
	float background[3] = { 0.17f, 0.18f, 0.20f };
	bool autoReload = true;
} gD;

static D3DCOLOR Heat(float w)
{
	// Dark blue at 0, through blue, cyan, green and yellow to red at 1.
	w = std::clamp(w, 0.0f, 1.0f);
	if (w <= 0.0f) return D3DCOLOR_ARGB(255, 18, 22, 70);
	const float stops[][3] = { { 0.10f, 0.15f, 0.75f }, { 0.0f, 0.75f, 0.95f }, { 0.15f, 0.85f, 0.25f }, { 0.95f, 0.90f, 0.15f }, { 0.95f, 0.15f, 0.10f } };
	float x = w * 4.0f;
	int i = std::min(3, (int)x);
	float t = x - i;
	float r = stops[i][0] + (stops[i + 1][0] - stops[i][0]) * t, g = stops[i][1] + (stops[i + 1][1] - stops[i][1]) * t, b = stops[i][2] + (stops[i + 1][2] - stops[i][2]) * t;
	return D3DCOLOR_ARGB(255, (int)(r * 255), (int)(g * 255), (int)(b * 255));
}

static D3DCOLOR BoneColor(const std::string& name)
{
	uint32_t h = 2166136261u;
	for (char c : name) h = (h ^ (uint8_t)c) * 16777619u;
	float hue = (h % 360) / 60.0f, s = 0.55f + ((h >> 9) % 30) / 100.0f, v = 0.75f + ((h >> 17) % 20) / 100.0f;
	int i = (int)hue;
	float f = hue - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
	float r, g, b;
	switch (i % 6) { case 0: r = v; g = t; b = p; break; case 1: r = q; g = v; b = p; break; case 2: r = p; g = v; b = t; break;
		case 3: r = p; g = q; b = v; break; case 4: r = t; g = p; b = v; break; default: r = v; g = p; b = q; break; }
	return D3DCOLOR_ARGB(255, (int)(r * 255), (int)(g * 255), (int)(b * 255));
}

static float WeightFor(const ShapeRig& s, int v, const std::string& bone)
{
	float w = 0;
	for (auto& [slot, wt] : s.weights[v])
		if (slot < (int)s.slotNames.size() && s.slotNames[slot] == bone) w += wt;
	return w;
}

static D3DCOLOR VertexColor(const ShapeRig& s, int v)
{
	switch (gD.view) {
	case kBoneWeights:
		return gSel.bone.empty() ? D3DCOLOR_ARGB(255, 150, 150, 155) : Heat(WeightFor(s, v, gSel.bone));
	case kMainBone: {
		int best = -1;
		float bw = 0;
		for (auto& [slot, w] : s.weights[v]) if (w > bw) { bw = w; best = slot; }
		if (best < 0 || best >= (int)s.slotNames.size()) return D3DCOLOR_ARGB(255, 255, 40, 220);
		return BoneColor(s.slotNames[best]);
	}
	case kInfluenceCount: {
		static const D3DCOLOR c[] = { D3DCOLOR_ARGB(255, 255, 40, 220), D3DCOLOR_ARGB(255, 60, 110, 220), D3DCOLOR_ARGB(255, 40, 190, 120), D3DCOLOR_ARGB(255, 230, 200, 40), D3DCOLOR_ARGB(255, 240, 120, 30), D3DCOLOR_ARGB(255, 230, 40, 40) };
		int n = 0;
		for (auto& [slot, w] : s.weights[v]) if (w > 0.001f) n++;
		if ((int)s.dataWeights.size() > v) n = std::max(n, (int)s.dataWeights[v].size());
		return c[std::min(n, 5)];
	}
	case kProblems: {
		uint8_t p = v < (int)s.problems.size() ? s.problems[v] : 0;
		for (int b = 0; b < 6; b++) if (p & (1 << b)) return kProblemColors[b];
		return D3DCOLOR_ARGB(255, 120, 124, 130);
	}
	}
	return D3DCOLOR_ARGB(255, 255, 255, 255);
}

// --- camera -----------------------------------------------------------------------------------------------

struct Camera
{
	float yaw = 0.35f, pitch = 0.12f, dist = 190.0f;
	V3 target = V3(0, 0, 75);
	bool follow = true;   // the target travels with the body (its ground position), keeping the framing
	V3 lastBody;
} gCam;

static V3 BodyGround()
{
	auto* a = gItem.scene.skeleton.actor;
	return a ? V3(a->local.translate.x, a->local.translate.y, 0) : V3();
}

static V3 CameraEye()
{
	V3 dir(-std::sin(gCam.yaw) * std::cos(gCam.pitch), std::cos(gCam.yaw) * std::cos(gCam.pitch), std::sin(gCam.pitch));
	return gCam.target + dir * gCam.dist;
}

static void FrameSelection()
{
	if (!gSel.bone.empty())
		if (auto* n = Bone(gSel.bone.c_str())) { gCam.target = V3(n->world.translate); gCam.dist = 90; return; }
	if (auto* p = Bone("Bip01 Spine")) gCam.target = V3(p->world.translate);
	gCam.dist = 190;
}

static void UpdateCamera()
{
	ImGuiIO& io = ImGui::GetIO();
	V3 body = BodyGround();
	if (gCam.follow) gCam.target = gCam.target + (body - gCam.lastBody);
	gCam.lastBody = body;
	if (io.WantCaptureMouse) return;
	if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)) {
		gCam.yaw -= io.MouseDelta.x * 0.008f;
		gCam.pitch = std::clamp(gCam.pitch + io.MouseDelta.y * 0.008f, -1.5f, 1.5f);
	}
	if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0)) {
		V3 eye = CameraEye(), fwd = Norm(gCam.target - eye), right = Norm(Cross(fwd, V3(0, 0, 1))), up = Cross(right, fwd);
		float s = gCam.dist * 0.0016f;
		gCam.target = gCam.target + right * (-io.MouseDelta.x * s) + up * (io.MouseDelta.y * s);
	}
	if (io.MouseWheel != 0) gCam.dist = std::clamp(gCam.dist * std::pow(0.88f, io.MouseWheel), 8.0f, 2000.0f);
	if (!io.WantCaptureKeyboard && ImGui::IsKeyPressed(ImGuiKey_F)) FrameSelection();
}

// --- drawing ----------------------------------------------------------------------------------------------

static void SkinShape(ShapeRig& s, bool colours)
{
	auto* g = s.info.geometry;
	int n = (int)s.verts.size();
	bool hasNormals = (int)s.info.normals.size() == n;
	for (int v = 0; v < n; v++) {
		RE::NiPoint3 p = g->skinInstance ? scene::SkinVertex(g, v, s.weights) : g->world * g->data->vertices[v];
		auto& o = s.verts[v];
		o.px = p.x; o.py = p.y; o.pz = p.z;
		if (hasNormals) {
			RE::NiPoint3 nn = g->skinInstance ? scene::SkinNormal(g, s.info.normals[v], v, s.weights)
			                                  : g->world.rotate * RE::NiPoint3{ s.info.normals[v].x, s.info.normals[v].y, s.info.normals[v].z };
			o.nx = nn.x; o.ny = nn.y; o.nz = nn.z;
		}
		if (v < (int)s.info.uvs.size()) { o.u = s.info.uvs[v].first; o.v = s.info.uvs[v].second; }
		o.color = colours && v < (int)s.weights.size() ? VertexColor(s, v) : D3DCOLOR_ARGB(255, 255, 255, 255);
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

static void SetPS(const float color[4], float textured, float alphaTest, float unlit, float vertexColours)
{
	V3 key = Norm(V3(0.35f, 0.6f, 0.7f)), fill = Norm(V3(-0.6f, -0.4f, 0.2f));
	float c[16] = { key.x, key.y, key.z, 0, color[0], color[1], color[2], color[3], textured, alphaTest, unlit, vertexColours, fill.x, fill.y, fill.z, 0 };
	gDev->SetPixelShaderConstantF(0, c, 4);
}

static void DrawLines(const std::vector<LineVertex>& lines, D3DPRIMITIVETYPE type, bool onTop, float pointSize = 6.0f)
{
	if (lines.empty()) return;
	gDev->SetVertexShader(nullptr);
	gDev->SetPixelShader(nullptr);
	gDev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
	gDev->SetTexture(0, nullptr);
	gDev->SetRenderState(D3DRS_LIGHTING, FALSE);
	gDev->SetRenderState(D3DRS_ZENABLE, onTop ? FALSE : TRUE);
	gDev->SetRenderState(D3DRS_POINTSIZE_MIN, 0);
	gDev->SetRenderState(D3DRS_POINTSIZE, *(DWORD*)&pointSize);
	UINT count = type == D3DPT_LINELIST ? (UINT)lines.size() / 2 : (UINT)lines.size();
	gDev->DrawPrimitiveUP(type, count, lines.data(), sizeof(LineVertex));
	gDev->SetRenderState(D3DRS_ZENABLE, TRUE);
}

static void DrawShapes(std::vector<ShapeRig>& shapes, bool reference)
{
	gDev->SetVertexShader(gVS);
	gDev->SetPixelShader(gPS);
	gDev->SetVertexDeclaration(gDecl);
	static const float kPalette[][4] = { { 0.62f, 0.60f, 0.58f, 1 }, { 0.56f, 0.57f, 0.60f, 1 }, { 0.60f, 0.58f, 0.55f, 1 }, { 0.54f, 0.56f, 0.58f, 1 } };
	bool colours = !reference && gD.view >= kBoneWeights;
	if (reference) {
		gDev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
		gDev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		gDev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
		gDev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	}
	for (size_t i = 0; i < shapes.size(); i++) {
		auto& s = shapes[i];
		if (!s.visible || s.indices.empty()) continue;
		SkinShape(s, colours);
		if (reference) {
			const float grey[4] = { 0.75f, 0.72f, 0.70f, gD.referenceAlpha };
			SetPS(grey, 0, 0, 0, 0);
			gDev->SetTexture(0, nullptr);
		} else {
			bool tex = gD.view == kTextured && s.texture && !s.info.uvs.empty();
			const float white[4] = { 1, 1, 1, 1 };
			SetPS(tex ? white : kPalette[i % 4], tex ? 1.0f : 0.0f, s.info.alpha && tex ? 1.0f : 0.0f, 0, colours ? 1.0f : 0.0f);
			gDev->SetTexture(0, tex ? s.texture : nullptr);
		}
		gDev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)s.verts.size(), (UINT)s.indices.size() / 3, s.indices.data(), D3DFMT_INDEX16, s.verts.data(), sizeof(MeshVertex));
	}
	if (reference) {
		gDev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		gDev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		return;
	}
	if (gD.wireframe) {
		gDev->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
		gDev->SetTexture(0, nullptr);
		const float wire[4] = { 0.03f, 0.03f, 0.03f, 1 };
		SetPS(wire, 0, 0, 1, 0);
		for (auto& s : shapes)
			if (s.visible && !s.indices.empty())
				gDev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)s.verts.size(), (UINT)s.indices.size() / 3, s.indices.data(), D3DFMT_INDEX16, s.verts.data(), sizeof(MeshVertex));
		gDev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
	}
}

static bool Project(const M4& viewProj, V3 p, ImVec2& out)
{
	float x = p.x * viewProj.m[0][0] + p.y * viewProj.m[1][0] + p.z * viewProj.m[2][0] + viewProj.m[3][0];
	float y = p.x * viewProj.m[0][1] + p.y * viewProj.m[1][1] + p.z * viewProj.m[2][1] + viewProj.m[3][1];
	float w = p.x * viewProj.m[0][3] + p.y * viewProj.m[1][3] + p.z * viewProj.m[2][3] + viewProj.m[3][3];
	if (w <= 0.01f) return false;
	ImVec2 size = ImGui::GetIO().DisplaySize;
	out = ImVec2((x / w * 0.5f + 0.5f) * size.x, (0.5f - y / w * 0.5f) * size.y);
	return true;
}

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

	if (gD.floor) {
		std::vector<LineVertex> grid;
		const float cell = 16.0f;
		const int n = 16;
		// Around the body as it walks, on whole cells so the grid does not slide.
		V3 body = BodyGround();
		float cx = std::floor(body.x / cell) * cell, cy = std::floor(body.y / cell) * cell;
		for (int i = -n; i <= n; i++) {
			D3DCOLOR c = i == 0 ? D3DCOLOR_ARGB(255, 90, 95, 105) : D3DCOLOR_ARGB(255, 60, 64, 72);
			grid.push_back({ cx + i * cell, cy - n * cell, 0, c });
			grid.push_back({ cx + i * cell, cy + n * cell, 0, c });
			grid.push_back({ cx - n * cell, cy + i * cell, 0, c });
			grid.push_back({ cx + n * cell, cy + i * cell, 0, c });
		}
		DrawLines(grid, D3DPT_LINELIST, false);
	}

	gDev->SetVertexShaderConstantF(0, (float*)&viewProj, 4);
	gDev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
	gDev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	gDev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	gDev->SetSamplerState(0, D3DSAMP_MAXANISOTROPY, 8);
	DrawShapes(gItem.shapes, false);
	if (gD.referenceOn && !gItem.referenceShapes.empty()) DrawShapes(gItem.referenceShapes, true);

	// The skeleton, through everything: used bones bright, the selected one yellow.
	if (gD.skeleton) {
		std::vector<LineVertex> lines, points, selected;
		for (auto& [name, node] : gItem.scene.skeleton.bones) {
			bool used = gItem.usedBones.count(name) > 0, sel = name == gSel.bone;
			if (gD.onlyUsedBones && !used && !sel) continue;
			V3 p(node->world.translate);
			D3DCOLOR c = sel ? D3DCOLOR_ARGB(255, 255, 230, 60) : used ? D3DCOLOR_ARGB(255, 120, 200, 255) : D3DCOLOR_ARGB(255, 110, 115, 128);
			(sel ? selected : points).push_back({ p.x, p.y, p.z, c });
			RE::NiNode* parent = node->parent;
			if (parent && parent != gItem.scene.skeleton.actor && parent->m_name) {
				V3 q(parent->world.translate);
				lines.push_back({ q.x, q.y, q.z, c });
				lines.push_back({ p.x, p.y, p.z, c });
			}
		}
		DrawLines(lines, D3DPT_LINELIST, true);
		DrawLines(points, D3DPT_POINTLIST, true, 6.0f);
		DrawLines(selected, D3DPT_POINTLIST, true, 11.0f);
	}

	// The picked vertex, and a line to each bone that moves it (brighter for more weight).
	if (gSel.shape >= 0 && gSel.shape < (int)gItem.shapes.size()) {
		auto& s = gItem.shapes[gSel.shape];
		if (gSel.vertex >= 0 && gSel.vertex < (int)s.verts.size()) {
			auto& mv = s.verts[gSel.vertex];
			V3 p(mv.px, mv.py, mv.pz);
			std::vector<LineVertex> lines, pt = { { p.x, p.y, p.z, D3DCOLOR_ARGB(255, 255, 255, 255) } };
			if (s.info.geometry->skinInstance)
				for (auto& [slot, w] : s.weights[gSel.vertex]) {
					auto* b = s.info.geometry->skinInstance->bones[slot];
					V3 q(b->world.translate);
					D3DCOLOR c = Heat(w);
					lines.push_back({ p.x, p.y, p.z, c });
					lines.push_back({ q.x, q.y, q.z, c });
				}
			DrawLines(lines, D3DPT_LINELIST, true);
			DrawLines(pt, D3DPT_POINTLIST, true, 10.0f);
		}
	}

	// Names next to the joints: the selected bone always, every used bone with "Bone names".
	if (gD.skeleton) {
		ImDrawList* dl = ImGui::GetBackgroundDrawList();
		for (auto& [name, node] : gItem.scene.skeleton.bones) {
			bool sel = name == gSel.bone, hover = name == gSel.hoverBone;
			if (!sel && !hover && !(gD.boneNames && gItem.usedBones.count(name))) continue;
			ImVec2 sp;
			if (!Project(viewProj, V3(node->world.translate), sp)) continue;
			ImU32 col = sel ? IM_COL32(255, 230, 60, 255) : IM_COL32(200, 220, 255, 220);
			dl->AddText(ImVec2(sp.x + 7, sp.y - 7), IM_COL32(0, 0, 0, 200), name.c_str());
			dl->AddText(ImVec2(sp.x + 6, sp.y - 8), col, name.c_str());
		}
	}
}

// --- picking ----------------------------------------------------------------------------------------------
//
// A click casts a ray from the camera through the mouse: the nearest triangle it hits gives the vertex
// nearest the hit. Shift-click picks the joint nearest the mouse on screen.

static void Pick(const M4& viewProj)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureMouse || !gItem.loaded || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
	ImVec2 mouse = io.MousePos, size = io.DisplaySize;
	if (io.KeyShift) {
		float best = 18.0f * 18.0f;
		std::string pick;
		for (auto& [name, node] : gItem.scene.skeleton.bones) {
			if (gD.onlyUsedBones && !gItem.usedBones.count(name)) continue;
			ImVec2 sp;
			if (!Project(viewProj, V3(node->world.translate), sp)) continue;
			float d = (sp.x - mouse.x) * (sp.x - mouse.x) + (sp.y - mouse.y) * (sp.y - mouse.y);
			if (d < best) { best = d; pick = name; }
		}
		if (!pick.empty()) gSel.bone = pick;
		return;
	}
	V3 eye = CameraEye(), fwd = Norm(gCam.target - eye), right = Norm(Cross(fwd, V3(0, 0, 1))), up = Cross(right, fwd);
	float tanY = std::tan(0.75f / 2), tanX = tanY * size.x / std::max(1.0f, size.y);
	float nx = mouse.x / size.x * 2 - 1, ny = 1 - mouse.y / size.y * 2;
	V3 dir = Norm(fwd + right * (nx * tanX) + up * (ny * tanY));
	float bestT = 1e30f;
	int bestShape = -1, bestVertex = -1;
	for (size_t si = 0; si < gItem.shapes.size(); si++) {
		auto& s = gItem.shapes[si];
		if (!s.visible) continue;
		for (size_t t = 0; t + 2 < s.indices.size(); t += 3) {
			int ia = s.indices[t], ib = s.indices[t + 1], ic = s.indices[t + 2];
			V3 a(s.verts[ia].px, s.verts[ia].py, s.verts[ia].pz), b(s.verts[ib].px, s.verts[ib].py, s.verts[ib].pz), c(s.verts[ic].px, s.verts[ic].py, s.verts[ic].pz);
			V3 e1 = b - a, e2 = c - a, pv = Cross(dir, e2);
			float det = Dot(e1, pv);
			if (std::fabs(det) < 1e-9f) continue;
			float inv = 1.0f / det;
			V3 tv = eye - a;
			float u = Dot(tv, pv) * inv;
			if (u < 0 || u > 1) continue;
			V3 qv = Cross(tv, e1);
			float v = Dot(dir, qv) * inv;
			if (v < 0 || u + v > 1) continue;
			float hit = Dot(e2, qv) * inv;
			if (hit <= 0 || hit >= bestT) continue;
			bestT = hit;
			bestShape = (int)si;
			bestVertex = (1 - u - v) >= u && (1 - u - v) >= v ? ia : (u >= v ? ib : ic);
		}
	}
	gSel.shape = bestShape;
	gSel.vertex = bestVertex;
	if (bestShape >= 0 && gD.clickSelectsBone) {
		auto& s = gItem.shapes[bestShape];
		float bw = 0;
		for (auto& [slot, w] : s.weights[bestVertex])
			if (w > bw && slot < (int)s.slotNames.size()) { bw = w; gSel.bone = s.slotNames[slot]; }
	}
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

static void HeatLegend()
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	float w = ImGui::GetContentRegionAvail().x, h = 12;
	for (int i = 0; i < 64; i++) {
		D3DCOLOR c = Heat(i / 63.0f);
		ImU32 col = IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
		dl->AddRectFilled(ImVec2(p.x + w * i / 64, p.y), ImVec2(p.x + w * (i + 1) / 64 + 1, p.y + h), col);
	}
	ImGui::Dummy(ImVec2(w, h));
	ImGui::TextDisabled("0");
	ImGui::SameLine(w * 0.5f - 8);
	ImGui::TextDisabled("0.5");
	ImGui::SameLine(w - 8);
	ImGui::TextDisabled("1");
}

static void BoneTree(RE::NiNode* node, const std::string& filter)
{
	if (!node) return;
	std::string name = node->m_name ? node->m_name : "";
	bool used = gItem.usedBones.count(name) > 0;
	std::vector<RE::NiNode*> kids;
	for (uint16_t i = 0; i < node->ChildEnd(); i++)
		if (auto* c = static_cast<RE::NiNode*>(node->children[i] ? node->children[i]->IsNode() : nullptr))
			if (c != gItem.scene.model) kids.push_back(c);
	bool match = filter.empty() || Lower(name).find(filter) != std::string::npos;
	if (!filter.empty()) {
		// Filtered: a flat list of the matches.
		if (match && !name.empty()) {
			if (ImGui::Selectable((name + (used ? "" : "  (unused)")).c_str(), gSel.bone == name)) gSel.bone = name;
			if (ImGui::IsItemHovered()) gSel.hoverBone = name;
		}
		for (auto* c : kids) BoneTree(c, filter);
		return;
	}
	if (gD.onlyUsedBones && !used) {
		// Unused bones are skipped, their used descendants shown at this level.
		for (auto* c : kids) BoneTree(c, filter);
		return;
	}
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
	if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
	if (gSel.bone == name) flags |= ImGuiTreeNodeFlags_Selected;
	bool item = gItem.itemBones.count(name) > 0;
	if (!used || item) ImGui::PushStyleColor(ImGuiCol_Text, item ? ImVec4(1.0f, 0.72f, 0.35f, 1) : ImVec4(0.55f, 0.57f, 0.62f, 1));
	bool open = ImGui::TreeNodeEx(name.empty() ? "(unnamed)" : name.c_str(), flags);
	if (!used || item) ImGui::PopStyleColor();
	if (item) ImGui::SetItemTooltip("The item's own bone (SMP Bones): in game the SMP physics moves it; here it is carried on its parent.");
	if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) gSel.bone = name;
	if (ImGui::IsItemHovered()) gSel.hoverBone = name;
	if (open) {
		for (auto* c : kids) BoneTree(c, filter);
		ImGui::TreePop();
	}
}

static void Panels(float fps)
{
	ImGuiIO& io = ImGui::GetIO();
	gSel.hoverBone.clear();
	ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(380, io.DisplaySize.y - 16), ImGuiCond_FirstUseEver);
	ImGui::Begin("NV Rig Previewer");

	if (ImGui::Button("Open NIF...")) {
		std::string last = IniGet("LastFile");
		std::string f = PickFile("NIF meshes (*.nif)\0*.nif\0All files\0*.*\0", last.empty() ? "" : fs::path(last).parent_path().string());
		if (!f.empty()) LoadItem(f);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!gItem.loaded);
	if (ImGui::Button("Reload")) LoadItem(gItem.nifPath);
	ImGui::SameLine();
	if (ImGui::Button("Folder")) ShellExecuteA(gWnd, "open", fs::path(gItem.nifPath).parent_path().string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Screenshot")) gShotPending = true;
	ImGui::Checkbox("Reload when the NIF changes", &gD.autoReload);
	if (gItem.loaded) ImGui::TextWrapped("%s", gItem.nifPath.c_str());
	else ImGui::TextWrapped("Open a skinned NIF (or drop one on the window).");
	ImGui::TextDisabled("%.0f fps", fps);

	if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Combo("Colours", &gD.view, kViews, IM_ARRAYSIZE(kViews));
		Help("Textured / Shaded: the mesh as it looks.\nSelected bone's weights: how much the selected bone moves each vertex, dark blue none, red all.\n"
		     "Main bone: each vertex in the colour of the bone that moves it most, so the borders between bones show.\n"
		     "Number of bones: magenta none, blue 1, green 2, yellow 3, orange 4, red more than 4.\nProblems: see Weight problems below.");
		if (gD.view == kBoneWeights) HeatLegend();
		ImGui::TextUnformatted("Weights from");
		ImGui::SameLine();
		if (ImGui::RadioButton("Partitions", !gWeightsFromData)) { gWeightsFromData = false; ApplyWeightSource(); }
		ImGui::SameLine();
		if (ImGui::RadioButton("Skin data", gWeightsFromData)) { gWeightsFromData = true; ApplyWeightSource(); }
		Help("Partitions: the weights the game draws with (what the views show by default).\nSkin data: the weights the exporter wrote, before partitioning.\n"
		     "If the mesh looks right on the skin data and wrong on the partitions, the export's partitioning broke it: re-export, or update the partitions in NifSkope.");
		ImGui::Checkbox("Wireframe", &gD.wireframe);
		ImGui::SameLine();
		ImGui::Checkbox("Skeleton", &gD.skeleton);
		ImGui::SameLine();
		ImGui::Checkbox("Bone names", &gD.boneNames);
		ImGui::Checkbox("Only bones the mesh uses", &gD.onlyUsedBones);
		ImGui::SameLine();
		ImGui::Checkbox("Floor", &gD.floor);
		ImGui::Checkbox("Clicking a vertex selects its main bone", &gD.clickSelectsBone);
		bool ref = gD.referenceOn;
		if (ImGui::Checkbox("Body for the fit", &ref)) {
			gD.referenceOn = ref;
			IniSet("ShowReference", ref ? "1" : "0");
			if (ref && gItem.referenceShapes.empty()) {
				std::string body = FindBody(false, "upperbody");
				if (body.empty()) AddLog(3, "no upperbody.nif found: pick a body with Pick...");
				else LoadReference(body);
			}
		}
		Help("A body drawn see-through over the mesh, posed with it: to see the mesh follow the body and where it pokes through.");
		ImGui::SameLine();
		if (ImGui::SmallButton("Male")) { LoadReference(FindBody(false, "upperbody")); gD.referenceOn = true; }
		ImGui::SameLine();
		if (ImGui::SmallButton("Female")) { LoadReference(FindBody(true, "upperbody")); gD.referenceOn = true; }
		ImGui::SameLine();
		if (ImGui::SmallButton("Pick...")) {
			std::string f = PickFile("NIF meshes (*.nif)\0*.nif\0", gItem.referencePath.empty() ? "" : fs::path(gItem.referencePath).parent_path().string());
			if (!f.empty()) { LoadReference(f); gD.referenceOn = true; }
		}
		if (gD.referenceOn) ImGui::SliderFloat("Body opacity", &gD.referenceAlpha, 0.1f, 1.0f);
		ImGui::ColorEdit3("Background", gD.background, ImGuiColorEditFlags_NoInputs);
		ImGui::TextDisabled("Right drag turns, middle drag pans, wheel zooms, F frames the bone.\nClick picks a vertex, Shift+click a joint.");
	}

	if (gItem.loaded && ImGui::CollapsingHeader("Weight problems", ImGuiTreeNodeFlags_DefaultOpen)) {
		int totals[6] = {};
		for (auto& s : gItem.shapes) for (int b = 0; b < 6; b++) totals[b] += s.problemCounts[b];
		bool any = false;
		for (int b = 0; b < 6; b++) {
			if (!totals[b]) continue;
			any = true;
			D3DCOLOR c = kProblemColors[b];
			ImGui::TextColored(ImVec4(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1), "%s: %d vertices", kProblemNames[b], totals[b]);
		}
		if (!any) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "No problems found.");
		else if (ImGui::SmallButton("Show them")) gD.view = kProblems;
		Help("Unweighted: no bone moves the vertex; it stays behind when the body moves.\nNot adding up to 1: the vertex drifts toward or past its bones.\n"
		     "More than 4 bones: the game uses 4 per vertex; the exporter's extra weights are lost.\n"
		     "Lost in the partitions: the partitions the game draws with differ from the skin data (re-export, or update the partitions).\n"
		     "Not in the skeleton: the bone is missing from skeleton.nif; the game hangs those vertices on the skeleton root, so they do not follow the body.\n"
		     "Broken partition: the partition holding the vertex is missing data or points past its bones, or two partitions draw the vertex with different weights (see the log).");
		if (!gItem.unknownBones.empty()) {
			std::string list;
			for (auto& b : gItem.unknownBones) list += (list.empty() ? "" : ", ") + b;
			if (ImGui::TreeNode("missing", "Bones not in the skeleton (%zu)", gItem.unknownBones.size())) { ImGui::TextWrapped("%s", list.c_str()); ImGui::TreePop(); }
		}
	}

	if (gItem.loaded && ImGui::CollapsingHeader("Shapes")) {
		for (size_t i = 0; i < gItem.shapes.size(); i++) {
			auto& s = gItem.shapes[i];
			ImGui::PushID((int)i);
			ImGui::Checkbox("##vis", &s.visible);
			ImGui::SameLine();
			bool skinned = s.info.geometry->skinInstance != nullptr;
			if (ImGui::TreeNode(s.name.c_str(), "%s  (%zu verts%s)", s.name.c_str(), s.verts.size(), skinned ? "" : ", not skinned")) {
				ImGui::Text("%zu triangles, %zu bones, %d partitions", s.indices.size() / 3, s.slotNames.size(), s.partitions);
				ImGui::Text("Most bones on a vertex: %d in the skin data, %d drawn", s.maxDataInfluences, s.maxPartInfluences);
				if (!s.textureNote.empty()) ImGui::TextColored(ImVec4(1, 0.6f, 0.4f, 1), "texture: %s", s.textureNote.c_str());
				for (int b = 0; b < 6; b++) if (s.problemCounts[b]) ImGui::Text("%s: %d", kProblemNames[b], s.problemCounts[b]);
				for (auto& note : s.partitionNotes) ImGui::TextColored(ImVec4(0.75f, 0.6f, 1, 1), "%s", note.c_str());
				// Per bone: how many vertices it moves, its strongest weight.
				if (skinned && ImGui::BeginTable("bones", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Sortable)) {
					ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("Verts");
					ImGui::TableSetupColumn("Max");
					ImGui::TableSetupColumn("");
					ImGui::TableHeadersRow();
					std::vector<int> count(s.slotNames.size(), 0);
					std::vector<float> peak(s.slotNames.size(), 0);
					for (auto& inf : s.weights)
						for (auto& [slot, w] : inf)
							if (slot < (int)count.size() && w > 0.001f) { count[slot]++; peak[slot] = std::max(peak[slot], w); }
					for (size_t b = 0; b < s.slotNames.size(); b++) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						if (ImGui::Selectable(s.slotNames[b].c_str(), gSel.bone == s.slotNames[b], ImGuiSelectableFlags_SpanAllColumns)) gSel.bone = s.slotNames[b];
						ImGui::TableNextColumn();
						ImGui::Text("%d", count[b]);
						ImGui::TableNextColumn();
						ImGui::Text("%.2f", peak[b]);
						ImGui::TableNextColumn();
						if (!s.slotKnown[b]) ImGui::TextColored(ImVec4(0.3f, 0.85f, 1, 1), "not in skeleton");
						else if (count[b] == 0) ImGui::TextDisabled("unused");
					}
					ImGui::EndTable();
				}
				ImGui::TreePop();
			}
			ImGui::PopID();
		}
	}

	if (gItem.loaded && ImGui::CollapsingHeader("Bones", ImGuiTreeNodeFlags_DefaultOpen)) {
		static char filter[64] = "";
		ImGui::InputTextWithHint("##filter", "find a bone", filter, sizeof filter);
		ImGui::BeginChild("tree", ImVec2(0, 220), ImGuiChildFlags_Borders);
		std::string f = Lower(filter);
		BoneTree(gItem.scene.skeleton.actor, f);
		for (auto& b : gItem.unknownBones) {
			if (!f.empty() && Lower(b).find(f) == std::string::npos) continue;
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.85f, 1, 1));
			if (ImGui::Selectable((b + "  (not in skeleton)").c_str(), gSel.bone == b)) gSel.bone = b;
			ImGui::PopStyleColor();
		}
		ImGui::EndChild();
		if (!gSel.bone.empty()) {
			ImGui::Text("Selected: %s", gSel.bone.c_str());
			int verts = 0;
			for (auto& s : gItem.shapes) for (size_t v = 0; v < s.weights.size(); v++) if (WeightFor(s, (int)v, gSel.bone) > 0.001f) verts++;
			ImGui::SameLine();
			ImGui::TextDisabled("(moves %d vertices)", verts);
			if (Bone(gSel.bone.c_str())) {
				V3& deg = gPose.manual[gSel.bone];
				ImGui::SliderFloat("Turn about X", &deg.x, -180, 180, "%.0f deg");
				ImGui::SliderFloat("Turn about Y", &deg.y, -180, 180, "%.0f deg");
				ImGui::SliderFloat("Turn about Z", &deg.z, -180, 180, "%.0f deg");
				Help("The bone turned about the body's own axes (X its side, Y its front, Z up, turning with it as it walks), on top of the animation and the pose; its children follow.");
				if (ImGui::Button("Reset this bone")) gPose.manual.erase(gSel.bone);
				ImGui::SameLine();
				if (ImGui::Button("Reset all bones")) gPose.manual.clear();
				ImGui::Checkbox("Flex test", &gPose.flex);
				Help("Swings the selected bone back and forth about an axis, to watch the weights around the joint stretch and fold.");
				if (gPose.flex) {
					ImGui::Combo("Axis", &gPose.flexAxis, kAxisNames, IM_ARRAYSIZE(kAxisNames));
					ImGui::SliderFloat("Swing", &gPose.flexDegrees, 5, 150, "%.0f deg");
					ImGui::SliderFloat("Speed##flex", &gPose.flexSpeed, 0.05f, 3, "%.2f Hz");
				}
			}
		}
	}

	if (gItem.loaded && ImGui::CollapsingHeader("Animation", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (ImGui::Combo("Motion", &gAnim.preset, kPresets, IM_ARRAYSIZE(kPresets))) {
			// The sneak presets crouch; leaving them stands up.
			bool sneakPreset = gAnim.preset == kSneak || gAnim.preset == kSneakWalk;
			if (sneakPreset != gAnim.sneak && (sneakPreset || gAnim.preset != kDrive)) { gAnim.sneak = sneakPreset; FindAnimations(); }
			if (gAnim.preset == kCustom && gAnim.customPath.empty()) AddLog(3, "pick an animation file below (the list, or Open .kf...)");
		}
		if (gAnim.preset == kDrive) ImGui::TextDisabled("W/S move, Q/E strafe, A/D turn, Shift runs, Space jumps, C sneaks");
		if (gItem.skeletonPath.empty()) {
			ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "No skeleton.nif found: the animations cannot play.");
		} else {
			bool rescan = ImGui::Combo("Stance", &gAnim.stance, kStances, IM_ARRAYSIZE(kStances));
			ImGui::SameLine();
			rescan |= ImGui::Checkbox("Female", &gAnim.female);
			ImGui::SameLine();
			rescan |= ImGui::Checkbox("Sneak", &gAnim.sneak);
			if (rescan) { FindAnimations(); gAnim.current = gAnim.previous = ClipPlay(); }
		}
		// What plays now: its time, which can be scrubbed (that pauses).
		if (gAnim.current.clip) {
			const kf::Clip* c = gAnim.current.clip;
			ImGui::TextDisabled("%s", fs::path(gAnim.current.path).filename().string().c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", gAnim.current.path.c_str());
			float t = gAnim.current.time - c->start;
			if (ImGui::SliderFloat("Time##clip", &t, 0, std::max(0.01f, c->Duration()), "%.2f s")) {
				gAnim.current.time = c->start + t;
				gAnim.previous = ClipPlay();
				gAnim.blend = 1;
				gAnim.paused = true;
			}
		}
		if (ImGui::Button(gAnim.paused ? "Play" : "Pause")) gAnim.paused = !gAnim.paused;
		ImGui::SameLine();
		if (ImGui::Button("Back to start")) { gMotion = Motion(); gAnim.current = gAnim.previous = ClipPlay(); }
		ImGui::SliderFloat("Speed", &gAnim.speedScale, 0.1f, 2.0f, "%.2f x");
		ImGui::SliderFloat("Time", &gAnim.timeScale, 0.05f, 1.0f, "%.2f x");
		Help("Slow motion: the body and its animations slow down (the stress pose's swing and the flex test keep their speed).");
		ImGui::Checkbox("Stay in place", &gAnim.inPlace);
		Help("The animations play without carrying the body away (handy with a close camera).");
		ImGui::SameLine();
		ImGui::Checkbox("Camera follows", &gCam.follow);
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

		ImGui::SeparatorText("Animation file");
		if (ImGui::Button("Open .kf...")) {
			std::string f = PickFile("Animations (*.kf)\0*.kf\0All files\0*.*\0", gAnim.customPath.empty() ? "" : fs::path(gAnim.customPath).parent_path().string());
			if (!f.empty()) PlayFile(f);
		}
		ImGui::SameLine();
		if (ImGui::Button(gAnimListed ? "Refresh list" : "List the game's animations")) ListAnimations();
		Help("Every .kf under meshes\\characters, loose in the Data folders and in the archives. Picking one plays it (the \"Animation file\" motion). You can also drop a .kf on the window.");
		if (gAnimListed) {
			static char animFilter[96] = "";
			ImGui::InputTextWithHint("##animfilter", "filter, e.g. mtidle, 2hraim, sneak", animFilter, sizeof animFilter);
			ImGui::BeginChild("anims", ImVec2(0, 160), ImGuiChildFlags_Borders);
			std::string af = Lower(animFilter);
			int shown = 0;
			for (auto& a : gAnimList) {
				if (!af.empty() && a.find(af) == std::string::npos) continue;
				if (++shown > 400) { ImGui::TextDisabled("... narrow the filter"); break; }
				bool playing = gAnim.preset == kCustom && gAnim.customRel == a;
				if (ImGui::Selectable(a.c_str() + 18, playing)) {   // without "meshes\characters\"
					std::string p = datafiles::Resolve(gItem.roots, a);
					if (p.empty()) AddLog(4, "cannot find " + a);
					else { PlayFile(p); gAnim.customRel = a; }
				}
			}
			ImGui::EndChild();
		}
	}

	if (gItem.loaded && ImGui::CollapsingHeader("Pose", ImGuiTreeNodeFlags_DefaultOpen)) {
		std::vector<const char*> names;
		for (auto& p : kStressPoses) names.push_back(p.name);
		ImGui::Combo("Stress pose", &gPose.stress, names.data(), (int)names.size());
		Help("Poses that push the joints the way armor has to follow: the limbs are aimed in the body's own directions, so they work whatever the mesh's bind pose (T or A), and on top of an animation.");
		ImGui::SliderFloat("Amount", &gPose.amount, 0, 1);
		ImGui::Checkbox("Swing in and out", &gPose.cycle);
		if (gPose.cycle) { ImGui::SameLine(); ImGui::SetNextItemWidth(120); ImGui::SliderFloat("##cycle", &gPose.cycleSpeed, 0.05f, 2, "%.2f Hz"); }
	}

	if (gItem.loaded && gSel.shape >= 0 && ImGui::CollapsingHeader("Picked vertex", ImGuiTreeNodeFlags_DefaultOpen)) {
		auto& s = gItem.shapes[gSel.shape];
		int v = gSel.vertex;
		ImGui::Text("%s, vertex %d", s.name.c_str(), v);
		uint8_t p = v < (int)s.problems.size() ? s.problems[v] : 0;
		for (int b = 0; b < 6; b++) if (p & (1 << b)) {
			D3DCOLOR c = kProblemColors[b];
			ImGui::TextColored(ImVec4(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1), "%s", kProblemNames[b]);
		}
		if (ImGui::BeginTable("vw", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("Bone", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Drawn");
			ImGui::TableSetupColumn("Skin data");
			ImGui::TableHeadersRow();
			std::map<int, std::pair<float, float>> rows;
			for (auto& [slot, w] : s.partWeights[v]) rows[slot].first = w;
			if (v < (int)s.dataWeights.size()) for (auto& [slot, w] : s.dataWeights[v]) rows[slot].second = w;
			float sumP = 0, sumD = 0;
			for (auto& [slot, w] : rows) {
				sumP += w.first;
				sumD += w.second;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				std::string name = slot < (int)s.slotNames.size() ? s.slotNames[slot] : "slot " + std::to_string(slot);
				if (ImGui::Selectable(name.c_str(), gSel.bone == name, ImGuiSelectableFlags_SpanAllColumns)) gSel.bone = name;
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", w.first);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", w.second);
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextDisabled("sum");
			ImGui::TableNextColumn();
			ImGui::Text("%.3f", sumP);
			ImGui::TableNextColumn();
			ImGui::Text("%.3f", sumD);
			ImGui::EndTable();
		}
		Help("Drawn: the weights in the skin partitions, which the game skins with (4 bones at most). Skin data: what the exporter wrote.");
	}

	if (ImGui::CollapsingHeader("Data sources")) {
		auto& r = gItem.roots;
		ImGui::TextWrapped("MO2: %s", r.mo2Instance.empty() ? "none found" : (r.mo2Instance + " (" + r.mo2Profile + ")").c_str());
		ImGui::TextWrapped("Game: %s", r.gameData.empty() ? "not found" : r.gameData.c_str());
		ImGui::TextWrapped("Skeleton: %s", gItem.skeletonPath.empty() ? "none (the mesh's bind pose)" : gItem.skeletonPath.c_str());
		bool changed = false;
		if (ImGui::Button("Set game folder...")) { std::string f = PickFolder(); if (!f.empty()) { gSources.gameFolder = f; changed = true; } }
		ImGui::SameLine();
		if (ImGui::Button("Set MO2 instance...")) { std::string f = PickFolder(); if (!f.empty()) { gSources.mo2Instance = f; changed = true; } }
		changed |= ImGui::Checkbox("Read BSA archives", &gSources.useArchives);
		if (ImGui::Button("Detect again")) { gSources = datafiles::Options(); changed = true; }
		if (changed) { SaveSources(); if (gItem.loaded) LoadItem(gItem.nifPath); }
	}

	if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
		for (auto& e : gLog) {
			ImVec4 c = e.level >= 4 ? ImVec4(1, 0.45f, 0.4f, 1) : e.level == 3 ? ImVec4(1, 0.8f, 0.4f, 1) : ImVec4(0.8f, 0.82f, 0.86f, 1);
			ImGui::PushStyleColor(ImGuiCol_Text, c);
			ImGui::TextWrapped("%s", e.text.c_str());
			ImGui::PopStyleColor();
		}
		if (gLogScroll) { ImGui::SetScrollHereY(1.0f); gLogScroll = false; }
		ImGui::EndChild();
	}
	ImGui::End();
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
		if (DragQueryFileA((HDROP)wParam, 0, file, MAX_PATH)) {
			std::string ext = Lower(fs::path(file).extension().string());
			if (ext == ".kf") { if (gItem.loaded) PlayFile(file); }
			else LoadItem(file);
		}
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
	gIniPath = (fs::path(exe).parent_path() / "NVRigPreviewer.ini").string();

	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIcon(nullptr, IDI_APPLICATION), LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"NVRigPreviewer", nullptr };
	RegisterClassExW(&wc);
	gWnd = CreateWindowW(wc.lpszClassName, L"NV Rig Previewer", WS_OVERLAPPEDWINDOW, 100, 60, 1600, 960, nullptr, nullptr, inst, nullptr);
	if (!CreateDevice()) {
		MessageBoxA(nullptr, "Could not create a Direct3D 9 device.", "NV Rig Previewer", MB_ICONERROR);
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
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	LoadSources();
	gD.referenceOn = IniGet("ShowReference", "0") == "1";
	AddLog(2, "NV Rig Previewer: open a skinned NIF to test its rigging and weights");

	// Command line: [mesh.nif] [--kf anim.kf] [--motion N] [--stance N] [--pose N] [--view N] [--bone "Bip01 L UpperArm"]
	// [--shot file.bmp --frames N] (writes one screenshot after N frames, then closes: for checking visuals).
	int argc = 0;
	LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) {
		char buf[1024];
		WideCharToMultiByte(CP_ACP, 0, argv[i], -1, buf, sizeof buf, nullptr, nullptr);
		args.push_back(buf);
	}
	LocalFree(argv);
	std::string start, kfArg, shot, boneArg;
	int shotFrames = 30;
	bool shotUi = false;
	for (size_t i = 0; i < args.size(); i++) {
		const std::string& a = args[i];
		auto next = [&]() { return i + 1 < args.size() ? args[++i] : std::string(); };
		if (a == "--kf") kfArg = next();
		else if (a == "--motion") gAnim.preset = std::clamp(atoi(next().c_str()), 0, IM_ARRAYSIZE(kPresets) - 1);
		else if (a == "--stance") gAnim.stance = std::clamp(atoi(next().c_str()), 0, IM_ARRAYSIZE(kStances) - 1);
		else if (a == "--pose") gPose.stress = std::clamp(atoi(next().c_str()), 0, (int)kStressPoses.size() - 1);
		else if (a == "--view") gD.view = std::clamp(atoi(next().c_str()), 0, IM_ARRAYSIZE(kViews) - 1);
		else if (a == "--bone") boneArg = next();
		else if (a == "--shot") shot = next();
		else if (a == "--ui") shotUi = true;
		else if (a == "--frames") shotFrames = std::max(1, atoi(next().c_str()));
		else if (a == "--yaw") gCam.yaw = (float)atof(next().c_str()) * 3.14159f / 180.0f;
		else if (a == "--pitch") gCam.pitch = (float)atof(next().c_str()) * 3.14159f / 180.0f;
		else if (a == "--dist") gCam.dist = (float)atof(next().c_str());
		else if (a.rfind("--", 0) != 0) start = a;
	}
	if (start.empty()) start = IniGet("LastFile");
	if (!start.empty() && fs::exists(start)) LoadItem(start);
	if (!kfArg.empty() && gItem.loaded) PlayFile(kfArg);
	if (!boneArg.empty()) gSel.bone = boneArg;
	if (gItem.loaded) FrameSelection();
	if (!boneArg.empty()) gCam.dist = std::max(gCam.dist, 120.0f);

	LARGE_INTEGER freq, last, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&last);
	float fps = 60, reloadCheck = 0;
	int frameNo = 0;
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
		fps += (1.0f / std::max(dt, 1e-4f) - fps) * 0.05f;

		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		reloadCheck += dt;
		if (gItem.loaded && gD.autoReload && reloadCheck > 0.5f) {
			reloadCheck = 0;
			if (TimeOf(gItem.nifPath) != gItem.nifTime) LoadItem(gItem.nifPath);
		}

		ApplyPose(dt);
		UpdateCamera();
		Panels(fps);

		RECT rc;
		GetClientRect(gWnd, &rc);
		float aspect = (float)std::max(1L, rc.right) / (float)std::max(1L, rc.bottom);
		M4 view = LookAtRH(CameraEye(), gCam.target, V3(0, 0, 1));
		M4 proj = PerspectiveRH(0.75f, aspect, 1.0f, 20000.0f);
		M4 viewProj = Mul(view, proj);
		Pick(viewProj);

		D3DCOLOR bg = D3DCOLOR_RGBA((int)(gD.background[0] * 255), (int)(gD.background[1] * 255), (int)(gD.background[2] * 255), 255);
		gDev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, bg, 1.0f, 0);
		if (SUCCEEDED(gDev->BeginScene())) {
			if (gItem.loaded) DrawScene(viewProj, view, proj);
			ImGui::EndFrame();
			ImGui::Render();
			if (!gShotPending && (shot.empty() || shotUi)) ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
			gDev->EndScene();
		} else ImGui::EndFrame();
		frameNo++;
		if (gShotPending) {
			gShotPending = false;
			char name[64];
			SYSTEMTIME st;
			GetLocalTime(&st);
			snprintf(name, sizeof name, "NVRigPreviewer %04d-%02d-%02d %02d%02d%02d.bmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
			fs::path dir = fs::path(gIniPath).parent_path() / "screenshots";
			fs::create_directories(dir);
			std::string p = (dir / name).string();
			if (SaveScreenshot(p)) AddLog(2, "screenshot: " + p);
		}
		if (!shot.empty() && frameNo == shotFrames) {
			SaveScreenshot(shot);
			done = true;
		}
		HRESULT hr = gDev->Present(nullptr, nullptr, nullptr, nullptr);
		if (hr == D3DERR_DEVICELOST && gDev->TestCooperativeLevel() == D3DERR_DEVICENOTRESET) ResetDevice();
	}

	ImGui_ImplDX9_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	return 0;
}
