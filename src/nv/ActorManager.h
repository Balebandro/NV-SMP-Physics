#pragma once
// Which actors and items have physics. Each frame it walks the player's and the high-process
// actors' 3D for items carrying an SMP physics file (the "HDT Skinned Mesh Physics Object" string
// extra data, on the item's root node or on its shapes, as in Skyrim), builds a system for each new
// one, and drops the systems of items that were unequipped or of actors that left range.
#include "hdtSkyrimSystem.h"

#include <map>
#include <memory>

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
			const char* headPartModel = nullptr);   // a head part's: keyed by its shape (hairShape), not the FaceGen node

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
		float m_lastRootRadius = 0;   // the player root's last finite bound radius (see KeepRadius)
		std::string m_playerRootName;   // what the last scan took as the player's 3D (for the log)
		RE::NiNode* m_firstPersonRoot = nullptr;   // the player's first-person model this frame (its items: SkyrimSystem::setFirstPerson)
		float m_builtWaistCollision = -1.0f;   // the waist collision the systems were built with
		int m_builtGround = -1;                // and the ground setting
		bool m_rebuildPending = false;         // a collision setting changed: rebuild at the next update
	};
}
