#pragma once
// Which actors and items have physics. Each frame it walks the player's and the high-process
// actors' 3D for items carrying an SMP physics file (the "HDT Skinned Mesh Physics Object" string
// extra data, on the item's root node or on its shapes, as in Skyrim), builds a system for each new
// one, and drops the systems of items that were unequipped or of actors that left range.
#include "hdtSkyrimSystem.h"

#include <map>
#include <memory>
#include <tuple>
#include <unordered_set>

namespace nvsmp
{
	struct ActorStats
	{
		int actors = 0;        // actors with physics items in range
		int systems = 0;       // systems in the world
		int failed = 0;        // items whose physics file could not be built (see the log)
	};

	class ActorManager
	{
	public:
		static ActorManager* instance();

		// Once per frame, before the world steps.
		void update();
		// Drops every system (settings changed, menu reset, game load).
		void clear();
		// Writes each system's state to the log (see the .cpp).
		void report();
		// After a settings change: marks every system for a rebuild if a built-in collision setting changed.
		void applyCollision();
		// Once per frame, before update: does that rebuild.
		void rebuildIfPending();
		// Once per frame, after the world steps: race hair skinned on the CPU (see CpuSkin).
		void skinOnCpu();

		ActorStats stats;

	private:
		struct Item
		{
			RE::NiPointer<RE::NiNode> skeleton;   // the actor's 3D root
			RE::NiPointer<RE::NiAVObject> model;  // the item's root under it
			std::string file;
			RE::BSTSmartPointer<hdt::SkyrimSystem> system;
			unsigned frame = 0;                   // last frame it was seen
			unsigned buildAt = 0;                 // frame of the next build
			int builds = 0;
			bool failed = false;
			bool waiting = false;                 // the game has not bound its skin yet
			bool firstPerson = false;             // on the player's first-person model
			bool worldObject = false;             // a placed object's own 3D (a cloth on a wall), not an actor's (see scanWorldObjects)
			std::vector<RE::NiTransform> worldRest;   // a world object's bones at rest, in its root's frame (for the stats), by bone index
			float jointGapPhys = 0, jointGapDrawn = 0;   // the widest a constraint's two pivots were apart since the last report
			hdt::DefaultBBP::NameMap_t names;     // race hair: the physics file's shape names -> the game's (see RaceHairFile)
			std::unordered_map<std::string, std::string> boneLists;   // and the "SMP Bones" lists the game stripped
			// Race hair is drawn without skinning (see CpuSkin): its shapes skinned here, their skins held aside.
			struct CpuSkin
			{
				RE::NiPointer<RE::NiAVObject> shape;
				RE::NiPointer<RE::NiSkinInstance> skin;
				std::vector<RE::NiPoint3> bind;      // the vertices as the game made them (its slider morphs included)
				std::vector<RE::NiPoint3> written;   // what was written last frame: anything else is a new morph from the game
				bool unskinnedFlag = false;          // the Skinned shader flag was cleared (given back with the skin)
				// The face sliders, applied here (the game does not reshape a skinned hair): the file's vertices, its .egm,
				// how the NPC's values are read (-1: not found to rebuild what the game drew), and the values last used.
				std::vector<RE::NiPoint3> fileBase;
				std::shared_ptr<struct Egm> egm;
				int symWay = -1, asymWay = -1;
				float sym[50] = {}, asym[30] = {};
				int colourWay = -1;                  // the NPC's hair colour into the tint: 0 RGB, 1 BGR (-1: no match)
			};
			std::vector<CpuSkin> cpuSkins;
			void* npc = nullptr;                  // race hair: the actor's TESNPC (face values, hair colour)
			std::string hairModel;                // and its hair's model path
			bool headPart = false;                // a head part (hairModel is its model path), not race hair (HeadPart)
			bool raceHead = false;                // a race's head: skinned and morphed by the game, as any item (no CpuSkin)
		};

		static constexpr unsigned kSettleFrames = 10, kRetryFrames = 30;
		static constexpr int kMaxBuilds = 4;

		void build(Item& item);
		static void ReleaseCpuSkins(Item& item);
		static bool rebound(const Item& item);   // the game re-pointed a skin slot the system had bound
		static bool skinsBound(RE::NiAVObject* obj, int depth = 0);
		void scanActor(RE::NiNode* root, void* ref);
		void scan(RE::NiNode* root, RE::NiAVObject* obj, int depth);
		void found(RE::NiNode* root, RE::NiAVObject* model, const char* file, const hdt::DefaultBBP::NameMap_t* names = nullptr,
			const std::unordered_map<std::string, std::string>* lists = nullptr, RE::NiAVObject* hairShape = nullptr,
			const char* headPartModel = nullptr,    // a head part's: keyed by its shape (hairShape), not the FaceGen node
			bool raceHead = false);                 // a race's head (see RaceHead): keyed by its shape too

		std::map<std::pair<void*, void*>, Item> m_items;   // by (skeleton, model)
		unsigned m_frame = 0;
		void* m_scanRef = nullptr;   // the actor whose 3D scanActor is walking (its hair form, for race hair)
		bool m_raceMenuWhenScanned = false;   // for the log line when the race menu closes
		unsigned m_hatSwapAt = 0;             // the frame the game's hat swap runs again after the race menu closed (0: none)
		bool m_hatSwapAfterBuild = false;     // and once more when the player's race hair is built in game after it
		static constexpr unsigned kHatSwapDelay = 1;    // the frame after: the hair copies are settled by then
		// The race menu rebuilds the head whenever the hair or the face changes: race hair is not held there (no system, no
		// references kept between frames), only placed on the head each frame from what the scan found (skinMenuHair).
		bool m_raceMenu = false;
		struct MenuHair { RE::NiAVObject* shape; RE::NiNode* root; void* npc; std::string model; };
		std::vector<MenuHair> m_menuHair;   // this frame's, found by the scan
		void skinMenuHair();
		// The player's race hair physics in the race menu: running on its own bone nodes, holding nothing of the hair (its
		// skin slots released), for whichever copy of the hair the menu shows (see skinMenuHair).
		RE::BSTSmartPointer<hdt::SkyrimSystem> m_menuSystem;
		std::string m_menuModel;                                       // the hair it was built for
		std::unordered_map<std::string, RE::NiAVObject*> m_menuBones;   // its bones by name, lower case
		unsigned m_menuMissing = 0;                                    // frames its hair was not seen
		// Its skeleton: stand-ins of the player's skeleton nodes (the plugin's own, by name, lower case), given the real
		// ones' places every frame (the race menu rebuilds the player's 3D: nothing of it may be held).
		RE::NiPointer<RE::NiNode> m_menuRoot;
		std::unordered_map<std::string, RE::NiNode*> m_menuProxies;
		void refreshMenuProxies(RE::NiNode* playerRoot);
		void dropMenuSystem(const char* why);
		bool buildMenuSystem(RE::NiNode* root, RE::NiAVObject* shape, const std::string& model);
		// Placed objects with physics (statics, clutter: their NIF's root carries the physics file), within kWorldObjectReach
		// of the player in the loaded cells: looked for every kWorldObjectRescan frames, seen every frame while their 3D is
		// in the scene. Each is an item on its own root.
		static constexpr unsigned kWorldObjectRescan = 30;
		static constexpr float kWorldObjectReach = 4096.0f;
		struct WorldObject { RE::NiPointer<RE::NiNode> root; std::string file; };
		std::vector<WorldObject> m_worldObjects;
		unsigned m_worldObjectsAt = 0;
		bool m_scanWorldObject = false;   // found() is given a world object
		void scanWorldObjects(const RE::NiPoint3& playerPos);
		// Actors' collision stand-ins for world objects' cloth (SkyrimSystemCreator::createStandIn): only while a world object's
		// physics is built, only for actors within kStandInReach of one (whatever SMP items they wear), at most
		// kStandInBuildsPerFrame made a frame; dropped when the actor leaves that reach or its 3D goes.
		static constexpr float kStandInReach = 1500.0f;
		static constexpr int kStandInBuildsPerFrame = 2;
		// Its actor's "Weapon" node and the model under it when built: another model there (drawn, swapped) rebuilds it.
		struct StandIn { RE::BSTSmartPointer<hdt::SkyrimSystem> system; unsigned frame = 0; RE::NiPointer<RE::NiNode> weaponNode; const void* weapon = nullptr; };
		std::map<void*, StandIn> m_standIns;   // by actor 3D root
		void updateStandIns(const std::vector<std::tuple<float, RE::NiNode*, void*>>& actors);
		// Projectiles through world objects' cloth: a bone within kProjectileRadius of a projectile's path since the last frame
		// gets up to kProjectilePush (units/s) along it; paths longer than kProjectileMaxStep are not the same projectile, and
		// cloth farther than kProjectileReach from the path is not looked at.
		// The radius is wider than a cloth's bone spacing (a drape's are about 23 across, 57 down): a shot anywhere through the
		// cloth reaches a bone.
		static constexpr float kProjectileRadius = 45.0f, kProjectilePush = 900.0f, kProjectileMaxStep = 3000.0f, kProjectileReach = 400.0f;
		std::unordered_map<void*, RE::NiPoint3> m_projectileLast;   // each projectile's place last frame
		unsigned m_projectileHits = 0;                              // for the log
		void pushByProjectiles(const std::vector<void*>& projectiles);
		// Explosions: a bone within kExplosionReach times the explosion's radius gets up to kExplosionPush (units/s) away from its
		// centre, once, the first frame the explosion is seen; cloth whose root is farther than that plus kExplosionItemSize is
		// not looked at.
		static constexpr float kExplosionReach = 1.5f, kExplosionPush = 4000.0f, kExplosionItemSize = 400.0f;
		std::unordered_set<void*> m_explosionsSeen;   // last frame's explosions (each pushes once)
		unsigned m_explosionHits = 0;                 // for the log
		void pushByExplosions(const std::vector<void*>& explosions);
		// The game's grab (Z) on simulated bones (see grabBones): a bone within kGrabRadius of the line of sight and kGrabReach
		// along it is taken, then pulled each frame toward where it was taken along the view (its velocity kGrabPull times the
		// gap per second, at most kGrabMaxSpeed units/s).
		static constexpr float kGrabReach = 300.0f, kGrabRadius = 12.0f, kGrabPull = 12.0f, kGrabMaxSpeed = 3000.0f;
		const void* m_grabSystem = nullptr;   // the held bone's system (only compared: used while still an item's)
		size_t m_grabBone = 0;                // and its index there
		float m_grabDistance = 0;             // how far along the view it is held
		bool m_gameGrabbing = false;          // the game's own grab held something last frame
		void grabBones(RE::NiNode* playerRoot);
		// Head parts with physics in the race menu, as race hair: the player's copies (skin off, see g_menuHeadPartSkins)
		// skinned each frame (skinMenuHeadParts) on a system of their own per head part file, on the same stand-in skeleton.
		struct MenuHeadPart { RE::NiAVObject* shape; RE::NiNode* root; std::string model, key; };   // key: its lower-case game name
		std::vector<MenuHeadPart> m_menuHeadParts;   // this frame's, found by the scan
		struct MenuPartSystem
		{
			RE::BSTSmartPointer<hdt::SkyrimSystem> system;
			std::unordered_map<std::string, RE::NiAVObject*> bones;   // its bones by name, lower case
			unsigned missing = 0, lastTry = 0;
		};
		std::map<std::string, MenuPartSystem> m_menuPartSystems;   // by head part model
		void skinMenuHeadParts();
		bool buildMenuHeadPart(const MenuHeadPart& h, MenuPartSystem& out);
		void dropMenuHeadParts(const char* why);
		// The player's race head physics in the race menu, as a head part's (its slots released): the face copies the menu
		// shows have their own bones' slot nodes put on its bones each frame (PlaceFaceBones), the game skins them.
		MenuPartSystem m_menuFace;
		std::string m_menuFaceModel;   // the race head it was built for
		bool buildMenuFace(RE::NiGeometry* g, RE::NiNode* root, const std::string& model);
		void dropMenuFace(const char* why);
		float m_lastRootRadius = 0;   // the player root's last finite bound radius (see KeepRadius)
		std::string m_playerRootName;   // what the last scan took as the player's 3D (for the log)
		RE::NiNode* m_firstPersonRoot = nullptr;   // the player's first-person model this frame (its items: SkyrimSystem::setFirstPerson)
		int m_builtGround = -1;                // the ground setting the systems were built with
		bool m_rebuildPending = false;         // a collision setting changed: rebuild at the next update
	};
}
