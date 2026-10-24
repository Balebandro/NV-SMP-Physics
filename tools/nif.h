// Minimal NIF 20.2.0.7 (Fallout 3 / New Vegas, user version 11, BS version 34) reader/writer.
// Blocks are kept as raw bytes (the header stores every block's size); only the block types the
// cloth tool edits are parsed and re-serialised.
#pragma once
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>
#include <fstream>
#include <sstream>

namespace nif {

typedef std::vector<uint8_t> Bytes;

struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(Vec3 a) { float l = length(a); return l > 1e-12f ? a * (1.0f / l) : Vec3{ 0, 0, 1 }; }

// Row-major, column vectors: v' = M * v (Gamebryo convention).
struct Mat3 {
	float m[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
	Vec3 operator*(Vec3 v) const { return { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z, m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z }; }
	Mat3 operator*(const Mat3& b) const { Mat3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { r.m[i][j] = 0; for (int k = 0; k < 3; k++) r.m[i][j] += m[i][k] * b.m[k][j]; } return r; }
	Mat3 transposed() const { Mat3 r; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r.m[i][j] = m[j][i]; return r; }
};

struct Transform {
	Mat3 rot; Vec3 trans; float scale = 1.0f;
	Vec3 apply(Vec3 v) const { return rot * (v * scale) + trans; }
	Transform operator*(const Transform& b) const { Transform r; r.rot = rot * b.rot; r.scale = scale * b.scale; r.trans = rot * (b.trans * scale) + trans; return r; }
	Transform inverse() const { Transform r; r.rot = rot.transposed(); r.scale = 1.0f / scale; r.trans = (r.rot * trans) * -r.scale; return r; }
};

// ---------------------------------------------------------------------------------------------
struct Reader {
	const uint8_t* p; size_t n; size_t pos = 0;
	Reader(const uint8_t* d, size_t s) : p(d), n(s) {}
	void need(size_t k) { if (pos + k > n) throw std::runtime_error("read past end of block"); }
	template<class T> T get() { need(sizeof(T)); T v; memcpy(&v, p + pos, sizeof(T)); pos += sizeof(T); return v; }
	uint8_t u8() { return get<uint8_t>(); } uint16_t u16() { return get<uint16_t>(); } uint32_t u32() { return get<uint32_t>(); }
	int32_t i32() { return get<int32_t>(); } float f32() { return get<float>(); }
	Vec3 v3() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
	Mat3 m33() { Mat3 m; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) m.m[i][j] = f32(); return m; }
	Transform xf() { Transform t; t.rot = m33(); t.trans = v3(); t.scale = f32(); return t; }   // NiTransform: rot, trans, scale
	void skip(size_t k) { need(k); pos += k; }
	bool done() const { return pos == n; }
};

struct Writer {
	Bytes b;
	template<class T> void put(T v) { size_t o = b.size(); b.resize(o + sizeof(T)); memcpy(&b[o], &v, sizeof(T)); }
	void u8(uint8_t v) { put(v); } void u16(uint16_t v) { put(v); } void u32(uint32_t v) { put(v); } void i32(int32_t v) { put(v); } void f32(float v) { put(v); }
	void v3(Vec3 v) { f32(v.x); f32(v.y); f32(v.z); }
	void m33(const Mat3& m) { for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) f32(m.m[i][j]); }
	void xf(const Transform& t) { m33(t.rot); v3(t.trans); f32(t.scale); }
	void raw(const uint8_t* d, size_t k) { b.insert(b.end(), d, d + k); }
};

// ---------------------------------------------------------------------------------------------
struct File {
	std::string headerString;
	uint32_t version = 0, userVersion = 0, bsVersion = 0;
	uint8_t endian = 1;
	std::string author, processScript, exportScript;
	std::vector<std::string> blockTypes;
	std::vector<uint16_t> blockTypeIndex;
	std::vector<Bytes> blocks;
	std::vector<std::string> strings;
	uint32_t maxStringLength = 0;
	std::vector<uint32_t> groups;
	std::vector<int32_t> roots;

	const std::string& typeOf(size_t i) const { return blockTypes[blockTypeIndex[i] & 0x7FFF]; }

	uint32_t addString(const std::string& s) {
		for (size_t i = 0; i < strings.size(); i++) if (strings[i] == s) return (uint32_t)i;
		strings.push_back(s); return (uint32_t)strings.size() - 1;
	}
	int addBlock(const std::string& type, Bytes data) {
		size_t t = 0;
		for (; t < blockTypes.size(); t++) if (blockTypes[t] == type) break;
		if (t == blockTypes.size()) blockTypes.push_back(type);
		blockTypeIndex.push_back((uint16_t)t);
		blocks.push_back(std::move(data));
		return (int)blocks.size() - 1;
	}
	void setType(size_t i, const std::string& type) {
		size_t t = 0;
		for (; t < blockTypes.size(); t++) if (blockTypes[t] == type) break;
		if (t == blockTypes.size()) blockTypes.push_back(type);
		blockTypeIndex[i] = (uint16_t)t;
	}

	static std::string shortString(Reader& r) { uint8_t l = r.u8(); std::string s((const char*)r.p + r.pos, l); r.skip(l); if (!s.empty() && s.back() == 0) s.pop_back(); return s; }
	static std::string sizedString(Reader& r) { uint32_t l = r.u32(); std::string s((const char*)r.p + r.pos, l); r.skip(l); return s; }
	static void putShortString(Writer& w, const std::string& s) { w.u8((uint8_t)(s.size() + 1)); w.raw((const uint8_t*)s.c_str(), s.size() + 1); }
	static void putSizedString(Writer& w, const std::string& s) { w.u32((uint32_t)s.size()); w.raw((const uint8_t*)s.data(), s.size()); }

	void load(const std::string& path) {
		std::ifstream f(path, std::ios::binary);
		if (!f) throw std::runtime_error("cannot open " + path);
		Bytes d((std::istreambuf_iterator<char>(f)), {});
		Reader r(d.data(), d.size());
		size_t nl = 0; while (nl < d.size() && d[nl] != '\n') nl++;
		headerString.assign((const char*)d.data(), nl); r.pos = nl + 1;
		version = r.u32(); endian = r.u8(); userVersion = r.u32();
		uint32_t numBlocks = r.u32(); bsVersion = r.u32();
		if (version != 0x14020007 || userVersion != 11) throw std::runtime_error("not a Fallout 3/NV NIF (20.2.0.7, user 11)");
		author = shortString(r); processScript = shortString(r); exportScript = shortString(r);
		uint16_t nTypes = r.u16(); blockTypes.resize(nTypes); for (auto& t : blockTypes) t = sizedString(r);
		blockTypeIndex.resize(numBlocks); for (auto& t : blockTypeIndex) t = r.u16();
		std::vector<uint32_t> sizes(numBlocks); for (auto& s : sizes) s = r.u32();
		uint32_t nStrings = r.u32(); maxStringLength = r.u32(); strings.resize(nStrings); for (auto& s : strings) s = sizedString(r);
		uint32_t nGroups = r.u32(); groups.resize(nGroups); for (auto& g : groups) g = r.u32();
		blocks.resize(numBlocks);
		for (uint32_t i = 0; i < numBlocks; i++) { r.need(sizes[i]); blocks[i].assign(d.data() + r.pos, d.data() + r.pos + sizes[i]); r.pos += sizes[i]; }
		uint32_t nRoots = r.u32(); roots.resize(nRoots); for (auto& x : roots) x = r.i32();
	}

	void save(const std::string& path) const {
		Writer w;
		w.raw((const uint8_t*)headerString.data(), headerString.size()); w.u8('\n');
		w.u32(version); w.u8(endian); w.u32(userVersion); w.u32((uint32_t)blocks.size()); w.u32(bsVersion);
		putShortString(w, author); putShortString(w, processScript); putShortString(w, exportScript);
		w.u16((uint16_t)blockTypes.size()); for (auto& t : blockTypes) putSizedString(w, t);
		for (auto t : blockTypeIndex) w.u16(t);
		for (auto& b : blocks) w.u32((uint32_t)b.size());
		uint32_t maxLen = 0; for (auto& s : strings) maxLen = std::max<uint32_t>(maxLen, (uint32_t)s.size());
		w.u32((uint32_t)strings.size()); w.u32(maxLen); for (auto& s : strings) putSizedString(w, s);
		w.u32((uint32_t)groups.size()); for (auto g : groups) w.u32(g);
		for (auto& b : blocks) w.raw(b.data(), b.size());
		w.u32((uint32_t)roots.size()); for (auto x : roots) w.i32(x);
		std::ofstream f(path, std::ios::binary);
		if (!f) throw std::runtime_error("cannot write " + path);
		f.write((const char*)w.b.data(), w.b.size());
	}
};

// ---------------------------------------------------------------------------------------------
// NiObjectNET + NiAVObject (+ NiNode) for BS version 34.
struct AVObject {
	uint32_t name = 0xFFFFFFFF;
	std::vector<int32_t> extraData;
	int32_t controller = -1;
	uint32_t flags = 14;
	Transform local;
	std::vector<int32_t> properties;
	int32_t collision = -1;
	// NiNode
	bool isNode = false;
	std::vector<int32_t> children, effects;
	size_t tailPos = 0;   // where the subclass data starts

	void read(Reader& r, bool node) {
		name = r.u32();
		extraData.resize(r.u32()); for (auto& e : extraData) e = r.i32();
		controller = r.i32();
		flags = r.u32();
		local.trans = r.v3(); local.rot = r.m33(); local.scale = r.f32();
		properties.resize(r.u32()); for (auto& p : properties) p = r.i32();
		collision = r.i32();
		isNode = node;
		if (node) {
			children.resize(r.u32()); for (auto& c : children) c = r.i32();
			effects.resize(r.u32()); for (auto& e : effects) e = r.i32();
		}
		tailPos = r.pos;
	}
	void write(Writer& w) const {
		w.u32(name);
		w.u32((uint32_t)extraData.size()); for (auto e : extraData) w.i32(e);
		w.i32(controller); w.u32(flags);
		w.v3(local.trans); w.m33(local.rot); w.f32(local.scale);
		w.u32((uint32_t)properties.size()); for (auto p : properties) w.i32(p);
		w.i32(collision);
		if (isNode) {
			w.u32((uint32_t)children.size()); for (auto c : children) w.i32(c);
			w.u32((uint32_t)effects.size()); for (auto e : effects) w.i32(e);
		}
	}
};

// NiTriShape / NiTriStrips: NiAVObject + geometry refs; the rest is kept raw.
struct Geometry {
	AVObject av;
	int32_t data = -1, skin = -1;
	Bytes rest;
	void read(const Bytes& b) {
		Reader r(b.data(), b.size());
		av.read(r, false);
		data = r.i32(); skin = r.i32();
		rest.assign(b.begin() + r.pos, b.end());
	}
	Bytes write() const { Writer w; av.write(w); w.i32(data); w.i32(skin); w.raw(rest.data(), rest.size()); return w.b; }
};

struct Triangle { uint16_t v[3]; };

// NiTriShapeData / NiTriStripsData, read only.
struct GeometryData {
	std::vector<Vec3> vertices, normals;
	std::vector<float> uvs;   // u, v per vertex (the first set), empty if none
	std::vector<Triangle> triangles;
	void read(const Bytes& b, bool strips) {
		Reader r(b.data(), b.size());
		r.i32();   // group id
		uint16_t nv = r.u16();
		r.u8(); r.u8();   // keep, compress flags
		if (r.u8()) { vertices.resize(nv); for (auto& v : vertices) v = r.v3(); }
		uint16_t bsFlags = r.u16();
		if (r.u8()) {
			normals.resize(nv); for (auto& v : normals) v = r.v3();
			if (bsFlags & 4096) r.skip(nv * 24);   // tangents, bitangents
		}
		r.v3(); r.f32();   // bound
		if (r.u8()) r.skip(nv * 16);   // vertex colours
		if (bsFlags & 1) { uvs.resize(nv * 2); for (auto& x : uvs) x = r.f32(); }   // uv set
		r.u16();   // consistency
		r.i32();   // additional data
		if (!strips) {
			uint16_t nt = r.u16(); r.u32();
			if (r.u8()) { triangles.resize(nt); for (auto& t : triangles) { t.v[0] = r.u16(); t.v[1] = r.u16(); t.v[2] = r.u16(); } }
		}
		else {
			r.u16();
			uint16_t ns = r.u16(); std::vector<uint16_t> lens(ns); for (auto& l : lens) l = r.u16();
			if (r.u8()) {
				for (auto l : lens) {
					std::vector<uint16_t> s(l); for (auto& x : s) x = r.u16();
					for (int i = 0; i + 2 < (int)l; i++) {
						Triangle t = (i & 1) ? Triangle{ { s[i], s[i + 2], s[i + 1] } } : Triangle{ { s[i], s[i + 1], s[i + 2] } };
						if (t.v[0] != t.v[1] && t.v[1] != t.v[2] && t.v[0] != t.v[2]) triangles.push_back(t);
					}
				}
			}
		}
	}
};

// The game draws a skinned NiTriStrips from its skin partition's strips: one partitioned into triangles (as the tools
// here partition) draws nothing. Such a shape becomes an NiTriShape: the same data, its strips as a triangle list (the
// data's other fields are laid out alike up to the indices). False if the shape is not strips.
inline bool stripsToShape(File& f, int shapeBlock) {
	Geometry g;
	g.read(f.blocks[shapeBlock]);
	if (g.data < 0 || f.typeOf(g.data) != "NiTriStripsData") return false;
	const Bytes& b = f.blocks[g.data];
	GeometryData gd;
	gd.read(b, true);
	Reader r(b.data(), b.size());
	r.i32();
	uint16_t nv = r.u16();
	r.u8(); r.u8();
	if (r.u8()) r.skip(nv * 12);
	uint16_t bsFlags = r.u16();
	if (r.u8()) { r.skip(nv * 12); if (bsFlags & 4096) r.skip(nv * 24); }
	r.skip(16);
	if (r.u8()) r.skip(nv * 16);
	r.skip((bsFlags & 63) * nv * 8);   // uv sets
	r.u16(); r.i32();                  // consistency, additional data
	Writer w;
	w.raw(b.data(), r.pos);
	w.u16((uint16_t)gd.triangles.size()); w.u32((uint32_t)gd.triangles.size() * 3);
	w.u8(1); for (auto& t : gd.triangles) { w.u16(t.v[0]); w.u16(t.v[1]); w.u16(t.v[2]); }
	w.u16(0);   // match groups
	f.blocks[g.data] = w.b;
	f.setType(g.data, "NiTriShapeData");
	f.setType(shapeBlock, "NiTriShape");
	return true;
}

// NiSkinInstance / BSDismemberSkinInstance.
struct SkinInstance {
	bool dismember = false;
	int32_t data = -1, partition = -1, skeletonRoot = -1;
	std::vector<int32_t> bones;
	struct Part { uint16_t flags, bodyPart; };
	std::vector<Part> parts;
	void read(const Bytes& b, bool dism) {
		Reader r(b.data(), b.size());
		dismember = dism;
		data = r.i32(); partition = r.i32(); skeletonRoot = r.i32();
		bones.resize(r.u32()); for (auto& x : bones) x = r.i32();
		if (dism) { parts.resize(r.u32()); for (auto& p : parts) { p.flags = r.u16(); p.bodyPart = r.u16(); } }
		if (!r.done()) throw std::runtime_error("skin instance: unexpected trailing data");
	}
	Bytes write() const {
		Writer w; w.i32(data); w.i32(partition); w.i32(skeletonRoot);
		w.u32((uint32_t)bones.size()); for (auto x : bones) w.i32(x);
		if (dismember) { w.u32((uint32_t)parts.size()); for (auto& p : parts) { w.u16(p.flags); w.u16(p.bodyPart); } }
		return w.b;
	}
};

struct SkinData {
	Transform skinTransform;
	uint8_t hasWeights = 1;
	struct VW { uint16_t index; float weight; };
	struct Bone { Transform skinToBone; Vec3 center; float radius = 0; std::vector<VW> weights; };
	std::vector<Bone> bones;
	void read(const Bytes& b) {
		Reader r(b.data(), b.size());
		skinTransform = r.xf();
		bones.resize(r.u32());
		hasWeights = r.u8();
		for (auto& bn : bones) {
			bn.skinToBone = r.xf(); bn.center = r.v3(); bn.radius = r.f32();
			uint16_t n = r.u16();
			if (hasWeights) { bn.weights.resize(n); for (auto& vw : bn.weights) { vw.index = r.u16(); vw.weight = r.f32(); } }
		}
		if (!r.done()) throw std::runtime_error("skin data: unexpected trailing data");
	}
	Bytes write() const {
		Writer w; w.xf(skinTransform); w.u32((uint32_t)bones.size()); w.u8(hasWeights);
		for (auto& bn : bones) {
			w.xf(bn.skinToBone); w.v3(bn.center); w.f32(bn.radius);
			w.u16((uint16_t)bn.weights.size());
			if (hasWeights) for (auto& vw : bn.weights) { w.u16(vw.index); w.f32(vw.weight); }
		}
		return w.b;
	}
};

struct SkinPartition {
	struct Part {
		uint16_t numWeightsPerVertex = 4;
		uint16_t numVertices = 0;             // as read (the vertex map may be absent: then the partition's vertices are the shape's, in order)
		std::vector<uint16_t> bones;
		std::vector<uint16_t> vertexMap;
		std::vector<float> weights;           // numVertices * numWeightsPerVertex
		std::vector<std::vector<uint16_t>> strips;
		std::vector<Triangle> triangles;
		std::vector<uint8_t> boneIndices;     // numVertices * numWeightsPerVertex
		uint8_t hasVertexMap = 1, hasVertexWeights = 1, hasFaces = 1, hasBoneIndices = 1;
	};
	std::vector<Part> parts;
	void read(const Bytes& b) {
		Reader r(b.data(), b.size());
		parts.resize(r.u32());
		for (auto& p : parts) {
			uint16_t nv = r.u16(), nt = r.u16(), nb = r.u16(), ns = r.u16();
			p.numWeightsPerVertex = r.u16();
			p.numVertices = nv;
			p.bones.resize(nb); for (auto& x : p.bones) x = r.u16();
			p.hasVertexMap = r.u8(); if (p.hasVertexMap) { p.vertexMap.resize(nv); for (auto& x : p.vertexMap) x = r.u16(); }
			p.hasVertexWeights = r.u8(); if (p.hasVertexWeights) { p.weights.resize(nv * p.numWeightsPerVertex); for (auto& x : p.weights) x = r.f32(); }
			std::vector<uint16_t> lens(ns); for (auto& l : lens) l = r.u16();
			p.hasFaces = r.u8();
			if (p.hasFaces) {
				if (ns) { p.strips.resize(ns); for (int i = 0; i < ns; i++) { p.strips[i].resize(lens[i]); for (auto& x : p.strips[i]) x = r.u16(); } }
				else { p.triangles.resize(nt); for (auto& t : p.triangles) { t.v[0] = r.u16(); t.v[1] = r.u16(); t.v[2] = r.u16(); } }
			}
			p.hasBoneIndices = r.u8(); if (p.hasBoneIndices) { p.boneIndices.resize(nv * p.numWeightsPerVertex); for (auto& x : p.boneIndices) x = r.u8(); }
		}
		if (!r.done()) throw std::runtime_error("skin partition: unexpected trailing data");
	}
	Bytes write() const {
		Writer w; w.u32((uint32_t)parts.size());
		for (auto& p : parts) {
			uint16_t nv = (uint16_t)p.vertexMap.size();
			w.u16(nv); w.u16((uint16_t)p.triangles.size()); w.u16((uint16_t)p.bones.size()); w.u16((uint16_t)p.strips.size()); w.u16(p.numWeightsPerVertex);
			for (auto x : p.bones) w.u16(x);
			w.u8(1); for (auto x : p.vertexMap) w.u16(x);
			w.u8(1); for (auto x : p.weights) w.f32(x);
			for (auto& s : p.strips) w.u16((uint16_t)s.size());
			w.u8(1);
			if (p.strips.size()) { for (auto& s : p.strips) for (auto x : s) w.u16(x); }
			else for (auto& t : p.triangles) { w.u16(t.v[0]); w.u16(t.v[1]); w.u16(t.v[2]); }
			w.u8(1); for (auto x : p.boneIndices) w.u8(x);
		}
		return w.b;
	}
};

inline bool isNodeType(const std::string& t) { return t == "NiNode" || t == "BSFadeNode" || t == "BSOrderedNode" || t == "BSValueNode" || t == "BSMultiBoundNode" || t == "NiBillboardNode" || t == "NiSwitchNode" || t == "NiLODNode"; }

} // namespace nif
