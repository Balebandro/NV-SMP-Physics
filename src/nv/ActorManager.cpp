#include "ActorManager.h"
#include "Game.h"
#include "Settings.h"
#include "hdtSkyrimPhysicsWorld.h"

#include <algorithm>

namespace nvsmp
{
	static const char* kPhysicsKey = "HDT Skinned Mesh Physics Object";

	// smpdress --proxy collision stand-ins (shapes named "SMP Proxy:...") are hidden (app-culled, NiAVObject flags at
	// 0x30 bit 0) when their physics is built (generateMeshBody). Culled before the game binds the item's skin, a
	// stand-in loses its collisions, and the game does not bind the skin again after a re-equip of the same model: so
	// they are shown again whenever the item's physics goes (the model may be attached again as it is). The player's
	// first-person model gets no physics: hidden every frame.
	static void SetProxiesHidden(RE::NiAVObject* obj, bool hidden, int depth = 0)
	{
		if (!obj || depth > 48)
			return;
		if (auto* node = obj->AsNode()) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				SetProxiesHidden(node->children[i], hidden, depth + 1);
			return;
		}
		if (!obj->m_name || _strnicmp(obj->m_name, "SMP Proxy:", 10) != 0)
			return;
		auto& flags = *reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(obj) + 0x30);
		flags = hidden ? (flags | 1) : (flags & ~1u);
	}

	ActorManager* ActorManager::instance()
	{
		static ActorManager manager;
		return &manager;
	}

	void ActorManager::found(RE::NiNode* root, RE::NiAVObject* model, const char* file)
	{
		auto key = std::make_pair((void*)root, (void*)model);
		auto it = m_items.find(key);
		if (it != m_items.end() && it->second.file != file) {
			// The same node now names another file: rebuild.
			if (it->second.system)
				hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
			SetProxiesHidden(it->second.model.get(), false);
			m_items.erase(it);
			it = m_items.end();
		}
		if (it == m_items.end()) {
			Item item;
			item.skeleton = root;
			item.model = model;
			item.file = file;
			item.buildAt = m_frame + kSettleFrames;
			it = m_items.emplace(key, std::move(item)).first;
		}
		it->second.frame = m_frame;
		// The game re-pointed the item's skin since it was built: it detached the model and attached it again while
		// the plugin was not looking (equipping in the Pip-Boy, which pauses the game), and the slots that drew from
		// the cloth bones now draw from the skeleton, collapsing the cloth. Built again, from scratch.
		if (it->second.system && rebound(it->second)) {
			logger::warn("[SMP] {}: the game re-pointed its skin (detached and attached again), building again", model->GetName());
			it->second.system->m_itemDetached = true;
			hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
			SetProxiesHidden(model, false);
			it->second.system = nullptr;
			it->second.builds = 0;
			it->second.failed = false;
			it->second.buildAt = m_frame + kSettleFrames;
		}
		// The game has not bound the item's skin (yet, or any more): no system until it has, then a few frames to
		// settle. Waiting does not use up the item's builds, so a long stay in the Pip-Boy doesn't end in a broken one.
		if (!it->second.failed && !skinsBound(model)) {
			if (it->second.system) {
				logger::warn("[SMP] {}: the game unbound its skin, physics paused until it is bound again", model->GetName());
				it->second.system->m_itemDetached = true;
				hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
				SetProxiesHidden(model, false);
				it->second.system = nullptr;
				it->second.builds = 0;
			}
			if (!it->second.waiting)
				logger::info("[SMP] {}: waiting for the game to bind its skin", model->GetName());
			it->second.waiting = true;
			it->second.buildAt = m_frame + kSettleFrames;
			return;
		}
		it->second.waiting = false;
		if (!it->second.system && !it->second.failed && m_frame >= it->second.buildAt)
			build(it->second);
	}

	bool ActorManager::rebound(const Item& item)
	{
		for (auto& s : item.system->m_boundSlots)
			if (!s.holds())
				return true;
		return false;
	}

	// Whether the game has bound the skin of every shape that lists its own bones: a skin with all its slots
	// pointing at nodes. After equipping in the Pip-Boy the game leaves the item's skins empty (0 bones) until
	// the Pip-Boy closes, however long that takes.
	bool ActorManager::skinsBound(RE::NiAVObject* obj, int depth)
	{
		if (!obj || depth > 64)
			return true;
		if (auto* g = hdt::castNiGeometry(obj)) {
			if (!g->FindStringExtra("SMP Bones"))
				return true;
			auto* skin = g->skinInstance;
			if (!skin || !skin->bones || !skin->boneCount)
				return false;
			for (uint32_t i = 0; i < skin->boneCount; ++i)
				if (!skin->bones[i])
					return false;
			return true;
		}
		if (auto* node = hdt::castNiNode(obj))
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				if (!skinsBound(node->children[i], depth + 1))
					return false;
		return true;
	}

	// The game attaches an item over several frames (the model, then its skin re-pointed at the
	// skeleton), so an item is built once it has been there kSettleFrames, and built again if the build
	// found the item's own bones missing, up to kMaxBuilds times; the last try is kept as it is.
	void ActorManager::build(Item& item)
	{
		auto* root = item.skeleton.get();
		auto* model = item.model.get();
		const char* file = item.file.c_str();
		++item.builds;
		hdt::DefaultBBP::PhysicsFile_t physicsFile{ item.file, {} };
		hdt::SkyrimSystemCreator creator;
		// Waist collision 0 keeps the file's weight thresholds, 1 removes them.
		creator.m_weightThresholdScale = 1.0f - gSettings.waistCollision;
		creator.m_ground = gSettings.ground;
		m_builtWaistCollision = gSettings.waistCollision;
		m_builtGround = gSettings.ground;
		logger::info("[SMP] building '{}' for {} on '{}' (try {})", file, model->GetName(), root->GetName(), item.builds);
		auto system = creator.createOrUpdateSystem(root, model, &physicsFile, {}, nullptr);
		if (!system) {
			item.failed = true;
			logger::error("[SMP] '{}' produced no physics for {}", file, model->GetName());
			return;
		}
		if (!creator.complete() && item.builds < kMaxBuilds) {
			logger::warn("[SMP] {}: incomplete, building again in {} frames", model->GetName(), kRetryFrames);
			system = nullptr;   // gives its skin slots back
			item.buildAt = m_frame + kRetryFrames;
			return;
		}
		item.system = system;
		hdt::SkyrimPhysicsWorld::get()->addSkinnedMeshSystem(item.system.get());
		logger::info("[SMP] '{}': {} bones, {} collision bodies", file, item.system->getBones().size(), item.system->meshes().size());
	}

	void ActorManager::scan(RE::NiNode* root, RE::NiAVObject* obj, int depth)
	{
		if (!obj || depth > 48)
			return;
		if (obj->extraDataCount) {
			if (const char* file = obj->FindStringExtra(kPhysicsKey)) {
				// On a shape, the item is the node holding it.
				RE::NiAVObject* model = obj->AsNode() ? obj : (obj->parent ? static_cast<RE::NiAVObject*>(obj->parent) : obj);
				if (file[0])
					found(root, model, file);
				if (obj->AsNode())
					return;   // one system per item: its shapes are part of it
			}
		}
		if (auto* node = obj->AsNode()) {
			for (uint16_t i = 0; i < node->ChildEnd(); ++i)
				scan(root, node->children[i], depth + 1);
		}
	}

	void ActorManager::scanActor(RE::NiNode* root)
	{
		if (root)
			scan(root, root, 0);
	}

	void ActorManager::update()
	{
		++m_frame;
		const auto& s = gSettings;

		// The player, then the nearest high-process actors up to the limit.
		void* player = Game::Player();
		RE::NiNode* playerRoot = player ? Game::Get3D(player) : nullptr;
		m_playerRootName = playerRoot ? playerRoot->GetName() : "(none)";
		if (RE::NiNode* firstPerson = player ? Game::PlayerFirstPerson3D(player) : nullptr; firstPerson && firstPerson != playerRoot)
			SetProxiesHidden(firstPerson, true);
		std::vector<std::pair<float, RE::NiNode*>> actors;
		if (playerRoot)
			actors.push_back({ 0.0f, playerRoot });
		RE::NiPoint3 playerPos = playerRoot ? playerRoot->world.translate : RE::NiPoint3{};
		Game::ForEachHighProcessActor([&](void* ref) {
			if (auto* root = Game::Get3D(ref)) {
				float d = (root->world.translate - playerPos).Length();
				if (d <= s.maxDistance)
					actors.push_back({ d, root });
			}
		});
		std::sort(actors.begin(), actors.end(), [](auto& a, auto& b) { return a.first < b.first; });
		if ((int)actors.size() > std::max(1, s.maxActors))
			actors.resize(std::max(1, s.maxActors));

		for (auto& a : actors)
			scanActor(a.second);

		// Items not seen this frame: unequipped, unloaded, or their actor left range.
		int failed = 0;
		std::vector<void*> roots;
		for (auto it = m_items.begin(); it != m_items.end();) {
			if (it->second.frame != m_frame) {
				if (it->second.system) {
					it->second.system->m_itemDetached = true;   // unequipped, or its actor's 3D went
					hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
				}
				SetProxiesHidden(it->second.model.get(), false);
				it = m_items.erase(it);
				continue;
			}
			failed += it->second.failed;
			if (std::find(roots.begin(), roots.end(), it->first.first) == roots.end())
				roots.push_back(it->first.first);
			++it;
		}
		stats.actors = (int)roots.size();
		stats.systems = (int)hdt::SkyrimPhysicsWorld::get()->systemCount();
		stats.failed = failed;
	}

	// A line in the log every couple of seconds saying whether the physics is doing anything visible:
	// how many of each system's bones are simulated, how far the simulated ones are from where they
	// would hang if rigid (0 = not moving), and whether the item's skin still draws with them.
	void ActorManager::report()
	{
		auto* world = hdt::SkyrimPhysicsWorld::get();
		logger::info("[stats] scan: player 3D '{}', {} physics item(s) seen ({} built)", m_playerRootName, m_items.size(), std::count_if(m_items.begin(), m_items.end(), [](auto& kv) { return (bool)kv.second.system; }));
		for (auto& [key, item] : m_items) {
			if (!item.system)
				continue;
			int simulated = 0;
			float maxOffset = 0, sumOffset = 0;
			std::vector<std::pair<float, std::string>> worst;
			for (auto& b : item.system->getBones()) {
				auto* bone = static_cast<hdt::SkyrimBone*>(b.get());
				if (bone->m_rig.isStaticOrKinematicObject() || !bone->m_anchor)
					continue;
				++simulated;
				RE::NiPoint3 rigid = (bone->m_anchor->world * bone->m_anchorToNode).translate;
				float d = (bone->m_node->world.translate - rigid).Length();
				maxOffset = std::max(maxOffset, d);
				sumOffset += d;
				worst.push_back({ d, bone->m_name.c_str() });
			}
			std::sort(worst.rbegin(), worst.rend());
			std::string most;
			for (size_t k = 0; k < worst.size() && k < 5; k++)
				most += std::format("{} {:.1f}; ", worst[k].second, worst[k].first);
			int drawn = 0;
			for (auto& s : item.system->m_boundSlots)
				drawn += s.holds();
			logger::info("[stats] {}: {} bones, {} simulated, off rigid avg {:.2f} max {:.2f} units; skin slots drawn from cloth bones {}/{}; step {:.2f} ms",
				item.model->GetName(), item.system->getBones().size(), simulated, simulated ? sumOffset / simulated : 0.0f, maxOffset,
				drawn, item.system->m_boundSlots.size(), world->m_averageStepMs);
			if (!most.empty())
				logger::info("[stats]   furthest from rest: {}", most);
		}
	}

	// The waist collision and the ground are built in (see build), so a change rebuilds every system. Not here:
	// the menu calls this from the frame's present, and dropping a system frees game nodes. The rebuild waits
	// for the next update (rebuildIfPending), where the menu's Reset physics does its work too.
	void ActorManager::applyCollision()
	{
		if (gSettings.waistCollision == m_builtWaistCollision && (int)gSettings.ground == m_builtGround)
			return;
		m_builtWaistCollision = gSettings.waistCollision;
		m_builtGround = gSettings.ground;
		m_rebuildPending = true;
	}

	void ActorManager::rebuildIfPending()
	{
		if (!m_rebuildPending)
			return;
		m_rebuildPending = false;
		if (m_items.empty())
			return;
		logger::info("[SMP] collision settings changed (waist {:.2f}, ground {}): rebuilding", gSettings.waistCollision, gSettings.ground ? "on" : "off");
		clear();
	}

	void ActorManager::clear()
	{
		for (auto& [key, item] : m_items)
			if (item.system)
				hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(item.system.get());
		m_items.clear();
		stats = {};
	}
}
