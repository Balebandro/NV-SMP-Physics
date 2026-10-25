// Ported from FSMP (hdtSMP64, src/hdtSkyrimSystem.cpp), GPL-3.0 with FSMP's modding exception.
// The XML grammar, the templates and the bone, shape and constraint readers are FSMP's, unchanged.
// New Vegas specific:
//   - skin and mesh data come from NiGeometry / NiSkinInstance / NiSkinData / NiSkinPartition
//     (Gamebryo 2.x: partition-local vertex indices through a vertex map, strips or lists)
//   - bones the item brings that the skeleton lacks are created as nodes here (createExtraBoneNode)
//   - prepareForRead resets on teleports and fast turns instead of Skyrim's camera rules
//   - no <pattern> expansion and no Havok reference pose (Skyrim-only features)
#include "hdtSkyrimSystem.h"
#include "hdtSkinnedMesh/hdtSkinnedMeshShape.h"

#include "XmlReader.h"
#include "hdtSkyrimPhysicsWorld.h"

#include <cctype>
#include <map>
#include <sstream>
#include <unordered_set>

namespace hdt
{
	static constexpr float PI = 3.1415926535897932384626433832795f;

	btEmptyShape SkyrimSystemCreator::BoneTemplate::emptyShape[1];

	// See SkyrimSystem::isRetiredNode. Never released (a few KB per unequip).
	static std::vector<RE::NiPointer<RE::NiAVObject>> g_retiredNodes;
	static std::unordered_set<const RE::NiAVObject*> g_retiredSet;

	// NiSkinPartition (Gamebryo 2.x) as the game keeps it: the partitions the renderer skins with.
	struct NiSkinPartitionNV
	{
		void** vtbl;          // 00
		volatile long ref;    // 04
		uint32_t count;       // 08
		uint8_t* partitions;  // 0C array of kPartitionSize records
	};
	// One partition (Gamebryo's NiSkinPartition::Partition, NVR's GameNi.h for the counts):
	//   00 vtable, 04 bones (partition bone -> skin-instance bone), 08 weights [vertex][bonesPerVertex],
	//   0C vertex map (partition vertex -> shape vertex), 10 bone palette [vertex][bonesPerVertex]
	//   (-> partition bone; null: k), 14 triangle list or strips, 18 strip lengths, 1C vertices,
	//   1E triangles, 20 bones, 22 strips, 24 bones per vertex, 28 buffer data.
	static constexpr size_t kPartitionSize = 0x2C;
	static inline uint16_t PartU16(const uint8_t* p, size_t off) { return *(const uint16_t*)(p + off); }
	template <class T> static inline T* PartPtr(const uint8_t* p, size_t off) { return *(T**)(p + off); }

	static NiSkinPartitionNV* SkinPartition(RE::NiSkinInstance* skin)
	{
		auto* p = static_cast<NiSkinPartitionNV*>(skin ? skin->skinPartition : nullptr);
		if (!p && skin && skin->skinData)
			p = static_cast<NiSkinPartitionNV*>(skin->skinData->skinPartition);
		return p;
	}

	// The shape's triangles in its own vertex indices, from the skin partitions (lists or strips),
	// or from NiTriShapeData if there are none.
	static void CollectTriangles(RE::NiGeometry* g, std::vector<std::array<int, 3>>& out)
	{
		const int vertexCount = g->data ? g->data->vertexCount : 0;
		auto valid = [&](int a, int b, int c) {
			return a != b && b != c && a != c && a >= 0 && b >= 0 && c >= 0 && a < vertexCount && b < vertexCount && c < vertexCount;
		};
		if (auto* sp = SkinPartition(g->skinInstance); sp && sp->partitions && sp->count < 1024) {
			for (uint32_t i = 0; i < sp->count; ++i) {
				const uint8_t* p = sp->partitions + i * kPartitionSize;
				const uint16_t* map = PartPtr<uint16_t>(p, 0x0C);
				const uint16_t* tris = PartPtr<uint16_t>(p, 0x14);
				const uint16_t* stripLengths = PartPtr<uint16_t>(p, 0x18);
				const uint16_t vertices = PartU16(p, 0x1C), triangles = PartU16(p, 0x1E), strips = PartU16(p, 0x22);
				if (!map || !tris)
					continue;
				auto vertex = [&](uint16_t local) { return local < vertices ? (int)map[local] : -1; };
				if (strips && stripLengths) {
					// Strips: each triangle alternates winding; degenerate ones join the strips.
					const uint16_t* s = tris;
					for (uint16_t k = 0; k < strips; ++k) {
						for (uint16_t t = 2; t < stripLengths[k]; ++t) {
							int a = vertex(s[t - 2]), b = vertex(s[t - 1]), c = vertex(s[t]);
							if (t & 1)
								std::swap(a, b);
							if (valid(a, b, c))
								out.push_back({ a, b, c });
						}
						s += stripLengths[k];
					}
				} else {
					for (uint16_t t = 0; t < triangles; ++t) {
						int a = vertex(tris[t * 3]), b = vertex(tris[t * 3 + 1]), c = vertex(tris[t * 3 + 2]);
						if (valid(a, b, c))
							out.push_back({ a, b, c });
					}
				}
			}
			if (!out.empty())
				return;
		}
		auto* data = static_cast<RE::NiTriShapeData*>(g->data);
		if (data && data->triList && data->triangleCount && data->triListLength >= data->triangleCount * 3u) {
			for (uint16_t t = 0; t < data->triangleCount; ++t) {
				int a = data->triList[t * 3], b = data->triList[t * 3 + 1], c = data->triList[t * 3 + 2];
				if (valid(a, b, c))
					out.push_back({ a, b, c });
			}
		}
	}

	// Each vertex's bone weights (skin-instance bone, weight), from the skin partitions, or from
	// NiSkinData's per-bone lists when the game kept them and there are no partitions.
	static bool CollectWeights(RE::NiGeometry* g, std::vector<std::vector<std::pair<int, float>>>& out)
	{
		const int vertexCount = g->data ? g->data->vertexCount : 0;
		out.assign(vertexCount, {});
		auto* skin = g->skinInstance;
		if (auto* sp = SkinPartition(skin); sp && sp->partitions && sp->count < 1024) {
			bool any = false;
			for (uint32_t i = 0; i < sp->count; ++i) {
				const uint8_t* p = sp->partitions + i * kPartitionSize;
				const uint16_t* bones = PartPtr<uint16_t>(p, 0x04);
				const float* weights = PartPtr<float>(p, 0x08);
				const uint16_t* map = PartPtr<uint16_t>(p, 0x0C);
				const uint8_t* palette = PartPtr<uint8_t>(p, 0x10);
				const uint16_t vertices = PartU16(p, 0x1C), partBones = PartU16(p, 0x20), perVertex = PartU16(p, 0x24);
				if (!bones || !weights || !map || perVertex == 0 || perVertex > 8)
					continue;
				for (uint16_t v = 0; v < vertices; ++v) {
					int shapeVertex = map[v];
					if (shapeVertex < 0 || shapeVertex >= vertexCount || !out[shapeVertex].empty())
						continue;   // a vertex shared by two partitions has the same weights in both
					for (uint16_t k = 0; k < perVertex; ++k) {
						float w = weights[v * perVertex + k];
						uint16_t local = palette ? palette[v * perVertex + k] : k;
						if (w > 0.0f && local < partBones && bones[local] < skin->boneCount)
							out[shapeVertex].push_back({ (int)bones[local], w });
					}
					any = true;
				}
			}
			if (any)
				return true;
		}
		if (skin && skin->skinData && skin->skinData->boneData) {
			auto* data = skin->skinData;
			bool any = false;
			for (uint32_t b = 0; b < data->boneCount; ++b) {
				auto& bd = data->boneData[b];
				if (!bd.vertData)
					continue;
				for (uint16_t i = 0; i < bd.vertCount; ++i) {
					auto& vw = bd.vertData[i];
					if (vw.vertex < vertexCount && vw.weight > 0.0f) {
						out[vw.vertex].push_back({ (int)b, vw.weight });
						any = true;
					}
				}
			}
			return any;
		}
		return false;
	}

	static std::string Lower(std::string s)
	{
		for (auto& c : s)
			c = (char)std::tolower((unsigned char)c);
		return s;
	}

	SkinnedMeshBone* SkyrimSystem::findBone(const RE::BSFixedString& name)
	{
		for (auto i : m_bones) {
			if (i->m_name == name) {
				return i.get();
			}
		}
		return nullptr;
	}

	SkinnedMeshBody* SkyrimSystem::findBody(const RE::BSFixedString& name)
	{
		for (auto i : m_meshes) {
			if (i->m_name == name) {
				return i.get();
			}
		}
		return nullptr;
	}

	int SkyrimSystem::findBoneIdx(const RE::BSFixedString& name)
	{
		for (int i = 0; i < m_bones.size(); ++i) {
			if (m_bones[i]->m_name == name) {
				return i;
			}
		}
		return -1;
	}

	SkyrimSystem::SkyrimSystem(RE::NiNode* skeleton) :
		m_skeleton(skeleton), m_oldRoot(nullptr)
	{
		m_oldRoot = m_skeleton;
		m_lastRootPosition.setZero();
	}

	SkyrimSystem::~SkyrimSystem()
	{
		// The skin slots go back to what the game had put there before the nodes they point at go. A slot the game has
		// re-pointed since (the item detached and attached again) keeps the game's binding.
		// The skin is held, so its bone array lives as long as it still is the one the slot was put in.
		// An item the game detached is not touched at all, and its nodes are kept (see m_itemDetached).
		int restored = 0, retired = 0;
		for (auto& s : m_boundSlots) {
			if (!m_itemDetached && s.holds()) {
				s.skin->bones[s.slot] = s.original;
				++restored;
			} else if (s.bound && !isRetiredNode(s.bound)) {
				g_retiredNodes.emplace_back(s.bound);
				g_retiredSet.insert(s.bound);
				++retired;
			}
		}
		if (!m_boundSlots.empty())
			logger::info("[SMP] system removed: {} skin slot(s) put back, {} node(s) kept", restored, retired);
	}

	// The player's first-person model: the game draws it after moving it into camera space, updating its nodes again from
	// their local transforms. So the nodes made here (held by skin slots only, out of the scene) hang on their anchors, with
	// the update flags (SELECTIVE_UPDATE 0x2, SELECTIVE_UPDATE_TRANSFORMS 0x4), and the simulated bones write their local
	// transforms too: that update then puts the charm where the physics has it. And it collides with nothing of other
	// systems: it sits where the third-person body is.
	void SkyrimSystem::setFirstPerson()
	{
		typedef void(__thiscall * AttachChild_t)(RE::NiNode*, RE::NiAVObject*, int);
		static const auto attachChild = (AttachChild_t)0x00A5ED10;   // NiNode::AttachChild(child, bFirstAvail)
		m_firstPerson = true;
		for (auto& m : m_meshes)
			m->m_isolation = this;
		int attached = 0;
		for (auto& b : m_bones) {
			auto* bone = static_cast<SkyrimBone*>(b.get());
			bone->m_isolation = this;
			bone->m_writeLocal = true;
			RE::NiNode* node = bone->m_node.get();
			if (!node || node->parent || !bone->m_anchor)
				continue;
			node->local = bone->m_anchorToNode;
			*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(node) + 0x30) |= 0x2 | 0x4;
			attachChild(bone->m_anchor.get(), node, 1);
			++attached;
		}
		logger::info("[SMP] first person: {} node(s) hung on their anchors, collisions kept to this system", attached);
	}

	int SkyrimSystem::releaseSlots()
	{
		int restored = 0;
		for (auto& s : m_boundSlots)
			if (s.holds()) {
				s.skin->bones[s.slot] = s.original;
				++restored;
			}
		m_boundSlots.clear();
		return restored;
	}

	bool SkyrimSystem::isRetiredNode(const RE::NiAVObject* node)
	{
		return g_retiredSet.count(node) != 0;
	}

	// The item's root in the actor's frame (the skeleton root's).
	btVector3 SkyrimSystem::modelOffset() const
	{
		const RE::NiPoint3 d = m_model->world.translate - m_skeleton->world.translate;
		return convertNi(m_skeleton->world.rotate.Transpose() * d);
	}

	float SkyrimSystem::prepareForRead(float timeStep)
	{
		auto newRoot = m_skeleton.get();
		while (newRoot->parent) {
			newRoot = newRoot->parent;
		}

		if (m_oldRoot != newRoot)
			timeStep = RESET_PHYSICS;

		if (!m_initialized) {
			timeStep = RESET_PHYSICS;
			m_initialized = true;
		}

		auto* world = SkyrimPhysicsWorld::get();
		btQuaternion newRot = convertNi(m_skeleton->world.rotate);
		btVector3 newPos = convertNi(m_skeleton->world.translate);

		if (timeStep > RESET_PHYSICS && world->m_resetPc > 0)
			timeStep = RESET_PHYSICS;
		if (timeStep <= RESET_PHYSICS)
			++m_resetsOther;

		// A jump of the whole actor (a load door, fast travel, a teleport) or a turn faster than any
		// animation turns: start the cloth over in its rest pose, as FSMP's unclamped resets do.
		if (timeStep > RESET_PHYSICS) {
			if ((newPos - m_lastRootPosition).length() > 500.0f) {
				timeStep = RESET_PHYSICS;
				++m_resetsJump;
			} else if (const float jump = m_haveModelOffset && m_model ? (modelOffset() - m_lastModelOffset).length() : 0.0f; jump > 20.0f && jump > 1500.0f * timeStep) {
				// The item alone jumped on the actor, over 20 units and faster than 1500 a second (a weapon snapped into the
				// hands from a rest pose, re-attached elsewhere; a fast swing at a low frame rate is neither):
				// its pieces would be dragged through what they collide with (a charm through its weapon), so over again.
				timeStep = RESET_PHYSICS;
				++m_resetsJump;
			} else if (world->m_unclampedResets && !m_firstPerson) {
				// Not on the first-person model: its root turns with the camera (looking around is not a teleport).
				btVector3 rotAxis;
				float rotAngle;
				btTransformUtil::calculateDiffAxisAngleQuaternion(m_lastRootRotation, newRot, rotAxis, rotAngle);
				float limit = world->m_unclampedResetAngle * timeStep;
				if (rotAngle < -limit || rotAngle > limit) {
					timeStep = RESET_PHYSICS;
					++m_resetsTurn;
				}
			}
		}

		m_lastRootRotation = newRot;
		m_lastRootPosition = newPos;
		if (m_model) { m_lastModelOffset = modelOffset(); m_haveModelOffset = true; }
		m_oldRoot = hdt::make_nismart(newRoot);
		return timeStep;
	}

	SkyrimSystemCreator::SkyrimSystemCreator()
	{
	}

	void SkyrimSystemCreator::indexBone(SkyrimBone* bone)
	{
		m_boneIndex.emplace(bone->m_name.data(), bone);
	}

	SkyrimBone* SkyrimSystemCreator::findBoneFromIndex(const RE::BSFixedString& name) const
	{
		auto it = m_boneIndex.find(name.data());
		return it != m_boneIndex.end() ? it->second : nullptr;
	}

	// A skeleton node by name, outside the item's own subtree (whose copies of the bones the
	// game does not animate).
	RE::NiNode* SkyrimSystemCreator::findObjectByName(const RE::BSFixedString& name)
	{
		struct Search
		{
			RE::NiAVObject* skip;
			RE::NiNode* run(RE::NiAVObject* obj, const char* name, int depth)
			{
				if (!obj || obj == skip || depth > 64)
					return nullptr;
				auto* node = castNiNode(obj);
				if (!node)
					return nullptr;
				if (node->m_name && _stricmp(node->m_name, name) == 0)
					return node;
				for (uint16_t i = 0; i < node->ChildEnd(); ++i)
					if (auto* found = run(node->children[i], name, depth + 1))
						return found;
				return nullptr;
			}
		} search{ m_worldObject ? nullptr : m_model };   // a placed object's bones are its own nodes
		if (!m_skeletonNodes.empty()) {
			auto it = m_skeletonNodes.find(Lower(name.c_str()));
			return it != m_skeletonNodes.end() ? it->second : nullptr;
		}
		// Within the skeleton a bound bone belongs to first (its "Bip01"), then the whole actor.
		RE::NiNode* found = nullptr;
		for (auto& [key, bound] : m_boundNodes) {
			for (RE::NiAVObject* p = bound; p; p = p->parent)
				if (p->m_name && _stricmp(p->m_name, "Bip01") == 0) { found = search.run(p, name.c_str(), 0); break; }
			break;
		}
		RE::NiNode* anywhere = search.run(m_skeleton, name.c_str(), 0);
		if (!found)
			found = anywhere;
		else if (anywhere && anywhere != found) {
			static int reported = 0;
			if (reported++ < 20)
				logger::warn("[SMP] {}: the actor has another node of that name ({:p} at {:.1f} {:.1f} {:.1f}) outside the skeleton the item's skin "
				             "is bound to: the skeleton's ({:p} at {:.1f} {:.1f} {:.1f}) is used", name.c_str(), (void*)anywhere, anywhere->world.translate.x,
					anywhere->world.translate.y, anywhere->world.translate.z, (void*)found, found->world.translate.x, found->world.translate.y, found->world.translate.z);
		}
		// The node the item's skin is bound to for this bone comes first: the physics must move what the skin draws with.
		if (auto it = m_boundNodes.find(Lower(name.c_str())); it != m_boundNodes.end()) {
			if (found && found != it->second) {
				static int reported = 0;
				if (reported++ < 20)
					logger::warn("[SMP] {}: the scene has another node of that name ({:p} at {:.1f} {:.1f} {:.1f}) than the one the item's skin is bound to "
					             "({:p} at {:.1f} {:.1f} {:.1f}): the skin's is used", name.c_str(), (void*)found, found->world.translate.x, found->world.translate.y,
						found->world.translate.z, (void*)it->second, it->second->world.translate.x, it->second->world.translate.y, it->second->world.translate.z);
			}
			return it->second;
		}
		return found;
	}

	SkyrimBone* SkyrimSystemCreator::getOrCreateBone(const RE::BSFixedString& name)
	{
		auto bone = findBoneFromIndex(getRenamedBone(name));
		if (bone) {
			return bone;
		}

		logger::warn("Bone {} used before being created, trying to create it with current default values", name.c_str());
		return createBoneFromNodeName(getRenamedBone(name));
	}

	RE::BSFixedString SkyrimSystemCreator::getRenamedBone(const RE::BSFixedString& name)
	{
		auto iter = m_renameMap.find(name);
		if (iter != m_renameMap.end())
			return iter->second;
		return name;
	}

	// --- New Vegas: the item's own bones --------------------------------------------------------

	void SkyrimSystemCreator::collectExtraBones(RE::NiAVObject* obj, int depth)
	{
		if (!obj || depth > 64)
			return;
		if (auto* g = castNiGeometry(obj)) {
			std::string listed = g->SmpBonesList();
			bool has = g->FindStringExtra("SMP Bones") != nullptr;
			if (!has)
				if (auto it = m_boneLists.find(Lower(g->GetName())); it != m_boneLists.end()) { listed = it->second; has = true; }
			const char* list = has ? listed.c_str() : nullptr;
			if (list) {
				++m_listedShapes;
				if (!g->skinInstance || !g->skinInstance->boneCount)
					logger::warn("[SMP] {} lists its bones but has no skin yet", g->GetName());
				else {
					// The list names the skin's slots in order: a different count means the skin was changed after
					// (an editor removed unweighted bones) and every slot after the first change is misnamed.
					uint32_t entries = 0;
					for (const char* c = list; *c; ++c) entries += *c == ';';
					if (entries != g->skinInstance->boneCount)
						logger::warn("[SMP] {}: \"SMP Bones\" lists {} bones but the skin has {}: the NIF was edited after it was made "
						             "(Outfit Studio removes unweighted bones); run tools\\smprepair on it", g->GetName(), entries, g->skinInstance->boneCount);
				}
			}
			if (list && g->skinInstance) {
				uint32_t slot = 0;
				const char* p = list;
				while (*p && slot < g->skinInstance->boneCount) {
					const char* end = strchr(p, ';');
					std::string entry = end ? std::string(p, end) : std::string(p);
					auto bar = entry.find('|');
					std::string name = bar == std::string::npos ? entry : entry.substr(0, bar);
					std::string parent = bar == std::string::npos ? std::string() : entry.substr(bar + 1);
					if (!name.empty())
						m_extraBones[Lower(name)].push_back({ g, slot, name, parent });
					// A skeleton bone ("name|") the game bound: the node the skin draws it with.
					if (!name.empty() && parent.empty() && g->skinInstance->bones)
						if (auto* bound = castNiNode(g->skinInstance->bones[slot]); bound && bound->m_name && _stricmp(bound->m_name, name.c_str()) == 0)
							m_boundNodes.emplace(Lower(name), bound);
					++slot;
					if (!end)
						break;
					p = end + 1;
				}
			}
		}
		if (auto* node = castNiNode(obj)) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				collectExtraBones(node->children[i], depth + 1);
		}
	}

	std::string SkyrimSystemCreator::skinSlotName(RE::NiGeometry* geometry, uint32_t slot)
	{
		for (auto& [key, uses] : m_extraBones)
			for (auto& u : uses)
				if (u.geometry == geometry && u.slot == slot)
					return u.name;
		return {};
	}

	// A node for a bone the item brings, at its bind pose against the body, carried on its parent:
	// the bone it hangs from in the item's NIF (a skeleton bone, or another of the item's bones).
	RE::NiNode* SkyrimSystemCreator::createExtraBoneNode(const std::string& name, RE::NiTransform& anchorToNode, RE::NiNode*& anchor)
	{
		auto key = Lower(name);
		auto it = m_extraBones.find(key);
		if (it == m_extraBones.end() || it->second.empty())
			return nullptr;
		if (auto created = m_createdNodes.find(key); created != m_createdNodes.end()) {
			// Made earlier (as another bone's parent): what it is carried on too, for its physics bone.
			if (auto a = m_createdAnchors.find(key); a != m_createdAnchors.end()) { anchor = a->second.first; anchorToNode = a->second.second; }
			return created->second;
		}

		const ExtraBone& use = it->second.front();
		auto* skin = use.geometry->skinInstance;
		auto* data = skin ? skin->skinData : nullptr;
		if (!data || !data->boneData || use.slot >= data->boneCount || !skin->bones)
			return nullptr;

		// The skin's frame in the world, from a bone of the same skin the game did bind: the parent
		// if the skeleton has it, else the first bound bone with a skeleton name.
		int reference = -1;
		RE::NiNode* parentNode = nullptr;
		if (!use.parent.empty()) {
			// One of the item's own bones as parent: always the node made here, never a node of that name found in the
			// scene (a hair NIF's own copies of its bones, left where the file had them, would carry this bone off).
			parentNode = isItemBone(use.parent) ? nullptr : findObjectByName(use.parent.c_str());
			if (!parentNode) {
				// The parent is one of the item's own bones: build it first.
				RE::NiTransform dummy;
				RE::NiNode* dummyAnchor = nullptr;
				parentNode = createExtraBoneNode(use.parent, dummy, dummyAnchor);
				if (parentNode) {
					// Find or make its physics bone too, so it is driven and read before this one.
					getOrCreateBone(use.parent.c_str());
				}
			}
		}
		for (uint32_t s = 0; s < data->boneCount && reference < 0; ++s) {
			auto* b = skin->bones[s];
			if (!b || !b->m_name || s == use.slot)
				continue;
			// The slot must be the parent's own (its listed name): an unbound slot also points at the skeleton root, so
			// with the root as parent ("Bip01") another bone's bind would place this one.
			if (parentNode && b == parentNode && _stricmp(b->m_name, skinSlotName(use.geometry, s).c_str()) == 0)
				reference = (int)s;
		}
		// A slot the game bound: its node carries the name the item lists for it. (An unbound slot
		// points at the skeleton root, whatever that is called.) Only a skeleton bone's: an item bone's slot can still hold
		// the item NIF's own node of that name, left where the file had it (a hair rigged with its bones listed first).
		for (uint32_t s = 0; s < data->boneCount && reference < 0; ++s) {
			auto* b = skin->bones[s];
			const std::string slotName = skinSlotName(use.geometry, s);
			if (b && b->m_name && s != use.slot && _stricmp(b->m_name, slotName.c_str()) == 0 && !isItemBone(slotName))
				reference = (int)s;
		}
		if (reference < 0) {
			logger::warn("Bone {}: no bound bone in its skin to place it from, skipped", name.c_str());
			return nullptr;
		}

		const RE::NiTransform skinToWorld = skin->bones[reference]->world * data->boneData[reference].skinToBone;
		const RE::NiTransform nodeWorld = skinToWorld * data->boneData[use.slot].skinToBone.Invert();

		anchor = parentNode ? parentNode : castNiNode(skin->bones[reference]);
		if (!anchor) {
			logger::warn("Bone {}: no node to carry it on, skipped", name.c_str());
			return nullptr;
		}
		anchorToNode = anchor->world.Invert() * nodeWorld;

		auto* node = createNode(name.c_str());
		if (!node)
			return nullptr;
		node->local = anchorToNode;
		node->world = nodeWorld;
		m_createdNodes[key] = node;
		m_createdAnchors[key] = { anchor, anchorToNode };

		// Point every skin slot of this bone, on every shape of the item, at the node.
		for (auto& u : it->second) {
			auto* s = u.geometry->skinInstance;
			if (!s || !s->bones || u.slot >= s->boneCount)
				continue;
			// A slot still holding an earlier system's node was never bound by the game: it goes back to the skeleton root.
			RE::NiAVObject* original = s->bones[u.slot];
			if (SkyrimSystem::isRetiredNode(original))
				original = m_skeleton;
			else if (original != m_skeleton)   // only counted: it may be a stale pointer to a freed object, so it is not read
				++m_staleSlots;
			m_mesh->m_boundSlots.push_back({ RE::NiPointer<RE::NiSkinInstance>(s), u.slot, original, node, s->bones });
			s->bones[u.slot] = node;
		}
		logger::info("Created bone node {} on {}, {} skin slot(s)", name.c_str(), anchor->GetName(), it->second.size());
		return node;
	}

	RE::BSTSmartPointer<SkyrimSystem> SkyrimSystemCreator::createOrUpdateSystem(RE::NiNode* skeleton, RE::NiAVObject* model, DefaultBBP::PhysicsFile_t* file, std::unordered_map<RE::BSFixedString, RE::BSFixedString>&& renameMap, SkyrimSystem* old_system)
	{
		auto path = file->first;
		if (path.empty()) {
			return nullptr;
		}

		auto loaded = readAllFile(path.c_str());
		if (loaded.empty()) {
			logger::error("[SMP] physics file '{}' not found", path.c_str());
			return bindOnlySystem(skeleton, model);
		}

		m_renameMap = std::move(renameMap);
		m_skeleton = skeleton;
		m_model = model;
		m_filePath = path;

		XMLReader reader((uint8_t*)loaded.data(), loaded.size());
		m_reader = &reader;

		m_reader->nextStartElement();
		if (m_reader->GetName() != "system") {
			logger::error("[SMP] '{}' is not an SMP physics file (no <system>)", path.c_str());
			return bindOnlySystem(skeleton, model);
		}

		auto meshNameMap = file->second;

		m_mesh = RE::make_smart<SkyrimSystem>(skeleton);
		m_mesh->m_model = model;
		m_boneIndex.clear();
		m_extraBones.clear();
		m_createdNodes.clear();
		m_createdAnchors.clear();
		m_boundNodes.clear();
		m_listedShapes = 0;
		m_staleSlots = 0;
		if (!m_worldObject) collectExtraBones(model);

		try {
			while (m_reader->Inspect()) {
				if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
					const auto name = m_reader->GetName();
					if (name == "bone") {
						readOrUpdateBone();
					} else if (name == "bone-default") {
						auto clsname = m_reader->getAttribute("name", "");
						auto extends = m_reader->getAttribute("extends", "");
						auto defaultBoneInfo = getBoneTemplate(extends);
						readBoneTemplate(defaultBoneInfo);
						m_boneTemplates[clsname] = defaultBoneInfo;
					} else if (name == "per-vertex-shape") {
						auto shape = readPerVertexShape(meshNameMap);
						if (shape && shape->m_vertices.size()) {
							m_mesh->m_meshes.push_back(shape);
							shape->m_mesh = m_mesh.get();
						}
					} else if (name == "per-triangle-shape") {
						auto shape = readPerTriangleShape(&meshNameMap);
						if (shape && shape->m_vertices.size()) {
							m_mesh->m_meshes.push_back(shape);
							shape->m_mesh = m_mesh.get();
						}
					} else if (name == "nv-virtual-legs") {
						auto shape = readVirtualLegs();
						if (shape && shape->m_vertices.size()) {
							m_mesh->m_meshes.push_back(shape);
							shape->m_mesh = m_mesh.get();
						}
					} else if (name == "constraint-group") {
						auto constraint = readConstraintGroup();
						if (constraint)
							m_mesh->m_constraintGroups.push_back(constraint);
					} else if (name == "generic-constraint") {
						auto constraint = readGenericConstraint();
						if (constraint)
							m_mesh->m_constraints.push_back(constraint);
					} else if (name == "stiffspring-constraint") {
						auto constraint = readStiffSpringConstraint();
						if (constraint)
							m_mesh->m_constraints.push_back(constraint);
					} else if (name == "conetwist-constraint") {
						auto constraint = readConeTwistConstraint();
						if (constraint)
							m_mesh->m_constraints.push_back(constraint);
					} else if (name == "generic-constraint-default") {
						auto clsname = m_reader->getAttribute("name", "");
						auto extends = m_reader->getAttribute("extends", "");
						auto defaultGenericConstraintTemplate = getGenericConstraintTemplate(extends);
						readGenericConstraintTemplate(defaultGenericConstraintTemplate);
						m_genericConstraintTemplates[clsname] = defaultGenericConstraintTemplate;
					} else if (name == "stiffspring-constraint-default") {
						auto clsname = m_reader->getAttribute("name", "");
						auto extends = m_reader->getAttribute("extends", "");
						auto defaultStiffSpringConstraintTemplate = getStiffSpringConstraintTemplate(extends);
						readStiffSpringConstraintTemplate(defaultStiffSpringConstraintTemplate);
						m_stiffSpringConstraintTemplates[clsname] = defaultStiffSpringConstraintTemplate;
					} else if (name == "conetwist-constraint-default") {
						auto clsname = m_reader->getAttribute("name", "");
						auto extends = m_reader->getAttribute("extends", "");
						auto defaultConeTwistConstraintTemplate = getConeTwistConstraintTemplate(extends);
						readConeTwistConstraintTemplate(defaultConeTwistConstraintTemplate);
						m_coneTwistConstraintTemplates[clsname] = defaultConeTwistConstraintTemplate;
					} else if (name == "shape") {
						auto attrName = m_reader->getAttribute("name");
						auto shape = readShape();
						if (shape) {
							m_shapeRefs.push_back(shape);
							m_shapes.insert(std::make_pair(attrName, shape));
						}
					} else {
						logger::warn("unknown element - {}", name.c_str());
						m_reader->skipCurrentElement();
					}
				} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
					break;
			}
		} catch (const std::string& err) {
			logger::error("xml parse error - {}", err.c_str());
			m_deferredBuilds.clear();
			m_mesh = nullptr;   // gives its skin slots back first
			return bindOnlySystem(skeleton, model);
		}

		if (m_reader->GetErrorCode() != Xml::ErrorCode::None) {
			logger::error("xml parse error - {}", m_reader->GetErrorMessage());
			m_deferredBuilds.clear();
			m_mesh = nullptr;
			return bindOnlySystem(skeleton, model);
		}

		for (const auto& db : m_deferredBuilds) {
			if (db.vertexShape)
				db.vertexShape->autoGen();
			db.body->finishBuild();
		}
		m_deferredBuilds.clear();

		addRigidPieceCollision();

		// A small system (a charm, a pendant: a few light links on a bone that can move fast, a weapon swung by a
		// turn) has its joints solved with 4 times the solver's iterations: at 10, a hard turn left a charm's pivots up to
		// 2 units apart (the chain came apart), at 40 0.5 (previewer, --joints). Bullet takes the most any constraint of
		// a simulation island asks for, for that island only, so the other items' solving costs the same.
		{
			int simulated = 0;
			for (auto& b : m_mesh->m_bones) simulated += !b->m_rig.isStaticOrKinematicObject();
			if (simulated > 0 && simulated <= 16) {
				const int iterations = 4 * SkyrimPhysicsWorld::get()->getSolverInfo().m_numIterations;
				for (auto& c : m_mesh->m_constraints)
					if (c->getConstraint()) c->getConstraint()->setOverrideNumSolverIterations(iterations);
				for (auto& group : m_mesh->m_constraintGroups)
					for (auto& c : group->m_constraints)
						if (c->getConstraint()) c->getConstraint()->setOverrideNumSolverIterations(iterations);
			}
		}

		if (m_ground) {
			bool simulatedBody = false;
			for (auto& b : m_mesh->m_meshes) simulatedBody |= !b->m_isKinematic;
			if (simulatedBody) addGroundPlane();
		}

		m_mesh->m_skeleton = hdt::make_nismart(m_skeleton);
		m_mesh->m_shapeRefs.swap(m_shapeRefs);
		std::sort(m_mesh->m_bones.begin(), m_mesh->m_bones.end(), [](const auto& a, const auto& b) {
			return static_cast<SkyrimBone*>(a.get())->m_depth < static_cast<SkyrimBone*>(b.get())->m_depth;
		});

		if (m_staleSlots)
			logger::info("[SMP] {} skin slot(s) of the item's bones held something other than the skeleton root (left from an earlier attach; not referenced)", m_staleSlots);
		return m_mesh->valid() ? m_mesh : nullptr;
	}

	// New Vegas: solid collision for rigid pieces against a rigid wall, made from the item's own collision meshes, so no
	// XML has to ask for it. Mesh collision (cloth vertices against body triangles) is tested a step at a time and pushes
	// softly: small rigid pieces swung hard (a charm against its weapon) pass a wall between two steps. Here:
	//   - a body (per-triangle) mesh whose every vertex is on one animated bone (a weapon's wall proxy) gives that bone a
	//     convex hull of those vertices, solid (a mesh spread over several bones, as a body's legs, is left alone);
	//   - if the item has such a wall, each simulated bone that carries a rigid piece of a cloth (per-vertex) mesh (8+
	//     vertices weighted 0.99+ to it alone: a charm's link, its pendant) collides as a solid: a convex hull of that
	//     piece, or its own shape from the XML; these pieces do not collide with each other. No continuous collision: the
	//     solid contacts hold well enough with the item-jump reset (prepareForRead), and a chain kept whole matters more.
	// The meshes' own collision stays; this is added to it. Cloth and hair (blended weights, no rigid wall) are unchanged.
	void SkyrimSystemCreator::addRigidPieceCollision()
	{
		auto toRig = [](SkinnedMeshBone* bone, const SkinnedMeshBody::SkinnedBone& sb, const btVector3& skinPos) {
			return bone->m_rigToLocal * (sb.vertexToBone * skinPos);   // the vertex in the bone's body (rig) space
		};
		auto rigidSlot = [](const Vertex& v) {
			for (int k = 0; k < 4; ++k)
				if (v.m_weight[k] >= 0.99f) return (int)v.getBoneIdx(k);
			return -1;
		};
		auto hullOf = [this](const std::vector<btVector3>& points) {
			auto hull = std::make_shared<btConvexHullShape>();
			for (auto& p : points) hull->addPoint(p, false);
			hull->recalcLocalAabb();
			hull->setMargin(0.01f);
			m_shapeRefs.push_back(hull);
			return hull.get();
		};

		int walls = 0;
		for (auto& body : m_mesh->m_meshes) {
			if (!body->m_shape || !body->m_shape->asPerTriangleShape() || body->m_vertices.size() < 4) continue;
			int slot = rigidSlot(body->m_vertices[0]);
			for (auto& v : body->m_vertices) if (rigidSlot(v) != slot) { slot = -1; break; }
			if (slot < 0 || slot >= (int)body->m_skinnedBones.size()) continue;
			auto* bone = body->m_skinnedBones[slot].ptr;
			if (!bone || !bone->m_rig.isStaticOrKinematicObject() || bone->m_shapeCollides) continue;
			std::vector<btVector3> points;
			for (auto& v : body->m_vertices) points.push_back(toRig(bone, body->m_skinnedBones[slot], v.m_skinPos));
			bone->m_rig.setCollisionShape(hullOf(points));
			bone->m_shapeCollides = true;
			++walls;
			logger::info("[SMP] {}: rigid on {}, collides as a solid wall ({} vertices)", body->m_name.c_str(), bone->m_name.c_str(), points.size());
		}
		if (!walls) return;

		std::vector<SkinnedMeshBone*> pieces;
		for (auto& body : m_mesh->m_meshes) {
			if (!body->m_shape || body->m_shape->asPerTriangleShape()) continue;
			std::map<int, std::vector<btVector3>> rigid;
			for (auto& v : body->m_vertices) {
				const int slot = rigidSlot(v);
				if (slot >= 0 && slot < (int)body->m_skinnedBones.size()) rigid[slot].push_back(v.m_skinPos);
			}
			for (auto& [slot, skinPoints] : rigid) {
				auto* bone = body->m_skinnedBones[slot].ptr;
				if (!bone || bone->m_rig.isStaticOrKinematicObject() || skinPoints.size() < 8 ||
					std::find(pieces.begin(), pieces.end(), bone) != pieces.end()) continue;
				// A bone with its own shape (the XML's) keeps it.
				const bool own = bone->m_shapeCollides;
				if (!own) {
					std::vector<btVector3> points;
					for (auto& p : skinPoints) points.push_back(toRig(bone, body->m_skinnedBones[slot], p));
					bone->m_rig.setCollisionShape(hullOf(points));
					bone->m_shapeCollides = true;
				}
				pieces.push_back(bone);
				logger::info("[SMP] {}: its piece on {} collides as a solid ({} vertices, {})", body->m_name.c_str(), bone->m_name.c_str(), skinPoints.size(),
					own ? "the XML's shape" : "their hull");
			}
		}
		for (size_t i = 0; i < pieces.size(); ++i)
			for (size_t j = i + 1; j < pieces.size(); ++j) {
				pieces[i]->m_rig.setIgnoreCollisionCheck(&pieces[j]->m_rig, true);
				pieces[j]->m_rig.setIgnoreCollisionCheck(&pieces[i]->m_rig, true);
			}
	}

	// New Vegas: the ground under the actor, so cloth rests on it when crouching or sitting instead of
	// sinking through. A square of two triangles facing up, skinned to an animation-driven bone carried
	// on the actor's root (its feet; the root keeps upright), colliding as a per-triangle body tagged
	// "ground" with this actor's own cloth only. An item opts out with <no-collide-with-tag>ground.
	void SkyrimSystemCreator::addGroundPlane()
	{
		auto* node = createNode("SMP Ground");
		if (!node)
			return;
		BoneTemplate kinematic;   // mass 0: driven, never simulated
		auto* bone = new SkyrimBone("SMP Ground", node, m_skeleton, kinematic);
		bone->m_anchor = m_skeleton;
		bone->m_anchorToNode = RE::NiTransform();
		bone->m_anchorToNode.translate.z = m_groundOffset;
		bone->m_depth = 1;
		bone->readTransform(RESET_PHYSICS);
		m_mesh->m_bones.push_back(hdt::make_smart(bone));
		indexBone(bone);

		const float half = 150.0f;   // a 300-unit square: well past any garment's reach
		auto body = RE::make_smart<SkyrimBody>();
		body->m_name = "SMP Ground";
		body->m_shared = SkyrimBody::SharedType::SHARED_INTERNAL;
		body->m_tags.push_back("ground");
		body->m_noCollideWithTags.insert("ground");
		body->m_noCollideWithTags.insert("body");
		btQsTransform identity;
		body->addBone(bone, identity, BoundingSphere(btVector3(0, 0, 0), half * 1.5f));
		const float corners[4][2] = { { -half, -half }, { half, -half }, { half, half }, { -half, half } };
		body->m_vertices.resize(4);
		for (int i = 0; i < 4; i++) {
			auto& v = body->m_vertices[i];
			v.m_skinPos = btVector3(corners[i][0], corners[i][1], 0);
			v.m_weight[0] = 1.0f;
			v.m_boneIdx[0] = 0;
			for (int k = 1; k < 4; k++) { v.m_weight[k] = 0.0f; v.m_boneIdx[k] = 0; }
		}
		auto shape = RE::make_smart<PerTriangleShape>(body.get());
		shape->addTriangle(0, 1, 2);   // counter-clockwise from above: the normal points up
		shape->addTriangle(0, 2, 3);
		shape->m_shapeProp.margin = 0.5f;
		shape->m_shapeProp.penetration = 15.0f;   // anything up to 15 units under the ground is pushed back up
		body->finishBuild();
		m_mesh->m_meshes.push_back(body);
		body->m_mesh = m_mesh.get();
		logger::info("[SMP] ground plane added at the actor's feet");
	}

	// Tapered capsules (a radius at each end, straight between), each a closed low-poly mesh skinned to its bone, into one
	// per-triangle shape: the virtual legs' collision and the stand-ins'. One capsule per bone.
	void SkyrimSystemCreator::AddCapsules(SkyrimBody* body, PerTriangleShape* shape, const std::vector<CapsuleSegment>& capsules)
	{
		const int around = 10;
		for (auto& s : capsules) {
			// The capsule in the world, then into its bone's space (the body is skinned in bone space).
			RE::NiPoint3 axis = s.b - s.a;
			float len = axis.Length();
			axis = len > 1e-4f ? axis * (1.0f / len) : RE::NiPoint3{ 0, 0, -1 };
			RE::NiPoint3 ref = std::fabs(axis.z) < 0.9f ? RE::NiPoint3{ 0, 0, 1 } : RE::NiPoint3{ 1, 0, 0 };
			RE::NiPoint3 u{ axis.y * ref.z - axis.z * ref.y, axis.z * ref.x - axis.x * ref.z, axis.x * ref.y - axis.y * ref.x };
			u = u * (1.0f / u.Length());
			RE::NiPoint3 v{ axis.y * u.z - axis.z * u.y, axis.z * u.x - axis.x * u.z, axis.x * u.y - axis.y * u.x };
			RE::NiTransform toBone = s.bone->m_node->world.Invert();
			int boneIdx = body->addBone(s.bone, btQsTransform(), BoundingSphere(convertNi(toBone * ((s.a + s.b) * 0.5f)), len * 0.5f + std::max(s.ra, s.rb)));
			int first = (int)body->m_vertices.size();
			auto add = [&](const RE::NiPoint3& world) {
				Vertex vx;
				vx.m_skinPos = convertNi(toBone * world);
				vx.m_weight[0] = 1.0f;
				vx.m_boneIdx[0] = boneIdx;
				for (int k = 1; k < 4; k++) { vx.m_weight[k] = 0.0f; vx.m_boneIdx[k] = boneIdx; }
				body->m_vertices.push_back(vx);
			};
			// Rings: each end cap as two latitude rings, the cylinder between; then the two poles' rings.
			const float caps[] = { -0.866f, -0.5f, 0.0f };   // sin of -60, -30, 0 degrees
			struct Ring { RE::NiPoint3 c; float r; };
			std::vector<Ring> rings;
			for (float sn : caps) rings.push_back({ s.a + axis * (sn * s.ra), s.ra * std::sqrt(1 - sn * sn) });
			for (int i = 2; i >= 0; i--) rings.push_back({ s.b + axis * (-caps[i] * s.rb), s.rb * std::sqrt(1 - caps[i] * caps[i]) });
			add(s.a - axis * s.ra);   // the poles
			add(s.b + axis * s.rb);
			for (auto& r : rings)
				for (int k = 0; k < around; k++) {
					float ang = 6.2831853f * k / around;
					add(r.c + (u * std::cos(ang) + v * std::sin(ang)) * r.r);
				}
			// Triangles, wound so their normals point outward: away from the nearest point of the axis segment.
			btVector3 A = convertNi(toBone * s.a), B = convertNi(toBone * s.b);
			auto tri = [&](int i0, int i1, int i2) {
				auto P = [&](int i) { return body->m_vertices[first + i].m_skinPos; };
				btVector3 n = (P(i1) - P(i0)).cross(P(i2) - P(i0));
				btVector3 c = (P(i0) + P(i1) + P(i2)) / 3.0f;
				btVector3 ab = B - A;
				float t = std::clamp((c - A).dot(ab) / std::max(ab.length2(), 1e-6f), 0.0f, 1.0f);
				if (n.dot(c - (A + ab * t)) < 0) std::swap(i1, i2);
				shape->addTriangle(first + i0, first + i1, first + i2);
			};
			const int ringCount = (int)rings.size();
			auto ringV = [&](int r, int k) { return 2 + r * around + (k % around); };
			for (int k = 0; k < around; k++) {
				tri(0, ringV(0, k), ringV(0, k + 1));
				tri(1, ringV(ringCount - 1, k + 1), ringV(ringCount - 1, k));
				for (int r = 0; r + 1 < ringCount; r++) {
					tri(ringV(r, k), ringV(r + 1, k), ringV(r + 1, k + 1));
					tri(ringV(r, k), ringV(r + 1, k + 1), ringV(r, k + 1));
				}
			}
		}
	}

	// A stand-in's capsules fitted to what the actor wears, as the virtual legs are to the legs: every skinned shape under its
	// root (body, armour, gloves, the FaceGen head), posed by its skin, sampled to about 1500 vertices a shape; each vertex
	// to its nearest capsule (counted only beside it, not past its ends); each end's radius the median distance of the
	// vertices in that end's third (the whole
	// capsule's if fewer than 8 there), kept within 0.6 to 1.8 times the preset. A capsule with fewer than 12 keeps its preset.
	void SkyrimSystemCreator::fitStandIn(RE::NiNode* actorRoot, std::vector<CapsuleSegment>& capsules)
	{
		auto segDist = [](const CapsuleSegment& s, const RE::NiPoint3& p, float& along) {
			RE::NiPoint3 ab = s.b - s.a, ap = p - s.a;
			float len2 = std::max(ab.x * ab.x + ab.y * ab.y + ab.z * ab.z, 1e-6f);
			along = std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2, 0.0f, 1.0f);
			return (p - (s.a + ab * along)).Length();
		};
		std::vector<std::vector<std::pair<float, float>>> d(capsules.size());   // per capsule: (where along, distance)
		int shapes = 0;
		std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* obj, int depth) {
			if (!obj || depth > 64) return;
			if (auto* g = castNiGeometry(obj)) {
				std::vector<std::vector<std::pair<int, float>>> weights;
				if (!g->skinInstance || !g->skinInstance->skinData || !g->skinInstance->bones || !g->data || !g->data->vertices || !CollectWeights(g, weights))
					return;
				auto* skin = g->skinInstance;
				const int count = g->data->vertexCount, stride = std::max(1, count / 1500);
				++shapes;
				for (int v = 0; v < count; v += stride) {
					RE::NiPoint3 p{};
					for (auto& [slot, w] : weights[v])
						if (slot < (int)skin->boneCount && skin->bones[slot])
							p = p + (skin->bones[slot]->world * skin->skinData->boneData[slot].skinToBone) * g->data->vertices[v] * w;
					int best = -1;
					float bestD = FLT_MAX, bestT = 0;
					for (size_t i = 0; i < capsules.size(); ++i) {
						float t;
						if (float dist = segDist(capsules[i], p, t); dist < bestD) { bestD = dist; bestT = t; best = (int)i; }
					}
					// Only beside a capsule, not past its ends (a hand beyond the forearm's would swell its wrist).
					if (best >= 0 && bestT > 0.0f && bestT < 1.0f) d[best].push_back({ bestT, bestD });
				}
				return;
			}
			if (auto* node = obj->AsNode())
				for (uint16_t i = 0; i < node->ChildEnd(); ++i) walk(node->children[i], depth + 1);
		};
		walk(actorRoot, 0);
		auto median = [](std::vector<float> v) { std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
		int fitted = 0;
		for (size_t i = 0; i < capsules.size(); ++i) {
			auto& c = capsules[i];
			if (d[i].size() < 12) continue;
			std::vector<float> all, nearA, nearB;
			for (auto& [t, dist] : d[i]) {
				all.push_back(dist);
				if (t < 0.33f) nearA.push_back(dist);
				if (t > 0.67f) nearB.push_back(dist);
			}
			const float whole = median(all);
			c.ra = std::clamp(nearA.size() >= 8 ? median(nearA) : whole, c.ra * 0.6f, c.ra * 1.8f);
			c.rb = std::clamp(nearB.size() >= 8 ? median(nearB) : whole, c.rb * 0.6f, c.rb * 1.8f);
			++fitted;
		}
		std::string radii;
		for (auto& c : capsules) radii += std::format("{} {:.1f}/{:.1f}; ", c.bone->m_name.c_str(), c.ra, c.rb);
		logger::info("[SMP] stand-in for '{}': {} of {} capsule(s) fitted to {} skinned shape(s): {}", actorRoot->GetName(), fitted, capsules.size(), shapes, radii);
	}

	// The weapon the actor holds (its model under the skeleton's "Weapon" node, drawn or holstered): one capsule along the
	// longest side of its own geometry's box in that node's frame (a blade, a haft, a barrel; not a charm hung on it, not
	// SMP proxies), its radius half the next longest
	// side (1.5 to 10 units), on that node so it follows the swing. After the fitting, which it takes no part in.
	void SkyrimSystemCreator::addWeaponCapsule(std::vector<CapsuleSegment>& capsules)
	{
		RE::NiNode* weapon = findObjectByName("Weapon");
		if (!weapon || !weapon->ChildEnd()) return;
		const RE::NiTransform toWeapon = weapon->world.Invert();
		RE::NiPoint3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
		int count = 0;
		std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* obj, int depth) {
			if (!obj || depth > 32) return;
			if (auto* g = castNiGeometry(obj)) {
				// Not a charm hung on it (skinned) or SMP proxies (hidden collision shapes): the weapon's own geometry.
				if (!g->data || !g->data->vertices || g->skinInstance || (g->m_name && !_strnicmp(g->m_name, "SMP ", 4))) return;
				const RE::NiTransform toLocal = toWeapon * g->world;
				const int n = g->data->vertexCount, stride = std::max(1, n / 500);
				for (int v = 0; v < n; v += stride) {
					const RE::NiPoint3 p = toLocal * g->data->vertices[v];
					lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
					hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
					++count;
				}
				return;
			}
			if (auto* node = obj->AsNode())
				for (uint16_t i = 0; i < node->ChildEnd(); ++i) walk(node->children[i], depth + 1);
		};
		for (uint16_t i = 0; i < weapon->ChildEnd(); ++i) walk(weapon->children[i], 0);
		if (count < 8) return;
		const float ext[3] = { hi.x - lo.x, hi.y - lo.y, hi.z - lo.z };
		const int axis = ext[0] >= ext[1] && ext[0] >= ext[2] ? 0 : (ext[1] >= ext[2] ? 1 : 2);
		const float second = std::max(ext[(axis + 1) % 3], ext[(axis + 2) % 3]);
		const float radius = std::clamp(second * 0.5f, 1.5f, 10.0f);
		if (ext[axis] < 4.0f) return;
		RE::NiPoint3 centre = (lo + hi) * 0.5f, a = centre, b = centre;
		const float half = std::max(0.0f, ext[axis] * 0.5f - radius);
		float* pa = &a.x, * pb = &b.x;
		pa[axis] -= half;
		pb[axis] += half;
		SkyrimBone* bone = getOrCreateBone("Weapon");
		if (!bone) return;
		capsules.push_back({ bone, weapon->world * a, weapon->world * b, radius, radius });
		logger::info("[SMP] stand-in: weapon capsule {:.0f} units long, radius {:.1f}", ext[axis], radius);
	}

	// New Vegas: a stand-in for an actor without SMP collision of its own (an NPC in plain clothes, a creature, a robot), for
	// placed objects' cloth to collide with: animated (kinematic) capsules on its bones, as one per-triangle body tagged
	// body, colliding with nothing but shapes tagged world (see ActorManager: every world object's shapes get it). A
	// humanoid (a Bip01 skeleton with legs and arms) gets its torso, thighs, shins, upper arms and forearms, and its shoulders,
	// feet and head where the skeleton has them; anything else
	// one upright capsule from its feet, sized by its bounding sphere, on its root. Built from nothing but the skeleton.
	RE::BSTSmartPointer<SkyrimSystem> SkyrimSystemCreator::createStandIn(RE::NiNode* actorRoot)
	{
		m_skeleton = actorRoot;
		m_model = actorRoot;
		m_reader = nullptr;
		m_boneTemplates.clear();   // the default template: mass 0, kinematic
		m_worldObject = true;      // bones by name anywhere under the root (it is the skeleton and the model)
		m_mesh = RE::make_smart<SkyrimSystem>(actorRoot);
		m_mesh->m_model = actorRoot;
		m_boneIndex.clear();
		m_extraBones.clear();
		m_createdNodes.clear();
		m_createdAnchors.clear();
		m_boundNodes.clear();
		m_listedShapes = 0;
		m_staleSlots = 0;

		const float scale = actorRoot->world.scale > 0.01f ? actorRoot->world.scale : 1.0f;
		// Radii for the game's human (about 128 units tall), at each end; optional ones are left out if a skeleton lacks them.
		struct Preset { const char* from; const char* to; float ra, rb; bool optional; };
		static const Preset kHumanoid[] = {
			{ "Bip01 Pelvis", "Bip01 Spine2", 14.0f, 14.0f, false }, { "Bip01 Spine2", "Bip01 Neck1", 14.0f, 7.0f, false },
			{ "Bip01 L Thigh", "Bip01 L Calf", 9.0f, 6.0f, false }, { "Bip01 L Calf", "Bip01 L Foot", 5.5f, 4.0f, false },
			{ "Bip01 R Thigh", "Bip01 R Calf", 9.0f, 6.0f, false }, { "Bip01 R Calf", "Bip01 R Foot", 5.5f, 4.0f, false },
			{ "Bip01 L UpperArm", "Bip01 L Forearm", 5.0f, 4.0f, false }, { "Bip01 L Forearm", "Bip01 L Hand", 4.0f, 3.3f, false },
			{ "Bip01 R UpperArm", "Bip01 R Forearm", 5.0f, 4.0f, false }, { "Bip01 R Forearm", "Bip01 R Hand", 4.0f, 3.3f, false },
			{ "Bip01 L Clavicle", "Bip01 L UpperArm", 6.0f, 5.5f, true }, { "Bip01 R Clavicle", "Bip01 R UpperArm", 6.0f, 5.5f, true },
			{ "Bip01 L Foot", "Bip01 L Toe0", 3.5f, 3.0f, true }, { "Bip01 R Foot", "Bip01 R Toe0", 3.5f, 3.0f, true },
		};
		std::vector<CapsuleSegment> capsules;
		bool humanoid = true;
		for (auto& p : kHumanoid)
			if (!p.optional && (!findObjectByName(p.from) || !findObjectByName(p.to))) { humanoid = false; break; }
		if (humanoid) {
			for (auto& p : kHumanoid) {
				auto* from = findObjectByName(p.from);
				auto* to = findObjectByName(p.to);
				if (!from || !to) continue;   // an optional one
				SkyrimBone* bone = getOrCreateBone(p.from);
				if (!bone) return nullptr;
				capsules.push_back({ bone, from->world.translate, to->world.translate, p.ra * scale, p.rb * scale });
			}
			// The head: from its bone up 10 units the way the neck points, radius 8.
			if (auto* head = findObjectByName("Bip01 Head"))
				if (auto* neck = findObjectByName("Bip01 Neck1"))
					if (SkyrimBone* bone = getOrCreateBone("Bip01 Head")) {
						RE::NiPoint3 up = head->world.translate - neck->world.translate;
						const float l = up.Length();
						up = l > 0.5f ? up * (1.0f / l) : RE::NiPoint3{ 0, 0, 1 };
						capsules.push_back({ bone, head->world.translate, head->world.translate + up * (10.0f * scale), 8.0f * scale, 8.0f * scale });
					}
			fitStandIn(actorRoot, capsules);
			addWeaponCapsule(capsules);
		} else {
			// The bounding sphere (NiAVObject's world bound, through its pointer at +0x20): its centre's height above the
			// root as half the height, a radius of under half the sphere's.
			const auto* bound = *reinterpret_cast<RE::NiBound* const*>(reinterpret_cast<const uint8_t*>(actorRoot) + 0x20);
			if (!bound || !(bound->radius > 1.0f) || !actorRoot->m_name) return nullptr;
			const RE::NiPoint3 base = actorRoot->world.translate;
			const float height = std::clamp((bound->center.z - base.z) * 2.0f, 20.0f, 600.0f);
			const float radius = std::clamp(bound->radius * 0.45f, 6.0f, std::max(6.0f, height * 0.5f));
			SkyrimBone* bone = getOrCreateBone(actorRoot->m_name);
			if (!bone) return nullptr;
			capsules.push_back({ bone, base + RE::NiPoint3{ 0, 0, radius }, base + RE::NiPoint3{ 0, 0, std::max(radius, height - radius) }, radius, radius });
		}

		auto body = RE::make_smart<SkyrimBody>();
		body->m_name = "SMP Stand-in";
		auto shape = RE::make_smart<PerTriangleShape>(body.get());
		AddCapsules(body.get(), shape.get(), capsules);
		shape->m_shapeProp.margin = 0.5f;
		shape->m_shapeProp.penetration = 3.0f;
		body->m_tags.push_back("body");
		body->m_canCollideWithTags.insert("world");
		body->finishBuild();
		m_mesh->m_meshes.push_back(body);
		body->m_mesh = m_mesh.get();
		m_mesh->m_skeleton = hdt::make_nismart(m_skeleton);
		std::sort(m_mesh->m_bones.begin(), m_mesh->m_bones.end(), [](const auto& a, const auto& b) {
			return static_cast<SkyrimBone*>(a.get())->m_depth < static_cast<SkyrimBone*>(b.get())->m_depth;
		});
		return m_mesh->valid() ? m_mesh : nullptr;
	}

	// New Vegas: <nv-virtual-legs fit="Legs" scale="0.9"> - smooth collision for the legs, in place of their
	// mesh. A capsule along each thigh (hip to knee) and shin (knee to ankle), a closed low-poly mesh skinned to
	// that bone, as one per-triangle body. Each radius is fitted to the shape(s) named in "fit" (comma
	// separated): the leg mesh's thinner side around that bone, times "scale". Slimmer than the trousers and
	// without their creases, cloth can collide right up to the hips without being caught inside them. Takes the
	// per-triangle shape's elements (margin, penetration, tag, no-collide-with-tag, can-collide-with-tag).
	RE::BSTSmartPointer<SkyrimBody> SkyrimSystemCreator::readVirtualLegs()
	{
		std::string fit = m_reader->getAttribute("fit", "");
		float scale = 0.9f;
		if (auto s = m_reader->getAttribute("scale", ""); !s.empty()) scale = (float)atof(s.c_str());

		// Tapered: a radius at each end (a thigh is wide at the hip, narrow at the knee), straight between.
		struct Segment { const char* from; const char* to; float fallbackA, fallbackB; SkyrimBone* bone = nullptr; RE::NiPoint3 a, b; std::vector<std::pair<float, float>> d; float ra = 0, rb = 0; };
		Segment segs[4] = {
			{ "Bip01 L Thigh", "Bip01 L Calf", 8.0f, 5.5f }, { "Bip01 L Calf", "Bip01 L Foot", 5.0f, 3.5f },
			{ "Bip01 R Thigh", "Bip01 R Calf", 8.0f, 5.5f }, { "Bip01 R Calf", "Bip01 R Foot", 5.0f, 3.5f },
		};
		// Distance from the segment, and where along it (0 at the joint it starts from, 1 at the next).
		auto segDist = [](const Segment& s, const RE::NiPoint3& p, float* along = nullptr) {
			RE::NiPoint3 ab = s.b - s.a, ap = p - s.a;
			float len2 = std::max(ab.x * ab.x + ab.y * ab.y + ab.z * ab.z, 1e-6f);
			float t = std::clamp((ap.x * ab.x + ap.y * ab.y + ap.z * ab.z) / len2, 0.0f, 1.0f);
			if (along) *along = t;
			return (p - (s.a + ab * t)).Length();
		};
		for (auto& s : segs) {
			auto* from = findObjectByName(s.from);
			auto* to = findObjectByName(s.to);
			if (!from || !to) {
				logger::warn("[SMP] nv-virtual-legs: no {} or {} in the skeleton, skipped", s.from, s.to);
				m_reader->skipCurrentElement();
				return nullptr;
			}
			s.bone = getOrCreateBone(s.from);
			if (!s.bone) {
				m_reader->skipCurrentElement();
				return nullptr;
			}
			s.a = from->world.translate;
			s.b = to->world.translate;
		}

		// Fit: every vertex of the named shapes, posed, to its nearest segment.
		std::stringstream names(fit);
		std::string name;
		while (std::getline(names, name, ',')) {
			while (!name.empty() && name.front() == ' ') name.erase(0, 1);
			while (!name.empty() && name.back() == ' ') name.pop_back();
			auto* g = castNiGeometry(findObject(m_model, name.c_str()));
			std::vector<std::vector<std::pair<int, float>>> weights;
			if (!g || !g->skinInstance || !g->skinInstance->skinData || !g->data || !g->data->vertices || !CollectWeights(g, weights)) {
				logger::warn("[SMP] nv-virtual-legs: cannot fit to '{}' (not a skinned shape kept in memory)", name);
				continue;
			}
			auto* skin = g->skinInstance;
			for (int v = 0; v < g->data->vertexCount; v++) {
				RE::NiPoint3 p{};
				for (auto& [slot, w] : weights[v])
					if (slot < (int)skin->boneCount && skin->bones[slot])
						p = p + (skin->bones[slot]->world * skin->skinData->boneData[slot].skinToBone) * g->data->vertices[v] * w;
				int best = 0;
				float bt = 0, bd = segDist(segs[0], p, &bt);
				for (int i = 1; i < 4; i++) {
					float t;
					if (float d = segDist(segs[i], p, &t); d < bd) { bd = d; bt = t; best = i; }
				}
				segs[best].d.push_back({ bt, bd });
			}
		}
		// Each end's radius: the leg's thinner side (the lower quartile of the distances) over the third of the
		// segment nearest that end, slimmed by the scale. Too few vertices: the whole segment's, else a default.
		auto quartile = [](std::vector<float> v) { std::sort(v.begin(), v.end()); return v[v.size() / 4]; };
		for (auto& s : segs) {
			std::vector<float> all, nearA, nearB;
			for (auto& [t, d] : s.d) {
				all.push_back(d);
				if (t < 0.33f) nearA.push_back(d);
				if (t > 0.67f) nearB.push_back(d);
			}
			if (all.size() < 20) { s.ra = s.fallbackA; s.rb = s.fallbackB; continue; }
			float whole = quartile(all);
			s.ra = (nearA.size() >= 10 ? quartile(nearA) : whole) * scale;
			s.rb = (nearB.size() >= 10 ? quartile(nearB) : whole) * scale;
		}

		auto body = RE::make_smart<SkyrimBody>();
		body->m_name = "SMP Virtual Legs";
		auto shape = RE::make_smart<PerTriangleShape>(body.get());
		std::vector<CapsuleSegment> capsules;
		for (auto& s : segs) {
			capsules.push_back({ s.bone, s.a, s.b, s.ra, s.rb });
			logger::info("[SMP] virtual legs: {} to {}, radius {:.1f} to {:.1f}{}", s.from, s.to, s.ra, s.rb, s.d.size() >= 20 ? "" : " (default, nothing fitted)");
		}
		AddCapsules(body.get(), shape.get(), capsules);
		shape->m_shapeProp.margin = 0.5f;
		shape->m_shapeProp.penetration = 3.0f;
		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto nodeName = m_reader->GetName();
				if (nodeName == "margin") shape->m_shapeProp.margin = m_reader->readFloat();
				else if (nodeName == "prenetration" || nodeName == "penetration") shape->m_shapeProp.penetration = m_reader->readFloat();
				else if (nodeName == "tag") body->m_tags.push_back(m_reader->readText());
				else if (nodeName == "no-collide-with-tag") body->m_noCollideWithTags.insert(m_reader->readText());
				else if (nodeName == "can-collide-with-tag") body->m_canCollideWithTags.insert(m_reader->readText());
				else { logger::warn("unknown element - {}", nodeName.c_str()); m_reader->skipCurrentElement(); }
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag) {
				break;
			}
		}
		m_deferredBuilds.push_back({ body.get(), nullptr });
		return body;
	}

	bool SkyrimSystemCreator::complete()
	{
		if (m_listedShapes && m_extraBones.empty() && !m_worldObject)
			return false;   // shapes list bones but none had a skin to bind them to
		int missing = 0;
		for (auto& [key, uses] : m_extraBones)
			if (!m_createdNodes.count(key) && !findObjectByName(uses.front().name.c_str()))
				++missing;
		if (missing)
			logger::warn("[SMP] {} of the item's bones got no node", missing);
		return missing == 0;
	}

	RE::BSTSmartPointer<SkyrimSystem> SkyrimSystemCreator::bindOnlySystem(RE::NiNode* skeleton, RE::NiAVObject* model)
	{
		m_skeleton = skeleton;
		m_model = model;
		m_reader = nullptr;
		m_boneTemplates.clear();   // the default template: mass 0, kinematic
		m_mesh = RE::make_smart<SkyrimSystem>(skeleton);
		m_mesh->m_model = model;
		m_boneIndex.clear();
		m_extraBones.clear();
		m_createdNodes.clear();
		m_createdAnchors.clear();
		m_boundNodes.clear();
		m_listedShapes = 0;
		m_staleSlots = 0;
		collectExtraBones(model);

		// Every bone the item lists that the skeleton lacks, with the default (mass 0, kinematic) template.
		// Parents first, so each bone's depth (its update order) comes after its anchor's.
		std::function<void(const std::string&)> create = [&](const std::string& name) {
			auto it = m_extraBones.find(Lower(name.c_str()));
			if (it == m_extraBones.end() || findBoneFromIndex(name.c_str()))
				return;
			if (!isItemBone(name) && !m_createdNodes.count(Lower(name.c_str())) && findObjectByName(name.c_str()))
				return;   // the skeleton has it
			create(it->second.front().parent);
			createBoneFromNodeName(name.c_str());
		};
		for (auto& [key, uses] : m_extraBones)
			create(uses.front().name);
		if (!m_mesh->valid())
			return nullptr;

		m_mesh->m_skeleton = hdt::make_nismart(m_skeleton);
		std::sort(m_mesh->m_bones.begin(), m_mesh->m_bones.end(), [](const auto& a, const auto& b) {
			return static_cast<SkyrimBone*>(a.get())->m_depth < static_cast<SkyrimBone*>(b.get())->m_depth;
		});
		logger::warn("[SMP] {}: no usable physics, {} of its bones placed at rest without simulation", model->GetName(), m_mesh->getBones().size());
		return m_mesh;
	}

	RE::BSTSmartPointer<ConstraintGroup> SkyrimSystemCreator::readConstraintGroup()
	{
		RE::BSTSmartPointer<ConstraintGroup> ret = RE::make_smart<ConstraintGroup>();

		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto name = m_reader->GetName();

				if (name == "generic-constraint") {
					auto constraint = readGenericConstraint();
					if (constraint)
						ret->m_constraints.push_back(constraint);
				} else if (name == "stiffspring-constraint") {
					auto constraint = readStiffSpringConstraint();
					if (constraint)
						ret->m_constraints.push_back(constraint);
				} else if (name == "conetwist-constraint") {
					auto constraint = readConeTwistConstraint();
					if (constraint)
						ret->m_constraints.push_back(constraint);
				} else if (name == "generic-constraint-default") {
					auto clsname = m_reader->getAttribute("name", "");
					auto extends = m_reader->getAttribute("extends", "");
					auto defaultGenericConstraintTemplate = getGenericConstraintTemplate(extends);
					readGenericConstraintTemplate(defaultGenericConstraintTemplate);
					m_genericConstraintTemplates[clsname] = defaultGenericConstraintTemplate;
				} else if (name == "stiffspring-constraint-default") {
					auto clsname = m_reader->getAttribute("name", "");
					auto extends = m_reader->getAttribute("extends", "");
					auto defaultStiffSpringConstraintTemplate = getStiffSpringConstraintTemplate(extends);
					readStiffSpringConstraintTemplate(defaultStiffSpringConstraintTemplate);
					m_stiffSpringConstraintTemplates[clsname] = defaultStiffSpringConstraintTemplate;
				} else if (name == "conetwist-constraint-default") {
					auto clsname = m_reader->getAttribute("name", "");
					auto extends = m_reader->getAttribute("extends", "");
					auto defaultConeTwistConstraintTemplate = getConeTwistConstraintTemplate(extends);
					readConeTwistConstraintTemplate(defaultConeTwistConstraintTemplate);
					m_coneTwistConstraintTemplates[clsname] = defaultConeTwistConstraintTemplate;
				} else {
					logger::warn("unknown element - {}", name.c_str());
					m_reader->skipCurrentElement();
				}
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
				break;
		}
		return ret;
	}

	void SkyrimSystemCreator::readBoneTemplate(BoneTemplate& cinfo)
	{
		bool clearCollide = true;
		bool clearSounds = true, clearHardSounds = true;   // a bone's own impact sounds replace its template's
		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto name = m_reader->GetName();
				if (name == "mass")
					cinfo.m_mass = m_reader->readFloat();
				else if (name == "inertia")
					cinfo.m_localInertia = m_reader->readVector3();
				else if (name == "centerOfMassTransform")
					cinfo.m_centerOfMassTransform = m_reader->readTransform();
				else if (name == "linearDamping")
					cinfo.m_linearDamping = m_reader->readFloat();
				else if (name == "angularDamping")
					cinfo.m_angularDamping = m_reader->readFloat();
				else if (name == "friction")
					cinfo.m_friction = m_reader->readFloat();
				else if (name == "rollingFriction")
					cinfo.m_rollingFriction = m_reader->readFloat();
				else if (name == "restitution")
					cinfo.m_restitution = m_reader->readFloat();
				else if (name == "margin-multiplier")
					cinfo.m_marginMultipler = m_reader->readFloat();
				else if (name == "shape") {
					auto shape = readShape();
					if (shape) {
						m_shapeRefs.push_back(shape);
						cinfo.m_collisionShape = shape.get();
					} else
						cinfo.m_collisionShape = BoneTemplate::emptyShape;
				} else if (name == "collision-filter")
					cinfo.m_collisionFilter = m_reader->readInt();
				else if (name == "can-collide-with-bone") {
					if (clearCollide) {
						cinfo.m_canCollideWithBone.clear();
						cinfo.m_noCollideWithBone.clear();
						clearCollide = false;
					}
					cinfo.m_canCollideWithBone.push_back(m_reader->readText());
				} else if (name == "no-collide-with-bone") {
					if (clearCollide) {
						cinfo.m_canCollideWithBone.clear();
						cinfo.m_noCollideWithBone.clear();
						clearCollide = false;
					}
					cinfo.m_noCollideWithBone.push_back(m_reader->readText());
				} else if (name == "gravity-factor") {
					cinfo.m_gravityFactor = btClamped(m_reader->readFloat(), 0.0f, 1.0f);
				} else if (name == "wind-factor") {
					cinfo.m_windFactor = std::max(m_reader->readFloat(), 0.0f);
				} else if (name == "ccd") {
					cinfo.m_ccd = m_reader->readInt() != 0;
				} else if (name == "impact-sound") {
					if (clearSounds) { cinfo.m_impactSounds.clear(); clearSounds = false; }
					cinfo.m_impactSounds.push_back(m_reader->readText());
				} else if (name == "impact-sound-hard") {
					if (clearHardSounds) { cinfo.m_impactSoundsHard.clear(); clearHardSounds = false; }
					cinfo.m_impactSoundsHard.push_back(m_reader->readText());
				} else if (name == "impact-speed") {
					cinfo.m_impactSpeed = std::max(m_reader->readFloat(), 0.0f);
				} else if (name == "impact-hard-speed") {
					cinfo.m_impactHardSpeed = std::max(m_reader->readFloat(), 0.0f);
				} else if (name == "impact-volume") {
					cinfo.m_impactVolume = std::clamp(m_reader->readFloat(), 0.0f, 1.0f);
				} else {
					logger::warn("unknown element - {}", name.c_str());
					m_reader->skipCurrentElement();
				}
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
				break;
		}
	}

	std::shared_ptr<btCollisionShape> SkyrimSystemCreator::readShape()
	{
		auto typeStr = m_reader->getAttribute("type");
		if (typeStr == "ref") {
			auto shapeName = m_reader->getAttribute("name");
			m_reader->skipCurrentElement();
			auto iter = m_shapes.find(shapeName);
			if (iter != m_shapes.end())
				return iter->second;
			logger::warn("unknown shape - {}", shapeName.c_str());
			return nullptr;
		}
		if (typeStr == "box") {
			btVector3 halfExtend(0, 0, 0);
			float margin = 0;
			while (m_reader->Inspect()) {
				if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
					auto name = m_reader->GetName();
					if (name == "halfExtend")
						halfExtend = m_reader->readVector3();
					else if (name == "margin")
						margin = m_reader->readFloat();
					else {
						logger::warn("unknown element - {}", name.c_str());
						m_reader->skipCurrentElement();
					}
				} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
					break;
			}
			auto ret = std::make_shared<btBoxShape>(halfExtend);
			ret->setMargin(margin);
			return ret;
		}
		if (typeStr == "sphere") {
			float radius = 0;
			while (m_reader->Inspect()) {
				if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
					auto name = m_reader->GetName();
					if (name == "radius")
						radius = m_reader->readFloat();
					else {
						logger::warn("unknown element - {}", name.c_str());
						m_reader->skipCurrentElement();
					}
				} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
					break;
			}
			return std::make_shared<btSphereShape>(radius);
		}
		if (typeStr == "capsule") {
			float radius = 0;
			float height = 0;
			while (m_reader->Inspect()) {
				if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
					auto name = m_reader->GetName();
					if (name == "radius")
						radius = m_reader->readFloat();
					else if (name == "height")
						height = m_reader->readFloat();
					else {
						logger::warn("unknown element - {}", name.c_str());
						m_reader->skipCurrentElement();
					}
				} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
					break;
			}
			return std::make_shared<btCapsuleShape>(radius, height);
		}
		if (typeStr == "hull") {
			float margin = 0;
			auto ret = std::make_shared<btConvexHullShape>();
			while (m_reader->Inspect()) {
				if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
					auto name = m_reader->GetName();
					if (name == "point")
						ret->addPoint(m_reader->readVector3(), false);
					else if (name == "margin")
						margin = m_reader->readFloat();
					else {
						logger::warn("unknown element - {}", name.c_str());
						m_reader->skipCurrentElement();
					}
				} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
					break;
			}
			ret->recalcLocalAabb();
			return ret->getNumPoints() ? ret : nullptr;
		}
		if (typeStr == "cylinder") {
			float height = 0;
			float radius = 0;
			float margin = 0;
			while (m_reader->Inspect()) {
				if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
					auto name = m_reader->GetName();
					if (name == "height")
						height = m_reader->readFloat();
					else if (name == "radius")
						radius = m_reader->readFloat();
					else if (name == "margin")
						margin = m_reader->readFloat();
					else {
						logger::warn("unknown element - {}", name.c_str());
						m_reader->skipCurrentElement();
					}
				} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
					break;
			}

			if (radius >= 0 && height >= 0) {
				auto ret = std::make_shared<btCylinderShape>(btVector3(radius, height, radius));
				ret->setMargin(margin);
				return ret;
			}
			return nullptr;
		}
		if (typeStr == "compound") {
			auto ret = std::make_shared<btCompoundShape>();
			while (m_reader->Inspect()) {
				if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
					if (m_reader->GetName() == "child") {
						btTransform tr;
						std::shared_ptr<btCollisionShape> shape;

						while (m_reader->Inspect()) {
							if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
								if (m_reader->GetName() == "transform") {
									tr = m_reader->readTransform();
								} else if (m_reader->GetName() == "shape") {
									shape = readShape();
								} else {
									logger::warn("unknown element - {}", m_reader->GetName().c_str());
									m_reader->skipCurrentElement();
								}
							} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
								break;
						}

						if (shape) {
							ret->addChildShape(tr, shape.get());
							m_shapeRefs.push_back(shape);
						}
					}
				} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
					break;
			}
			return ret->getNumChildShapes() ? ret : nullptr;
		}
		logger::warn("Unknown shape type {}", typeStr.c_str());
		return nullptr;
	}

	void SkyrimSystemCreator::readOrUpdateBone()
	{
		RE::BSFixedString name = getRenamedBone(m_reader->getAttribute("name"));
		if (findBoneFromIndex(name)) {
			logger::warn("Bone {} already exists, skipped", name.c_str());
			m_reader->skipCurrentElement();
			return;
		}

		RE::BSFixedString cls = m_reader->getAttribute("template", "");
		if (!createBoneFromNodeName(name, cls, true))
			m_reader->skipCurrentElement();
	}


	SkyrimBone* SkyrimSystemCreator::createBoneFromNodeName(const RE::BSFixedString& bodyName, const RE::BSFixedString& templateName, const bool readTemplate)
	{
		// A bone the item brings comes before the skeleton: the item's NIF may carry copies of
		// skeleton-named nodes, but a name listed in "SMP Bones" is the item's own.
		RE::NiNode* anchor = nullptr;
		RE::NiTransform anchorToNode;
		RE::NiNode* node = nullptr;
		bool extra = isItemBone(bodyName.c_str()) || (m_extraBones.count(Lower(bodyName.c_str())) && !findObjectByName(bodyName));
		if (extra)
			node = createExtraBoneNode(bodyName.c_str(), anchorToNode, anchor);
		else
			node = findObjectByName(bodyName);

		if (node) {
			if (auto existing = findBoneFromIndex(bodyName))   // created while building a child's anchor
				return existing;
			logger::info("Found node named {}, creating bone", bodyName.c_str());
			auto boneTemplate = getBoneTemplate(templateName);
			if (readTemplate)
				readBoneTemplate(boneTemplate);
			auto bone = new SkyrimBone(bodyName, node, this->m_skeleton, boneTemplate);
			if (extra && anchor) {
				bone->m_anchor = anchor;
				bone->m_anchorToNode = anchorToNode;
				int depth = 1;
				for (RE::NiAVObject* i = anchor; i; i = i->parent)
					++depth;
				if (auto* anchorBone = findBoneFromIndex(anchor->GetName()))
					depth = std::max(depth, anchorBone->m_depth + 1);
				bone->m_depth = depth;
			}
			bone->m_localToRig = boneTemplate.m_centerOfMassTransform;
			bone->m_rigToLocal = boneTemplate.m_centerOfMassTransform.inverse();
			bone->m_marginMultipler = boneTemplate.m_marginMultipler;
			bone->m_gravityFactor = boneTemplate.m_gravityFactor;
			bone->m_windFactor = boneTemplate.m_windFactor;
			bone->m_impactSounds = boneTemplate.m_impactSounds;
			bone->m_impactSoundsHard = boneTemplate.m_impactSoundsHard;
			bone->m_impactSpeed = boneTemplate.m_impactSpeed;
			bone->m_impactHardSpeed = boneTemplate.m_impactHardSpeed;
			bone->m_impactVolume = boneTemplate.m_impactVolume;
			bone->m_shapeCollides = boneTemplate.m_collisionFilter != 0 && boneTemplate.m_collisionShape &&
				boneTemplate.m_collisionShape->getShapeType() != EMPTY_SHAPE_PROXYTYPE;
			if (boneTemplate.m_ccd && bone->m_shapeCollides && !bone->m_rig.isStaticOrKinematicObject()) {
				btVector3 centre;
				btScalar radius;
				boneTemplate.m_collisionShape->getBoundingSphere(centre, radius);
				bone->m_rig.setCcdMotionThreshold(radius * 0.5f);
				bone->m_rig.setCcdSweptSphereRadius(radius * 0.5f);
			}
			bone->m_canCollideWithBone = boneTemplate.m_canCollideWithBone;
			bone->m_noCollideWithBone = boneTemplate.m_noCollideWithBone;

			bone->readTransform(RESET_PHYSICS);

			m_mesh->m_bones.push_back(hdt::make_smart(bone));
			indexBone(bone);
			return bone;
		}
		logger::warn("Node named {} doesn't exist, skipped, no bone created", bodyName.c_str());
		return nullptr;
	}

	std::pair<RE::BSTSmartPointer<SkyrimBody>, SkyrimSystemCreator::VertexOffsetMap> SkyrimSystemCreator::generateMeshBody(const std::string name, DefaultBBP::NameSet_t* names)
	{
		RE::BSTSmartPointer<SkyrimBody> body = RE::make_smart<SkyrimBody>();
		body->m_name = name;

		int vertexStart = 0;
		int boneStart = 0;

		VertexOffsetMap vertexOffsetMap;

		for (auto& meshName : *names) {
			auto* g = castNiGeometry(findObject(m_model, meshName.c_str()));
			// Not in the item, but read from its NIF (see FileMesh): its bones are the skeleton's, by name.
			if (auto fm = g ? m_fileMeshes.end() : m_fileMeshes.find(Lower(meshName)); fm != m_fileMeshes.end()) {
				const FileMesh& m = fm->second;
				std::vector<int> localToBody(m.bones.size(), -1);
				for (size_t b = 0; b < m.bones.size(); ++b) {
					// A bone of this system (the physics file's own, or an item bone already made), else the skeleton's.
					RE::BSFixedString boneName = m.bones[b].name.c_str();
					auto bone = static_cast<SkinnedMeshBone*>(findBoneFromIndex(boneName));
					RE::NiNode* node = bone ? nullptr : findObjectByName(boneName);
					if (!bone && !node) {
						logger::warn("Shape {} (from its NIF): bone {} neither in the physics nor in the skeleton, its vertices on it skipped", meshName.c_str(), m.bones[b].name);
						continue;
					}
					if (!bone) {
						auto defaultBoneInfo = getBoneTemplate("");
						auto newBone = new SkyrimBone(boneName, node, this->m_skeleton, defaultBoneInfo);
						m_mesh->m_bones.push_back(hdt::make_smart(newBone));
						indexBone(newBone);
						bone = newBone;
					}
					localToBody[b] = body->addBone(bone, convertNi(m.bones[b].skinToBone), BoundingSphere(convertNi(m.bones[b].center), m.bones[b].radius));
				}
				const int vertexCount = (int)m.vertices.size();
				body->m_vertices.resize(vertexStart + vertexCount);
				for (int j = 0; j < vertexCount; ++j) {
					auto& v = body->m_vertices[j + vertexStart];
					v.m_skinPos = convertNi(m.vertices[j]);
					auto w = m.weights[j];
					std::sort(w.begin(), w.end(), [](auto& a, auto& b) { return a.second > b.second; });
					float total = 0.0f;
					int n = 0;
					for (auto& [slot, weight] : w) {
						if (n == 4) break;
						if (slot < 0 || slot >= (int)localToBody.size() || localToBody[slot] < 0) continue;
						v.m_weight[n] = weight;
						v.m_boneIdx[n] = localToBody[slot];
						total += weight;
						++n;
					}
					for (int k = 0; k < n; ++k) v.m_weight[k] /= total > 0.0f ? total : 1.0f;
					for (int k = n; k < 4; ++k) { v.m_weight[k] = 0.0f; v.m_boneIdx[k] = n ? v.m_boneIdx[0] : 0; }
				}
				// Where it is now (its vertices skinned through the bones as they stand), relative to the skeleton's head:
				// a mesh from the NIF placed wrong, or two of them overlapping at rest, shows here.
				{
					RE::NiPoint3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
					RE::NiNode* head = findObjectByName("Bip01 Head");
					const RE::NiPoint3 headAt = head ? head->world.translate : RE::NiPoint3{};
					for (int j = 0; j < vertexCount; ++j) {
						RE::NiPoint3 p{};
						float total = 0;
						for (auto& [slot, weight] : m.weights[j]) {
							if (slot < 0 || slot >= (int)localToBody.size() || localToBody[slot] < 0) continue;
							auto* sb = static_cast<SkyrimBone*>(body->m_skinnedBones[localToBody[slot]].ptr);
							if (!sb || !sb->m_node) continue;
							p = p + (sb->m_node->world * m.bones[slot].skinToBone) * m.vertices[j] * weight;
							total += weight;
						}
						if (total <= 0) continue;
						p = p * (1.0f / total) - headAt;
						lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
						hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
					}
					logger::info("Shape {}: read from its NIF ({} vertices, {} triangles, {} bones), now at ({:.1f} {:.1f} {:.1f})..({:.1f} {:.1f} {:.1f}) from the head{}",
						meshName.c_str(), vertexCount, m.triangles.size(), m.bones.size(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, head ? "" : " (no head found: from the world origin)");
				}
				vertexOffsetMap.insert({ meshName, vertexStart });
				boneStart = static_cast<int>(body->m_skinnedBones.size());
				vertexStart = static_cast<int>(body->m_vertices.size());
				continue;
			}
			if (!g) {
				logger::warn("Shape {} not found in the item, skipped", meshName.c_str());
				continue;
			}
			auto* skin = g->skinInstance;
			if (!skin || !skin->skinData || !skin->bones) {
				logger::warn("Shape {} has no skin, skipped", meshName.c_str());
				continue;
			}
			if (!g->data || !g->data->vertices || !g->data->vertexCount) {
				logger::warn("Shape {} has no vertex positions in memory, skipped", meshName.c_str());
				continue;
			}
			// An smpdress --proxy collision stand-in is drawn by the game until now: hidden (app-culled, NiAVObject
			// flags at 0x30 bit 0) once the physics has it. Culled in the NIF, its collisions do not work in game.
			if (_strnicmp(meshName.c_str(), "SMP Proxy:", 10) == 0)
				*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(g) + 0x30) |= 1;

			RE::NiSkinData* skinData = skin->skinData;
			const uint32_t boneCount = std::min(skinData->boneCount, skin->boneCount);

			// Skin slots of the item's own bones get their nodes now, if the XML has not made them.
			for (uint32_t boneIdx = 0; boneIdx < boneCount; ++boneIdx) {
				auto extraName = skinSlotName(g, boneIdx);
				if (!extraName.empty() && !findBoneFromIndex(extraName.c_str()))
					getOrCreateBone(extraName.c_str());
			}

			std::vector<int> localToBody(boneCount, -1);
			for (uint32_t boneIdx = 0; boneIdx < boneCount; ++boneIdx) {
				auto* node = skin->bones[boneIdx];
				if (!node) {
					continue;
				}
				const auto& boneData = skinData->boneData[boneIdx];
				auto boundingSphere = BoundingSphere(convertNi(boneData.bound.center), boneData.bound.radius);
				RE::BSFixedString boneName = node->GetName();
				auto bone = static_cast<SkinnedMeshBone*>(findBoneFromIndex(boneName));
				if (!bone) {
					auto* boneNode = castNiNode(node);
					if (!boneNode)
						continue;
					auto defaultBoneInfo = getBoneTemplate("");
					auto newBone = new SkyrimBone(boneName, boneNode, this->m_skeleton, defaultBoneInfo);
					m_mesh->m_bones.push_back(hdt::make_smart(newBone));
					indexBone(newBone);
					bone = newBone;
					logger::info("Created bone {} added to body {}, created without default values", boneName.c_str(), name);
				}

				localToBody[boneIdx] = body->addBone(bone, convertNi(boneData.skinToBone), boundingSphere);
			}

			std::vector<std::vector<std::pair<int, float>>> weights;
			if (!CollectWeights(g, weights)) {
				logger::warn("Shape {} has no bone weights in memory, skipped", meshName.c_str());
				continue;
			}

			const int vertexCount = g->data->vertexCount;
			body->m_vertices.resize(vertexStart + vertexCount);
			for (int j = 0; j < vertexCount; ++j) {
				auto& v = body->m_vertices[j + vertexStart];
				v.m_skinPos = convertNi(g->data->vertices[j]);

				// The four strongest influences, renormalised, as the game's own skinning keeps them.
				auto& w = weights[j];
				std::sort(w.begin(), w.end(), [](auto& a, auto& b) { return a.second > b.second; });
				float total = 0.0f;
				int n = 0;
				for (auto& [slot, weight] : w) {
					if (n == 4)
						break;
					if (slot < 0 || slot >= (int)boneCount || localToBody[slot] < 0)
						continue;
					v.m_weight[n] = weight;
					v.m_boneIdx[n] = localToBody[slot];
					total += weight;
					++n;
				}
				for (int k = 0; k < n; ++k)
					v.m_weight[k] /= total > 0.0f ? total : 1.0f;
				for (int k = n; k < 4; ++k) {
					v.m_weight[k] = 0.0f;
					v.m_boneIdx[k] = n ? v.m_boneIdx[0] : 0;
				}
			}

			// A proxy's place now, as for one read from the NIF above (for comparing the two).
			if (_strnicmp(meshName.c_str(), "SMP Proxy:", 10) == 0) {
				RE::NiPoint3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
				RE::NiNode* head = findObjectByName("Bip01 Head");
				const RE::NiPoint3 headAt = head ? head->world.translate : RE::NiPoint3{};
				for (int j = 0; j < vertexCount; ++j) {
					RE::NiPoint3 p{};
					float total = 0;
					for (auto& [slot, weight] : weights[j]) {
						if (slot < 0 || slot >= (int)boneCount || !skin->bones[slot]) continue;
						p = p + (skin->bones[slot]->world * skinData->boneData[slot].skinToBone) * g->data->vertices[j] * weight;
						total += weight;
					}
					if (total <= 0) continue;
					p = p * (1.0f / total) - headAt;
					lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
					hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
				}
				logger::info("Shape {}: in the item ({} vertices), now at ({:.1f} {:.1f} {:.1f})..({:.1f} {:.1f} {:.1f}) from the head", meshName.c_str(), vertexCount, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
			}

			vertexOffsetMap.insert({ meshName, vertexStart });
			boneStart = static_cast<int>(body->m_skinnedBones.size());
			vertexStart = static_cast<int>(body->m_vertices.size());
		}

		if (0 == vertexStart) {
			m_reader->skipCurrentElement();
			return { nullptr, {} };
		}

		for (auto& i : body->m_vertices)
			i.sortWeight();

		return { body, vertexOffsetMap };
	}

	RE::BSTSmartPointer<SkyrimBody> SkyrimSystemCreator::readPerVertexShape(DefaultBBP::NameMap_t meshNameMap)
	{
		auto name = m_reader->getAttribute("name");
		auto it = meshNameMap.find(name);
		auto names = (it == meshNameMap.end()) ? DefaultBBP::NameSet_t({ name }) : it->second;

		auto body = generateMeshBody(name, &names).first;
		if (!body) {
			return nullptr;
		}

		auto shape = RE::make_smart<PerVertexShape>(body.get());

		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto nodeName = m_reader->GetName();
				if (nodeName == "priority") {
					logger::warn("priority is deprecated and no longer used");
					m_reader->skipCurrentElement();
				} else if (nodeName == "margin") {
					shape->m_shapeProp.margin = m_reader->readFloat();
				} else if (nodeName == "shared") {
					auto str = m_reader->readText();
					if (str == "public") {
						body->m_shared = SkyrimBody::SharedType::SHARED_PUBLIC;
					} else if (str == "internal") {
						body->m_shared = SkyrimBody::SharedType::SHARED_INTERNAL;
					} else if (str == "external") {
						body->m_shared = SkyrimBody::SharedType::SHARED_EXTERNAL;
					} else if (str == "private") {
						body->m_shared = SkyrimBody::SharedType::SHARED_PRIVATE;
					} else {
						logger::warn("unknown shared value, use default value \"public\"");
						body->m_shared = SkyrimBody::SharedType::SHARED_PUBLIC;
					}
				} else if (nodeName == "tag") {
					body->m_tags.push_back(m_reader->readText());
				} else if (nodeName == "can-collide-with-tag") {
					body->m_canCollideWithTags.insert(m_reader->readText());
				} else if (nodeName == "no-collide-with-tag") {
					body->m_noCollideWithTags.insert(m_reader->readText());
				} else if (nodeName == "can-collide-with-bone") {
					auto bone = getOrCreateBone(m_reader->readText());
					if (bone)
						body->m_canCollideWithBones.insert(bone);
				} else if (nodeName == "no-collide-with-bone") {
					auto bone = getOrCreateBone(m_reader->readText());
					if (bone)
						body->m_noCollideWithBones.insert(bone);
				} else if (nodeName == "weight-threshold") {
					auto boneName = m_reader->getAttribute("bone");
					float wt = m_reader->readFloat();
					for (int i = 0; i < body->m_skinnedBones.size(); ++i) {
						if (body->m_skinnedBones[i].ptr->m_name == getRenamedBone(boneName)) {
							body->m_skinnedBones[i].weightThreshold = wt;
							break;
						}
					}
				} else if (nodeName == "disable-tag") {
					body->m_disableTag = m_reader->readText();
				} else if (nodeName == "disable-priority") {
					body->m_disablePriority = m_reader->readInt();
				} else {
					logger::warn("unknown element - {}", name.c_str());
					m_reader->skipCurrentElement();
				}
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag) {
				break;
			}
		}

		m_deferredBuilds.push_back({ body.get(), shape.get() });

		return body;
	}

	RE::BSTSmartPointer<SkyrimBody> SkyrimSystemCreator::readPerTriangleShape(DefaultBBP::NameMap_t* meshNameMap)
	{
		auto name = m_reader->getAttribute("name");
		auto it = meshNameMap->find(name);
		auto names = (it == meshNameMap->end()) ? DefaultBBP::NameSet_t({ name }) : it->second;

		auto bodyData = generateMeshBody(name, &names);
		auto body = bodyData.first;
		auto vertexOffsetMap = bodyData.second;
		if (!body)
			return nullptr;

		auto shape = RE::make_smart<PerTriangleShape>(body.get());

		for (auto entry : vertexOffsetMap) {
			auto* g = castNiGeometry(findObject(m_model, entry.first.c_str()));
			if (!g) {
				if (auto fm = m_fileMeshes.find(Lower(entry.first)); fm != m_fileMeshes.end())   // read from its NIF
					for (auto& t : fm->second.triangles)
						shape->addTriangle(t[0] + entry.second, t[1] + entry.second, t[2] + entry.second);
				continue;
			}
			if (g->skinInstance) {
				// Gamebryo 2.x partitions index their own vertices; CollectTriangles maps them back.
				std::vector<std::array<int, 3>> triangles;
				CollectTriangles(g, triangles);
				if (triangles.empty()) {
					logger::warn("Shape {} has no triangles in memory, skipped", entry.first.c_str());
					return nullptr;
				}
				for (auto& t : triangles)
					shape->addTriangle(t[0] + entry.second, t[1] + entry.second, t[2] + entry.second);
			} else {
				logger::warn("Shape {} has no skin data, skipped", entry.first.c_str());
				return nullptr;
			}
		}

		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto nodeName = m_reader->GetName();
				if (nodeName == "priority") {
					logger::warn("priority is deprecated and no longer used");
					m_reader->skipCurrentElement();
				} else if (nodeName == "margin") {
					shape->m_shapeProp.margin = m_reader->readFloat();
				} else if (nodeName == "shared") {
					auto str = m_reader->readText();
					if (str == "public") {
						body->m_shared = SkyrimBody::SharedType::SHARED_PUBLIC;
					} else if (str == "internal") {
						body->m_shared = SkyrimBody::SharedType::SHARED_INTERNAL;
					} else if (str == "external") {
						body->m_shared = SkyrimBody::SharedType::SHARED_EXTERNAL;
					} else if (str == "private") {
						body->m_shared = SkyrimBody::SharedType::SHARED_PRIVATE;
					} else {
						logger::warn("unknown shared value, use default value \"public\"");
						body->m_shared = SkyrimBody::SharedType::SHARED_PUBLIC;
					}
				} else if (nodeName == "prenetration" || nodeName == "penetration") {
					shape->m_shapeProp.penetration = m_reader->readFloat();
				} else if (nodeName == "tag") {
					body->m_tags.push_back(m_reader->readText());
				} else if (nodeName == "no-collide-with-tag") {
					body->m_noCollideWithTags.insert(m_reader->readText());
				} else if (nodeName == "can-collide-with-tag") {
					body->m_canCollideWithTags.insert(m_reader->readText());
				} else if (nodeName == "can-collide-with-bone") {
					auto bone = getOrCreateBone(m_reader->readText());
					if (bone)
						body->m_canCollideWithBones.insert(bone);
				} else if (nodeName == "no-collide-with-bone") {
					auto bone = getOrCreateBone(m_reader->readText());
					if (bone)
						body->m_noCollideWithBones.insert(bone);
				} else if (nodeName == "weight-threshold") {
					auto boneName = m_reader->getAttribute("bone");
					float wt = m_reader->readFloat();
					for (int i = 0; i < body->m_skinnedBones.size(); ++i) {
						if (body->m_skinnedBones[i].ptr->m_name == getRenamedBone(boneName)) {
							body->m_skinnedBones[i].weightThreshold = wt;
						}
					}
				} else if (nodeName == "disable-tag") {
					body->m_disableTag = m_reader->readText();
				} else if (nodeName == "disable-priority") {
					body->m_disablePriority = m_reader->readInt();
				} else {
					logger::warn("unknown element - {}", nodeName.c_str());
					m_reader->skipCurrentElement();
				}
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag) {
				break;
			}
		}

		m_deferredBuilds.push_back({ body.get(), nullptr });

		return body;
	}

	void SkyrimSystemCreator::readFrameLerp(btTransform& tr)
	{
		tr.setIdentity();
		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto name = m_reader->GetName();
				if (name == "translationLerp")
					tr.getOrigin().setX(m_reader->readFloat());
				else if (name == "rotationLerp")
					tr.getOrigin().setY(m_reader->readFloat());
				else {
					logger::warn("unknown element - {}", name.c_str());
					m_reader->skipCurrentElement();
				}
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
				break;
		}
	}

	bool SkyrimSystemCreator::parseFrameType(const std::string& name, FrameType& frameType, btTransform& frame)
	{
		if (name == "frameInA") {
			frameType = FrameInA;
			frame = m_reader->readTransform();
		} else if (name == "frameInB") {
			frameType = FrameInB;
			frame = m_reader->readTransform();
		} else if (name == "frameInLerp") {
			frameType = FrameInLerp;
			readFrameLerp(frame);
		} else
			return false;
		return true;
	}

	void SkyrimSystemCreator::readGenericConstraintTemplate(GenericConstraintTemplate& dest)
	{
		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto name = m_reader->GetName();
				if (parseFrameType(name, dest.frameType, dest.frame))
					;
				else if (name == "enableLinearSprings")
					dest.enableLinearSprings = m_reader->readBool();
				else if (name == "enableAngularSprings")
					dest.enableAngularSprings = m_reader->readBool();
				else if (name == "linearStiffnessLimited")
					dest.linearStiffnessLimited = m_reader->readBool();
				else if (name == "angularStiffnessLimited")
					dest.angularStiffnessLimited = m_reader->readBool();

				else if (name == "springDampingLimited")
					dest.springDampingLimited = m_reader->readBool();
				else if (name == "linearNonHookeanDamping")
					dest.linearNonHookeanDamping = m_reader->readVector3();
				else if (name == "angularNonHookeanDamping")
					dest.angularNonHookeanDamping = m_reader->readVector3();
				else if (name == "linearNonHookeanStiffness")
					dest.linearNonHookeanStiffness = m_reader->readVector3();
				else if (name == "angularNonHookeanStiffness")
					dest.angularNonHookeanStiffness = m_reader->readVector3();

				else if (name == "linearMotors")
					dest.linearMotors = m_reader->readBool();
				else if (name == "angularMotors")
					dest.angularMotors = m_reader->readBool();
				else if (name == "linearServoMotors")
					dest.linearServoMotors = m_reader->readBool();
				else if (name == "angularServoMotors")
					dest.angularServoMotors = m_reader->readBool();
				else if (name == "linearTargetVelocity")
					dest.linearTargetVelocity = m_reader->readVector3();
				else if (name == "angularTargetVelocity")
					dest.angularTargetVelocity = m_reader->readVector3();
				else if (name == "linearMaxMotorForce")
					dest.linearMaxMotorForce = m_reader->readVector3();
				else if (name == "angularMaxMotorForce")
					dest.angularMaxMotorForce = m_reader->readVector3();

				else if (name == "stopERP")
					dest.stopERP = m_reader->readFloat();
				else if (name == "stopCFM")
					dest.stopCFM = m_reader->readFloat();
				else if (name == "motorERP")
					dest.motorERP = m_reader->readFloat();
				else if (name == "motorCFM")
					dest.motorCFM = m_reader->readFloat();

				else if (name == "useLinearReferenceFrameA")
					dest.useLinearReferenceFrameA = m_reader->readBool();
				else if (name == "linearLowerLimit")
					dest.linearLowerLimit = m_reader->readVector3();
				else if (name == "linearUpperLimit")
					dest.linearUpperLimit = m_reader->readVector3();
				else if (name == "angularLowerLimit")
					dest.angularLowerLimit = m_reader->readVector3();
				else if (name == "angularUpperLimit")
					dest.angularUpperLimit = m_reader->readVector3();
				else if (name == "linearStiffness")
					dest.linearStiffness = m_reader->readVector3();
				else if (name == "angularStiffness")
					dest.angularStiffness = m_reader->readVector3();
				else if (name == "linearDamping")
					dest.linearDamping = m_reader->readVector3();
				else if (name == "angularDamping")
					dest.angularDamping = m_reader->readVector3();
				else if (name == "linearEquilibrium")
					dest.linearEquilibrium = m_reader->readVector3();
				else if (name == "angularEquilibrium")
					dest.angularEquilibrium = m_reader->readVector3();
				else if (name == "linearBounce")
					dest.linearBounce = m_reader->readVector3();
				else if (name == "angularBounce")
					dest.angularBounce = m_reader->readVector3();
				else {
					logger::warn("unknown element - {}", name.c_str());
					m_reader->skipCurrentElement();
				}
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
				break;
		}
	}

	bool SkyrimSystemCreator::findBones(const RE::BSFixedString& bodyAName, const RE::BSFixedString& bodyBName, SkyrimBone*& bodyA, SkyrimBone*& bodyB)
	{
		bodyA = findBoneFromIndex(bodyAName);
		bodyB = findBoneFromIndex(bodyBName);

		if (!bodyA) {
			logger::warn("constraint {} <-> {} : bone for bodyA doesn't exist, will try to create it", bodyAName.c_str(), bodyBName.c_str());
			bodyA = createBoneFromNodeName(bodyAName);
			if (!bodyA) {
				m_reader->skipCurrentElement();
				return false;
			}
		}
		if (!bodyB) {
			logger::warn("constraint {} <-> {} : bone for bodyB doesn't exist, will try to create it", bodyAName.c_str(), bodyBName.c_str());
			bodyB = createBoneFromNodeName(bodyBName);
			if (!bodyB) {
				m_reader->skipCurrentElement();
				return false;
			}
		}
		if (bodyA == bodyB) {
			logger::warn("constraint between same object {} <-> {}, skipped", bodyAName.c_str(), bodyBName.c_str());
			m_reader->skipCurrentElement();
			return false;
		}

		if (bodyA->m_rig.isKinematicObject() && bodyB->m_rig.isKinematicObject()) {
			logger::warn("constraint between two kinematic object {} <-> {}, skipped", bodyAName.c_str(), bodyBName.c_str());
			m_reader->skipCurrentElement();
			return false;
		}

		logger::info("OK: constraint between object {} <-> {}", bodyAName.c_str(), bodyBName.c_str());
		return true;
	}

	btQuaternion rotFromAtoB(const btVector3& a, const btVector3& b)
	{
		auto axis = a.cross(b);
		if (axis.fuzzyZero())
			return btQuaternion::getIdentity();
		float sinA = axis.length();
		float cosA = a.dot(b);
		float angle = btAtan2(cosA, sinA);
		return btQuaternion(axis, angle);
	}

	void SkyrimSystemCreator::calcFrame(FrameType type, const btTransform& frame, const btQsTransform& trA, const btQsTransform& trB, btTransform& frameA, btTransform& frameB)
	{
		btQsTransform frameInWorld;
		switch (type) {
		case FrameInA:
			frameA = frame;
			frameInWorld = trA * frame;
			frameB = (trB.inverse() * frameInWorld).asTransform();
			break;
		case FrameInB:
			frameB = frame;
			frameInWorld = trB * frameB;
			frameA = (trA.inverse() * frameInWorld).asTransform();
			break;
		case FrameInLerp:
			{
				auto trans = trA.getOrigin().lerp(trB.getOrigin(), frame.getOrigin().x());
				auto rot = trA.getBasis().slerp(trB.getBasis(), frame.getOrigin().y());
				frameInWorld = btQsTransform(rot, trans);
				frameA = (trA.inverse() * frameInWorld).asTransform();
				frameB = (trB.inverse() * frameInWorld).asTransform();
				break;
			}
		case AWithXPointToB:
			{
				btMatrix3x3 matr(trA.getBasis());
				frameInWorld = trA;
				auto old = matr.getColumn(0).normalized();
				auto a2b = (trB.getOrigin() - trA.getOrigin()).normalized();
				auto q = rotFromAtoB(old, a2b);
				frameInWorld.getBasis() *= q;
				frameA = (trA.inverse() * frameInWorld).asTransform();
				frameB = (trB.inverse() * frameInWorld).asTransform();
				break;
			}
		case AWithYPointToB:
			{
				btMatrix3x3 matr(trA.getBasis());
				frameInWorld = trA;
				auto old = matr.getColumn(1).normalized();
				auto a2b = (trB.getOrigin() - trA.getOrigin()).normalized();
				auto q = rotFromAtoB(old, a2b);
				frameInWorld.getBasis() *= q;
				frameA = (trA.inverse() * frameInWorld).asTransform();
				frameB = (trB.inverse() * frameInWorld).asTransform();
				break;
			}
		case AWithZPointToB:
			{
				btMatrix3x3 matr(trA.getBasis());
				frameInWorld = trA;
				auto old = matr.getColumn(2).normalized();
				auto a2b = (trB.getOrigin() - trA.getOrigin()).normalized();
				auto q = rotFromAtoB(old, a2b);
				frameInWorld.getBasis() *= q;
				frameA = (trA.inverse() * frameInWorld).asTransform();
				frameB = (trB.inverse() * frameInWorld).asTransform();
				break;
			}
		}
	}

	RE::BSTSmartPointer<Generic6DofConstraint> SkyrimSystemCreator::readGenericConstraint()
	{
		auto bodyAName = getRenamedBone(m_reader->getAttribute("bodyA"));
		auto bodyBName = getRenamedBone(m_reader->getAttribute("bodyB"));
		auto clsname = m_reader->getAttribute("template", "");

		SkyrimBone *bodyA, *bodyB;
		if (!findBones(bodyAName, bodyBName, bodyA, bodyB))
			return nullptr;

		auto trA = bodyA->m_currentTransform;
		auto trB = bodyB->m_currentTransform;

		auto cinfo = getGenericConstraintTemplate(clsname);
		readGenericConstraintTemplate(cinfo);
		btTransform frameA, frameB;
		calcFrame(cinfo.frameType, cinfo.frame, trA, trB, frameA, frameB);

		RE::BSTSmartPointer<Generic6DofConstraint> constraint;
		if (cinfo.useLinearReferenceFrameA) {
			constraint = RE::make_smart<Generic6DofConstraint>(bodyB, bodyA, frameB, frameA);
		} else {
			constraint = RE::make_smart<Generic6DofConstraint>(bodyA, bodyB, frameA, frameB);
		}

		constraint->setLinearLowerLimit(cinfo.linearLowerLimit);
		constraint->setLinearUpperLimit(cinfo.linearUpperLimit);
		constraint->setAngularLowerLimit(cinfo.angularLowerLimit);
		constraint->setAngularUpperLimit(cinfo.angularUpperLimit);
		for (int i = 0; i < 3; ++i) {
			constraint->setStiffness(i, cinfo.linearStiffness[i], cinfo.linearStiffnessLimited);
			constraint->setStiffness(i + 3, cinfo.angularStiffness[i], cinfo.angularStiffnessLimited);
			constraint->setDamping(i, cinfo.linearDamping[i], cinfo.springDampingLimited);
			constraint->setDamping(i + 3, cinfo.angularDamping[i], cinfo.springDampingLimited);

			constraint->setEquilibriumPoint(i, cinfo.linearEquilibrium[i]);
			constraint->setEquilibriumPoint(i + 3, cinfo.angularEquilibrium[i]);

			constraint->setNonHookeanDamping(i, cinfo.linearNonHookeanDamping[i]);
			constraint->setNonHookeanDamping(i + 3, cinfo.angularNonHookeanDamping[i]);
			constraint->setNonHookeanStiffness(i, cinfo.linearNonHookeanStiffness[i]);
			constraint->setNonHookeanStiffness(i + 3, cinfo.angularNonHookeanStiffness[i]);

			constraint->enableSpring(i, cinfo.enableLinearSprings);
			constraint->enableSpring(i + 3, cinfo.enableAngularSprings);

			constraint->enableMotor(i, cinfo.linearMotors);
			constraint->enableMotor(i + 3, cinfo.angularMotors);
			constraint->setServo(i, cinfo.linearServoMotors);
			constraint->setServo(i + 3, cinfo.angularServoMotors);
			// TODO: Test if servo motors go to [0, 0, 0], or whatever equilibrium is.  Provide option to set server motor target.  Hard coded to equilibrium right now.
			constraint->setServoTarget(i, cinfo.linearEquilibrium[i]);
			constraint->setServoTarget(i + 3, cinfo.angularEquilibrium[i]);
			constraint->setTargetVelocity(i, cinfo.linearTargetVelocity[i]);
			constraint->setTargetVelocity(i + 3, cinfo.angularTargetVelocity[i]);
			constraint->setMaxMotorForce(i, cinfo.linearMaxMotorForce[i]);
			constraint->setMaxMotorForce(i + 3, cinfo.angularMaxMotorForce[i]);

			constraint->setParam(BT_CONSTRAINT_ERP, cinfo.motorERP, i);
			constraint->setParam(BT_CONSTRAINT_CFM, cinfo.motorCFM, i);
			constraint->setParam(BT_CONSTRAINT_STOP_ERP, cinfo.stopERP, i);
			constraint->setParam(BT_CONSTRAINT_STOP_CFM, cinfo.stopCFM, i);

			auto rotMotor = constraint->getRotationalLimitMotor(i);
			if (rotMotor) {
				rotMotor->m_motorERP = cinfo.motorERP;
				rotMotor->m_motorCFM = cinfo.motorCFM;
				rotMotor->m_stopERP = cinfo.stopERP;
				rotMotor->m_stopCFM = cinfo.stopCFM;
			}
		}
		constraint->getTranslationalLimitMotor()->m_bounce = cinfo.linearBounce;
		constraint->getRotationalLimitMotor(0)->m_bounce = cinfo.angularBounce[0];
		constraint->getRotationalLimitMotor(1)->m_bounce = cinfo.angularBounce[1];
		constraint->getRotationalLimitMotor(2)->m_bounce = cinfo.angularBounce[2];
		/*constraint->getTranslationalLimitMotor()->m_limitSoftness = 1;
		constraint->getRotationalLimitMotor(0)->m_limitSoftness = 1;
		constraint->getRotationalLimitMotor(1)->m_limitSoftness = 1;
		constraint->getRotationalLimitMotor(2)->m_limitSoftness = 1;*/

		return constraint;
	}

	void SkyrimSystemCreator::readStiffSpringConstraintTemplate(StiffSpringConstraintTemplate& dest)
	{
		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto name = m_reader->GetName();
				if (name == "minDistanceFactor")
					dest.minDistanceFactor = std::max(m_reader->readFloat(), 0.0f);
				else if (name == "maxDistanceFactor")
					dest.maxDistanceFactor = std::max(m_reader->readFloat(), 0.0f);
				else if (name == "stiffness")
					dest.stiffness = std::max(m_reader->readFloat(), 0.0f);
				else if (name == "damping")
					dest.damping = std::max(m_reader->readFloat(), 0.0f);
				else if (name == "equilibrium")
					dest.equilibriumFactor = btClamped(m_reader->readFloat(), 0.0f, 1.0f);
				else {
					logger::warn("unknown element - {}", name.c_str());
					m_reader->skipCurrentElement();
				}
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
				break;
		}
	}

	void SkyrimSystemCreator::readConeTwistConstraintTemplate(ConeTwistConstraintTemplate& dest)
	{
		while (m_reader->Inspect()) {
			if (m_reader->GetInspected() == XMLReader::Inspected::StartTag) {
				auto name = m_reader->GetName();
				if (parseFrameType(name, dest.frameType, dest.frame))
					;
				else if (name == "swingSpan1" || name == "coneLimit" || name == "limitZ")
					dest.swingSpan1 = std::max(m_reader->readFloat(), 0.f);
				else if (name == "swingSpan2" || name == "planeLimit" || name == "limitY")
					dest.swingSpan2 = std::max(m_reader->readFloat(), 0.f);
				else if (name == "twistSpan" || name == "twistLimit" || name == "limitX")
					dest.twistSpan = std::max(m_reader->readFloat(), 0.f);
				else if (name == "limitSoftness")
					dest.limitSoftness = btClamped(m_reader->readFloat(), 0.f, 1.f);
				else if (name == "biasFactor")
					dest.biasFactor = btClamped(m_reader->readFloat(), 0.f, 1.f);
				else if (name == "relaxationFactor")
					dest.relaxationFactor = btClamped(m_reader->readFloat(), 0.f, 1.f);
				else {
					logger::warn("unknown element - {}", name.c_str());
					m_reader->skipCurrentElement();
				}
			} else if (m_reader->GetInspected() == XMLReader::Inspected::EndTag)
				break;
		}
	}

	const SkyrimSystemCreator::BoneTemplate& SkyrimSystemCreator::getBoneTemplate(const RE::BSFixedString& name)
	{
		auto iter = m_boneTemplates.find(name);
		if (iter == m_boneTemplates.end())
			return m_boneTemplates[RE::BSFixedString()];
		return iter->second;
	}

	const SkyrimSystemCreator::GenericConstraintTemplate& SkyrimSystemCreator::getGenericConstraintTemplate(const RE::BSFixedString& name)
	{
		auto iter = m_genericConstraintTemplates.find(name);
		if (iter == m_genericConstraintTemplates.end())
			return m_genericConstraintTemplates[RE::BSFixedString()];
		return iter->second;
	}

	const SkyrimSystemCreator::StiffSpringConstraintTemplate& SkyrimSystemCreator::getStiffSpringConstraintTemplate(const RE::BSFixedString& name)
	{
		auto iter = m_stiffSpringConstraintTemplates.find(name);
		if (iter == m_stiffSpringConstraintTemplates.end())
			return m_stiffSpringConstraintTemplates[RE::BSFixedString()];
		return iter->second;
	}

	const SkyrimSystemCreator::ConeTwistConstraintTemplate& SkyrimSystemCreator::getConeTwistConstraintTemplate(const RE::BSFixedString& name)
	{
		auto iter = m_coneTwistConstraintTemplates.find(name);
		if (iter == m_coneTwistConstraintTemplates.end())
			return m_coneTwistConstraintTemplates[RE::BSFixedString()];
		return iter->second;
	}

	RE::BSTSmartPointer<StiffSpringConstraint> SkyrimSystemCreator::readStiffSpringConstraint()
	{
		auto bodyAName = getRenamedBone(m_reader->getAttribute("bodyA"));
		auto bodyBName = getRenamedBone(m_reader->getAttribute("bodyB"));
		auto clsname = m_reader->getAttribute("template", "");

		SkyrimBone *bodyA, *bodyB;
		if (!findBones(bodyAName, bodyBName, bodyA, bodyB))
			return nullptr;

		StiffSpringConstraintTemplate cinfo = getStiffSpringConstraintTemplate(clsname);
		readStiffSpringConstraintTemplate(cinfo);

		RE::BSTSmartPointer<StiffSpringConstraint> constraint = RE::make_smart<StiffSpringConstraint>(bodyA, bodyB);
		constraint->m_minDistance *= cinfo.minDistanceFactor;
		constraint->m_maxDistance *= cinfo.maxDistanceFactor;
		constraint->m_stiffness = cinfo.stiffness;
		constraint->m_damping = cinfo.damping;
		constraint->m_equilibriumPoint = constraint->m_minDistance * cinfo.equilibriumFactor + constraint->m_maxDistance * (1 - cinfo.equilibriumFactor);
		return constraint;
	}

	RE::BSTSmartPointer<ConeTwistConstraint> SkyrimSystemCreator::readConeTwistConstraint()
	{
		auto bodyAName = getRenamedBone(m_reader->getAttribute("bodyA"));
		auto bodyBName = getRenamedBone(m_reader->getAttribute("bodyB"));
		auto clsname = m_reader->getAttribute("template", "");

		SkyrimBone *bodyA = nullptr, *bodyB = nullptr;
		if (!findBones(bodyAName, bodyBName, bodyA, bodyB)) {
			return nullptr;
		}

		auto trA = bodyA->m_currentTransform;
		auto trB = bodyB->m_currentTransform;

		auto cinfo = getConeTwistConstraintTemplate(clsname);
		readConeTwistConstraintTemplate(cinfo);
		btTransform frameA, frameB;
		calcFrame(cinfo.frameType, cinfo.frame, trA, trB, frameA, frameB);

		RE::BSTSmartPointer<ConeTwistConstraint> constraint = RE::make_smart<ConeTwistConstraint>(bodyA, bodyB, frameA, frameB);
		constraint->setLimit(cinfo.swingSpan1, cinfo.swingSpan2, cinfo.twistSpan, cinfo.limitSoftness, cinfo.biasFactor, cinfo.relaxationFactor);

		return constraint;
	}
}
