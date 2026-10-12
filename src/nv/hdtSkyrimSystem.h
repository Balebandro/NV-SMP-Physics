#pragma once
// FSMP's SkyrimSystem and SkyrimSystemCreator for New Vegas: one actor's physics for one item,
// built from an SMP physics XML. The XML grammar, the templates and the constraint set-up are
// FSMP's; where the bones and the mesh data come from is New Vegas's (see the .cpp).
#include "hdtConvertNi.h"
#include "hdtDefaultBBP.h"
#include "hdtSkinnedMesh/hdtConeTwistConstraint.h"
#include "hdtSkinnedMesh/hdtGeneric6DofConstraint.h"
#include "hdtSkinnedMesh/hdtSkinnedMeshSystem.h"
#include "hdtSkinnedMesh/hdtStiffSpringConstraint.h"
#include "hdtSkyrimBody.h"
#include "hdtSkyrimBone.h"

namespace hdt
{
	class PerVertexShape;
	class SkyrimSystem : public SkinnedMeshSystem
	{
		friend class SkyrimSystemCreator;

	public:
		SkyrimSystem(RE::NiNode* skeleton);
		~SkyrimSystem() override;

		SkinnedMeshBone* findBone(const RE::BSFixedString& name);
		SkinnedMeshBody* findBody(const RE::BSFixedString& name);
		int findBoneIdx(const RE::BSFixedString& name);

		float prepareForRead(float timeStep) override;

		const std::vector<RE::BSTSmartPointer<SkinnedMeshBody>>& meshes() const { return m_meshes; }
		// For drawing the system (the previewer).
		const std::vector<RE::BSTSmartPointer<BoneScaleConstraint>>& constraints() const { return m_constraints; }
		const std::vector<RE::BSTSmartPointer<ConstraintGroup>>& constraintGroups() const { return m_constraintGroups; }

		RE::NiPointer<RE::NiNode> m_skeleton;
		RE::NiPointer<RE::NiNode> m_oldRoot;
		bool m_initialized = false;
		float m_windFactor = 1.f;

		btQuaternion m_lastRootRotation;
		btVector3 m_lastRootPosition;

		// New Vegas: the skin slots this system pointed at its own extra-bone nodes, put back on
		// the skeleton root when it goes (so nothing renders with a node that is about to be freed).
		struct SkinSlot
		{
			RE::NiPointer<RE::NiSkinInstance> skin;
			uint32_t slot;
			RE::NiAVObject* original = nullptr;   // what the game had there: no reference taken, it may be a stale pointer to a freed object
			RE::NiAVObject* bound = nullptr;   // the node this system put there: if the slot no longer holds it, the game re-pointed the skin
			RE::NiAVObject** array = nullptr;  // the skin's bone array then: if the skin has another one, the game rebound it
			// The slot still holds this system's node, in the array it was put in (else nothing of it may be touched).
			bool holds() const { return skin && skin->bones == array && slot < skin->boneCount && skin->bones[slot] == bound; }
		};
		std::vector<SkinSlot> m_boundSlots;

		// A node a system put in a skin slot it did not put back when it went (the item had left the actor): kept alive
		// for the session, since the game hands the skin back on the next equip still pointing at it. True for such a node.
		static bool isRetiredNode(const RE::NiAVObject* node);
		// Set before the system goes when the game detached the item (unequipped, rebound or unbound its skin): the
		// slots are then left alone and the nodes retired (putting the slots back then left the item unbound on the
		// next equip: the actor turned invisible). Not set (an item still worn: menu reset, settings): put back.
		bool m_itemDetached = false;
	};

	class XMLReader;

	class SkyrimSystemCreator
	{
	public:
		SkyrimSystemCreator();

		RE::BSTSmartPointer<SkyrimSystem> createOrUpdateSystem(RE::NiNode* skeleton, RE::NiAVObject* model, DefaultBBP::PhysicsFile_t* file, std::unordered_map<RE::BSFixedString, RE::BSFixedString>&& renameMap, SkyrimSystem* old_system);

		// After a build: whether every bone the item lists got its node. An item caught halfway through
		// the game attaching it can be missing them; the caller builds it again a little later.
		bool complete();

		// New Vegas: scales every weight-threshold the physics file sets (the menu's waist collision: 1 -
		// waist). Applied at build, so FSMP drops exactly the colliders that cannot collide.
		float m_weightThresholdScale = 1.0f;

		// New Vegas: a ground plane at the actor's feet for the cloth to rest on (see addGroundPlane), raised
		// or lowered by the offset (game units).
		bool m_ground = true;
		float m_groundOffset = 0.0f;

	protected:
		struct PooledStringHash
		{
			size_t operator()(const char* p) const noexcept { return reinterpret_cast<size_t>(p); }
		};

		struct PooledStringEqual
		{
			bool operator()(const char* a, const char* b) const noexcept { return a == b; }
		};

		std::unordered_map<const char*, SkyrimBone*, PooledStringHash, PooledStringEqual> m_boneIndex;

		void indexBone(SkyrimBone* bone);
		SkyrimBone* findBoneFromIndex(const RE::BSFixedString& name) const;

		struct DeferredBuild
		{
			SkinnedMeshBody* body;
			PerVertexShape* vertexShape;
		};

		std::vector<DeferredBuild> m_deferredBuilds;

		struct BoneTemplate : public btRigidBody::btRigidBodyConstructionInfo
		{
			static btEmptyShape emptyShape[1];

			BoneTemplate() :
				btRigidBodyConstructionInfo(0, nullptr, emptyShape)
			{
				m_centerOfMassTransform = btTransform::getIdentity();
				m_marginMultipler = 1.f;
			}

			std::shared_ptr<btCollisionShape> m_shape;
			std::vector<RE::BSFixedString> m_canCollideWithBone;
			std::vector<RE::BSFixedString> m_noCollideWithBone;
			btTransform m_centerOfMassTransform;
			float m_marginMultipler;
			float m_gravityFactor = 1.0f;
			float m_windFactor = 1.0f;
			U32 m_collisionFilter = 0;
		};

		enum FrameType
		{
			FrameInA,
			FrameInB,
			FrameInLerp,
			AWithXPointToB,
			AWithYPointToB,
			AWithZPointToB
		};

		struct GenericConstraintTemplate
		{
			FrameType frameType = FrameInB;
			bool useLinearReferenceFrameA = false;
			btTransform frame = btTransform::getIdentity();
			btVector3 linearLowerLimit = btVector3(1, 1, 1);
			btVector3 linearUpperLimit = btVector3(-1, -1, -1);
			btVector3 angularLowerLimit = btVector3(1, 1, 1);
			btVector3 angularUpperLimit = btVector3(-1, -1, -1);
			btVector3 linearStiffness = btVector3(0, 0, 0);
			btVector3 angularStiffness = btVector3(0, 0, 0);
			btVector3 linearDamping = btVector3(0, 0, 0);
			btVector3 angularDamping = btVector3(0, 0, 0);
			btVector3 linearEquilibrium = btVector3(0, 0, 0);
			btVector3 angularEquilibrium = btVector3(0, 0, 0);
			btVector3 linearBounce = btVector3(0, 0, 0);
			btVector3 angularBounce = btVector3(0, 0, 0);
			bool enableLinearSprings = true;
			bool enableAngularSprings = true;
			bool linearStiffnessLimited = true;
			bool angularStiffnessLimited = true;
			bool springDampingLimited = true;
			bool linearMotors = false;
			bool angularMotors = false;
			bool linearServoMotors = false;
			bool angularServoMotors = false;
			btVector3 linearNonHookeanDamping = btVector3(0, 0, 0);
			btVector3 angularNonHookeanDamping = btVector3(0, 0, 0);
			btVector3 linearNonHookeanStiffness = btVector3(0, 0, 0);
			btVector3 angularNonHookeanStiffness = btVector3(0, 0, 0);
			btVector3 linearTargetVelocity = btVector3(0, 0, 0);
			btVector3 angularTargetVelocity = btVector3(0, 0, 0);
			btVector3 linearMaxMotorForce = btVector3(0, 0, 0);
			btVector3 angularMaxMotorForce = btVector3(0, 0, 0);
			btScalar motorERP = 0.9f;
			btScalar motorCFM = 0;
			btScalar stopERP = 0.2f;
			btScalar stopCFM = 0;
		};

		struct StiffSpringConstraintTemplate
		{
			float minDistanceFactor = 1;
			float maxDistanceFactor = 1;
			float stiffness = 0;
			float damping = 0;
			float equilibriumFactor = 0.5;
		};

		struct ConeTwistConstraintTemplate
		{
			btTransform frame = btTransform::getIdentity();
			FrameType frameType = FrameInB;
			float swingSpan1 = 0;
			float swingSpan2 = 0;
			float twistSpan = 0;
			float limitSoftness = 1.0f;
			float biasFactor = 0.3f;
			float relaxationFactor = 1.0f;
		};

		using VertexOffsetMap = std::unordered_map<std::string, int>;

		RE::BSFixedString getRenamedBone(const RE::BSFixedString& name);

		RE::BSTSmartPointer<SkyrimSystem> m_mesh;
		RE::NiNode* m_skeleton;
		RE::NiAVObject* m_model;
		XMLReader* m_reader;
		std::unordered_map<RE::BSFixedString, RE::BSFixedString> m_renameMap;

		RE::NiNode* findObjectByName(const RE::BSFixedString& name);
		SkyrimBone* getOrCreateBone(const RE::BSFixedString& name);

		std::string m_filePath;

		std::unordered_map<RE::BSFixedString, BoneTemplate> m_boneTemplates;
		std::unordered_map<RE::BSFixedString, GenericConstraintTemplate> m_genericConstraintTemplates;
		std::unordered_map<RE::BSFixedString, StiffSpringConstraintTemplate> m_stiffSpringConstraintTemplates;
		std::unordered_map<RE::BSFixedString, ConeTwistConstraintTemplate> m_coneTwistConstraintTemplates;
		std::unordered_map<RE::BSFixedString, std::shared_ptr<btCollisionShape>> m_shapes;
		std::vector<std::shared_ptr<btCollisionShape>> m_shapeRefs;

		std::pair<RE::BSTSmartPointer<SkyrimBody>, VertexOffsetMap> generateMeshBody(const std::string name, DefaultBBP::NameSet_t* names);

		bool findBones(const RE::BSFixedString& bodyAName, const RE::BSFixedString& bodyBName, SkyrimBone*& bodyA, SkyrimBone*& bodyB);
		bool parseFrameType(const std::string& name, FrameType& type, btTransform& frame);
		static void calcFrame(FrameType type, const btTransform& frame, const btQsTransform& trA, const btQsTransform& trB, btTransform& frameA, btTransform& frameB);
		void readFrameLerp(btTransform& tr);
		void readBoneTemplate(BoneTemplate& dest);
		void readGenericConstraintTemplate(GenericConstraintTemplate& dest);
		void readStiffSpringConstraintTemplate(StiffSpringConstraintTemplate& dest);
		void readConeTwistConstraintTemplate(ConeTwistConstraintTemplate& dest);

		const BoneTemplate& getBoneTemplate(const RE::BSFixedString& name);
		const GenericConstraintTemplate& getGenericConstraintTemplate(const RE::BSFixedString& name);
		const StiffSpringConstraintTemplate& getStiffSpringConstraintTemplate(const RE::BSFixedString& name);
		const ConeTwistConstraintTemplate& getConeTwistConstraintTemplate(const RE::BSFixedString& name);

		SkyrimBone* createBoneFromNodeName(const RE::BSFixedString& bodyName, const RE::BSFixedString& templateName = "", const bool readTemplate = false);
		void readOrUpdateBone();
		RE::BSTSmartPointer<SkyrimBody> readPerVertexShape(DefaultBBP::NameMap_t meshNameMap);
		RE::BSTSmartPointer<SkyrimBody> readPerTriangleShape(DefaultBBP::NameMap_t* meshNameMap);
		RE::BSTSmartPointer<Generic6DofConstraint> readGenericConstraint();
		RE::BSTSmartPointer<StiffSpringConstraint> readStiffSpringConstraint();
		RE::BSTSmartPointer<ConeTwistConstraint> readConeTwistConstraint();
		RE::BSTSmartPointer<ConstraintGroup> readConstraintGroup();
		std::shared_ptr<btCollisionShape> readShape();

		// --- New Vegas: bones the garment brings that the skeleton lacks ---------------------------
		// Each skinned shape of the item lists its skin slots' bone names (and parents) in an
		// "SMP Bones" string extra data: "name|parent;name|parent;..." in skin-instance order, as the
		// authoring tool writes it. The game re-points the skin slots it can find in the skeleton
		// and leaves the rest on the skeleton root, so these names are the only record of them.
		struct ExtraBone
		{
			RE::NiGeometry* geometry;
			uint32_t slot;
			std::string name;     // as written (bone names are case-sensitive in the physics)
			std::string parent;
		};
		std::unordered_map<std::string, std::vector<ExtraBone>> m_extraBones;   // by bone name (lower case)
		std::unordered_map<std::string, RE::NiNode*> m_createdNodes;            // this build's extra-bone nodes
		int m_listedShapes = 0;                                                 // shapes with "SMP Bones"
		int m_staleSlots = 0;                                                   // item-bone slots that held a pointer other than the skeleton root
		void collectExtraBones(RE::NiAVObject* obj, int depth = 0);
		// When the physics file is missing or broken: the item's own bones alone, kinematic, carried
		// on their parents, so it renders at rest instead of stretched toward the skeleton root.
		RE::BSTSmartPointer<SkyrimSystem> bindOnlySystem(RE::NiNode* skeleton, RE::NiAVObject* model);
		void addGroundPlane();
		RE::BSTSmartPointer<SkyrimBody> readVirtualLegs();
		RE::NiNode* createExtraBoneNode(const std::string& name, RE::NiTransform& anchorToNode, RE::NiNode*& anchor);
		std::string skinSlotName(RE::NiGeometry* geometry, uint32_t slot);
	};
}
