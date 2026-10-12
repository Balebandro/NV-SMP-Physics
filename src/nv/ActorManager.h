#pragma once
// Which actors and items have physics. Each frame it walks the player's and the high-process
// actors' 3D for items carrying an SMP physics file (the "HDT Skinned Mesh Physics Object" string
// extra data, on the item's root node or on its shapes, as in Skyrim), builds a system for each new
// one, and drops the systems of items that were unequipped or of actors that left range.
#include "hdtSkyrimSystem.h"

#include <map>

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
		};

		static constexpr unsigned kSettleFrames = 10, kRetryFrames = 30;
		static constexpr int kMaxBuilds = 4;

		void build(Item& item);
		static bool rebound(const Item& item);   // the game re-pointed a skin slot the system had bound
		static bool skinsBound(RE::NiAVObject* obj, int depth = 0);
		void scanActor(RE::NiNode* root);
		void scan(RE::NiNode* root, RE::NiAVObject* obj, int depth);
		void found(RE::NiNode* root, RE::NiAVObject* model, const char* file);

		std::map<std::pair<void*, void*>, Item> m_items;   // by (skeleton, model)
		unsigned m_frame = 0;
		std::string m_playerRootName;   // what the last scan took as the player's 3D (for the log)
		float m_builtWaistCollision = -1.0f;   // the waist collision the systems were built with
		int m_builtGround = -1;                // and the ground setting
		bool m_rebuildPending = false;         // a collision setting changed: rebuild at the next update
	};
}
