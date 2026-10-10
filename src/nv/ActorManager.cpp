#include "ActorManager.h"
#include "Game.h"
#include "Settings.h"
#include "hdtSkyrimPhysicsWorld.h"

#include <algorithm>

namespace nvsmp
{
	static const char* kPhysicsKey = "HDT Skinned Mesh Physics Object";

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
			m_items.erase(it);
			it = m_items.end();
		}
		if (it == m_items.end()) {
			Item item;
			item.skeleton = root;
			item.model = model;
			item.file = file;
			hdt::DefaultBBP::PhysicsFile_t physicsFile{ file, {} };
			hdt::SkyrimSystemCreator creator;
			logger::info("[SMP] building '{}' for {} on '{}'", file, model->GetName(), root->GetName());
			item.system = creator.createOrUpdateSystem(root, model, &physicsFile, {}, nullptr);
			if (item.system) {
				hdt::SkyrimPhysicsWorld::get()->addSkinnedMeshSystem(item.system.get());
				logger::info("[SMP] '{}': {} bones, {} collision bodies", file, item.system->getBones().size(), item.system->meshes().size());
			} else {
				item.failed = true;
				logger::error("[SMP] '{}' produced no physics for {}", file, model->GetName());
			}
			it = m_items.emplace(key, std::move(item)).first;
		}
		it->second.frame = m_frame;
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
				if (it->second.system)
					hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(it->second.system.get());
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

	void ActorManager::clear()
	{
		for (auto& [key, item] : m_items)
			if (item.system)
				hdt::SkyrimPhysicsWorld::get()->removeSkinnedMeshSystem(item.system.get());
		m_items.clear();
		stats = {};
	}
}
