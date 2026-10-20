#pragma once
// Fallout New Vegas 1.4.0.525 scene graph, as FSMP's glue sees Skyrim's: the NetImmerse objects SMP
// reads and writes, laid out as the game has them in memory (offsets from the decompiled
// FalloutNV.exe and NVR's GameNi.h, checked with static_assert), and the helpers FSMP's
// NetImmerseUtils.h provides. The object types live in namespace RE, as in FSMP, so the ported
// glue reads the same.
//
// Nothing here calls into the engine except the node constructor (CreateNode) and the reference
// count release: world transforms are composed here in plain math, as the engine does it.
#include "hdtSkinnedMesh/hdtBulletHelper.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace RE
{
	struct NiPoint3
	{
		float x = 0, y = 0, z = 0;

		NiPoint3() = default;
		NiPoint3(float a, float b, float c) : x(a), y(b), z(c) {}
		NiPoint3 operator+(const NiPoint3& o) const { return { x + o.x, y + o.y, z + o.z }; }
		NiPoint3 operator-(const NiPoint3& o) const { return { x - o.x, y - o.y, z - o.z }; }
		NiPoint3 operator*(float s) const { return { x * s, y * s, z * s }; }
		float Length() const { return std::sqrt(x * x + y * y + z * z); }
	};

	// Row-major, entry[row][column], as Gamebryo (and Skyrim's NiMatrix3) store it.
	struct NiMatrix3
	{
		float entry[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };

		NiPoint3 operator*(const NiPoint3& v) const
		{
			return { entry[0][0] * v.x + entry[0][1] * v.y + entry[0][2] * v.z,
				entry[1][0] * v.x + entry[1][1] * v.y + entry[1][2] * v.z,
				entry[2][0] * v.x + entry[2][1] * v.y + entry[2][2] * v.z };
		}
		NiMatrix3 operator*(const NiMatrix3& o) const
		{
			NiMatrix3 r;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					r.entry[i][j] = entry[i][0] * o.entry[0][j] + entry[i][1] * o.entry[1][j] + entry[i][2] * o.entry[2][j];
			return r;
		}
		NiMatrix3 Transpose() const
		{
			NiMatrix3 r;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					r.entry[i][j] = entry[j][i];
			return r;
		}
	};

	struct NiTransform
	{
		NiMatrix3 rotate;
		NiPoint3 translate;
		float scale = 1.0f;

		// this * rhs: rhs's frame expressed in this one's parent (world = parent.world * local).
		NiTransform operator*(const NiTransform& rhs) const
		{
			NiTransform r;
			r.rotate = rotate * rhs.rotate;
			r.translate = translate + rotate * rhs.translate * scale;
			r.scale = scale * rhs.scale;
			return r;
		}
		NiPoint3 operator*(const NiPoint3& p) const { return translate + rotate * p * scale; }
		NiTransform Invert() const
		{
			NiTransform r;
			r.rotate = rotate.Transpose();
			r.scale = scale != 0.0f ? 1.0f / scale : 1.0f;
			r.translate = r.rotate * translate * -r.scale;
			return r;
		}
	};
	static_assert(sizeof(NiTransform) == 0x34);

	struct NiBound
	{
		NiPoint3 center;
		float radius;
	};

	// NiRefObject / NiObject: vtable, reference count.
	class NiObject
	{
	public:
		void** vtbl;              // 00
		volatile long refCount;   // 04

		void IncRefCount() { InterlockedIncrement(&refCount); }
		void DecRefCount()
		{
			if (InterlockedDecrement(&refCount) == 0) {
				typedef void(__thiscall * DeleteThis_t)(NiObject*);
				((DeleteThis_t)vtbl[1])(this);   // NiRefObject::DeleteThis
			}
		}
		// NiObject::IsNode (vtable slot 3) and IsGeometry (slot 6): the object cast to that type, or null.
		void* IsNode() { typedef void*(__thiscall * F)(NiObject*); return ((F)vtbl[3])(this); }
		void* IsGeometry() { typedef void*(__thiscall * F)(NiObject*); return ((F)vtbl[6])(this); }
	};

	class NiExtraData : public NiObject
	{
	public:
		const char* name;   // 08
	};

	class NiStringExtraData : public NiExtraData
	{
	public:
		const char* value;   // 0C
	};

	class NiObjectNET : public NiObject
	{
	public:
		const char* m_name;          // 08 NiFixedString
		void* controller;            // 0C
		NiExtraData** extraData;     // 10
		uint16_t extraDataCount;     // 14
		uint16_t extraDataCapacity;  // 16

		const char* GetName() const { return m_name ? m_name : ""; }
		const char* FindStringExtra(const char* key) const
		{
			if (!extraData)
				return nullptr;
			for (uint16_t i = 0; i < extraDataCount; ++i) {
				auto* e = extraData[i];
				if (e && e->name && _stricmp(e->name, key) == 0)
					return static_cast<NiStringExtraData*>(e)->value;
			}
			return nullptr;
		}
		// "SMP Bones" may go on in "SMP Bones 2", "SMP Bones 3"... (Outfit Studio cannot read a NIF string of 2048
		// characters or more, so long lists are split): the whole list, or empty when there is none.
		std::string SmpBonesList() const
		{
			std::string list;
			if (const char* first = FindStringExtra("SMP Bones")) list = first;
			for (int i = 2; !list.empty(); ++i) {
				const char* more = FindStringExtra(("SMP Bones " + std::to_string(i)).c_str());
				if (!more) break;
				list += more;
			}
			return list;
		}
	};

	class NiNode;

	class NiAVObject : public NiObjectNET
	{
	public:
		NiNode* parent;               // 18
		uint8_t pad1C[0x34 - 0x1C];   // collision object, world bound, properties, flags
		NiTransform local;            // 34
		NiTransform world;            // 68

		NiNode* AsNode() { return static_cast<NiNode*>(IsNode()); }
	};
	static_assert(offsetof(NiAVObject, parent) == 0x18);
	static_assert(offsetof(NiAVObject, local) == 0x34);
	static_assert(offsetof(NiAVObject, world) == 0x68);
	static_assert(sizeof(NiAVObject) == 0x9C);

	class NiNode : public NiAVObject
	{
	public:
		void* childrenVtbl;        // 9C NiTObjectArray<NiAVObjectPtr>
		NiAVObject** children;     // A0
		uint16_t childCapacity;    // A4
		uint16_t childFirstFree;   // A6 one past the last used slot
		uint16_t childCount;       // A8
		uint16_t childGrowBy;      // AA

		uint16_t ChildEnd() const { return children ? childFirstFree : 0; }
	};
	static_assert(offsetof(NiNode, children) == 0xA0);
	static_assert(sizeof(NiNode) == 0xAC);

	// Skinning.
	class NiSkinData : public NiObject
	{
	public:
		struct BoneVertData
		{
			uint16_t vertex;   // 00
			float weight;      // 04
		};
		struct BoneData
		{
			NiTransform skinToBone;   // 00
			NiBound bound;            // 34
			BoneVertData* vertData;   // 44
			uint16_t vertCount;       // 48
			uint16_t pad4A;
		};
		static_assert(sizeof(BoneData) == 0x4C);

		void* skinPartition;          // 08
		NiTransform rootParentToSkin; // 0C
		BoneData* boneData;           // 40
		uint32_t boneCount;           // 44
	};
	static_assert(offsetof(NiSkinData, boneData) == 0x40);

	class NiSkinInstance : public NiObject
	{
	public:
		NiSkinData* skinData;      // 08
		void* skinPartition;       // 0C
		NiAVObject* rootParent;    // 10
		NiAVObject** bones;        // 14
		uint32_t frameID;          // 18
		uint32_t boneCount;        // 1C
	};
	static_assert(offsetof(NiSkinInstance, boneCount) == 0x1C);

	class NiGeometryData : public NiObject
	{
	public:
		uint16_t vertexCount;   // 08
		uint16_t id;            // 0A
		uint16_t dataFlags;     // 0C
		uint16_t dirtyFlags;    // 0E
		NiBound bound;          // 10
		NiPoint3* vertices;     // 20
		NiPoint3* normals;      // 24
		void* colors;           // 28
		void* uvs;              // 2C
		void* additional;       // 30
		void* buffData;         // 34
		uint8_t keepFlags;      // 38
		uint8_t compressFlags;  // 39
		uint8_t pad3A[6];
	};
	static_assert(sizeof(NiGeometryData) == 0x40);

	// NiTriBasedGeomData (triangle count) and NiTriShapeData (index list).
	class NiTriShapeData : public NiGeometryData
	{
	public:
		uint16_t triangleCount;        // 40
		uint16_t activeTriangleCount;  // 42
		uint32_t triListLength;        // 44 index count
		uint16_t* triList;             // 48
	};
	static_assert(offsetof(NiTriShapeData, triList) == 0x48);

	class NiGeometry : public NiAVObject
	{
	public:
		uint8_t propertyState[0xB8 - 0x9C];   // 9C
		NiGeometryData* data;                 // B8
		NiSkinInstance* skinInstance;         // BC
		void* shader;                         // C0
	};
	static_assert(offsetof(NiGeometry, data) == 0xB8);
	static_assert(offsetof(NiGeometry, skinInstance) == 0xBC);

	// Intrusive pointer on the game's reference count.
	template <class T>
	class NiPointer
	{
	public:
		NiPointer() = default;
		NiPointer(T* p) : m_ptr(p) { if (m_ptr) m_ptr->IncRefCount(); }
		NiPointer(const NiPointer& o) : NiPointer(o.m_ptr) {}
		NiPointer(NiPointer&& o) noexcept : m_ptr(std::exchange(o.m_ptr, nullptr)) {}
		~NiPointer() { if (m_ptr) m_ptr->DecRefCount(); }
		NiPointer& operator=(const NiPointer& o) { reset(o.m_ptr); return *this; }
		NiPointer& operator=(NiPointer&& o) noexcept { if (this != &o) { if (m_ptr) m_ptr->DecRefCount(); m_ptr = std::exchange(o.m_ptr, nullptr); } return *this; }
		NiPointer& operator=(T* p) { reset(p); return *this; }

		void reset(T* p = nullptr)
		{
			if (p) p->IncRefCount();
			if (m_ptr) m_ptr->DecRefCount();
			m_ptr = p;
		}
		T* get() const { return m_ptr; }
		T* operator->() const { return m_ptr; }
		T& operator*() const { return *m_ptr; }
		explicit operator bool() const { return m_ptr != nullptr; }
		bool operator==(const T* p) const { return m_ptr == p; }
		bool operator!=(const T* p) const { return m_ptr != p; }
		bool operator==(const void* p) const { return m_ptr == p; }
		bool operator!=(const void* p) const { return m_ptr != p; }
		bool operator==(const NiPointer& o) const { return m_ptr == o.m_ptr; }
		bool operator!=(const NiPointer& o) const { return m_ptr != o.m_ptr; }

	private:
		T* m_ptr = nullptr;
	};
}

namespace hdt
{
	template <class T>
	inline RE::NiPointer<T> make_nismart(T* p) { return RE::NiPointer<T>(p); }

	inline RE::NiNode* castNiNode(RE::NiAVObject* obj) { return obj ? obj->AsNode() : nullptr; }
	inline RE::NiGeometry* castNiGeometry(RE::NiAVObject* obj) { return obj ? static_cast<RE::NiGeometry*>(obj->IsGeometry()) : nullptr; }

	// The first object named `name` under `obj`, obj included (case-insensitive, as the game's own lookup).
	inline RE::NiAVObject* findObject(RE::NiAVObject* obj, const char* name, int depth = 0)
	{
		if (!obj || depth > 64)
			return nullptr;
		if (obj->m_name && _stricmp(obj->m_name, name) == 0)
			return obj;
		if (auto* node = castNiNode(obj)) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				if (auto* found = findObject(node->children[i], name, depth + 1))
					return found;
		}
		return nullptr;
	}

	inline RE::NiNode* findNode(RE::NiNode* obj, const char* name)
	{
		return castNiNode(findObject(obj, name));
	}

	// World transforms of obj and everything under it from their local ones, as NiAVObject::UpdateWorldData.
	inline void updateTransformUpDown(RE::NiAVObject* obj, bool = false, int depth = 0)
	{
		if (!obj || depth > 64)
			return;
		if (obj->parent)
			obj->world = obj->parent->world * obj->local;
		if (auto* node = castNiNode(obj)) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				updateTransformUpDown(node->children[i], false, depth + 1);
		}
	}

	// A new, unparented NiNode named `name`, with one reference held by the caller (NiPointer).
	RE::NiNode* createNode(const char* name);

	// A file from the game's Data folder: loose (through Mod Organizer's virtual file system) or in a BSA.
	std::string readAllFile(const char* path);
}
