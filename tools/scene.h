#pragma once
// An item NIF laid out in memory as Fallout New Vegas has it (the plugin's NetImmerseUtils.h types),
// attached to a skeleton the way the game attaches armor: skin slots pointed at the skeleton's nodes by
// name, unknown ones at the skeleton root. Shared by the offline test and the previewer, which then run
// the plugin's real system builder and physics world on it.
#include "hdtSkyrimPhysicsWorld.h"
#include "hdtSkyrimSystem.h"
#include "nif.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace scene
{
	struct Skeleton
	{
		RE::NiNode* actor = nullptr;   // the actor's 3D root (what the plugin calls the skeleton)
		RE::NiNode* bip01 = nullptr;   // the skeleton root the game points unknown skin slots at
		std::map<std::string, RE::NiNode*> bones;
		std::map<std::string, RE::NiTransform> bind;   // world bind pose
	};

	struct ShapeInfo
	{
		RE::NiGeometry* geometry = nullptr;
		std::vector<nif::Vec3> normals;        // bind-pose normals (shape space), may be empty
		std::vector<std::pair<float, float>> uvs;   // may be empty
		std::string diffuse;                   // the texture set's first texture, as the NIF names it
		bool alpha = false;                    // has an NiAlphaProperty
	};

	struct Item
	{
		Skeleton skeleton;
		RE::NiNode* model = nullptr;           // the item's root under the actor ("Scene Root")
		std::vector<ShapeInfo> shapes;         // in file order
		std::map<std::string, RE::NiGeometry*> byName;
		std::string physicsFile;               // what the root's "HDT Skinned Mesh Physics Object" names
		bool weapon = false;                   // on the skeleton's Weapon node (a weapon NIF), not the actor
		// A placed object's physics (a static: its root names the physics file, its skins name no skeleton bone): on its own
		// root, not under the actor, its skins on its own nodes, as the game places it in a cell.
		bool worldObject = false;
	};

	// Loads the NIF into a new Item (never freed: the objects are leaked on reload, a few MB at most).
	// Throws std::runtime_error on a file the reader cannot parse.
	// With skeletonPath (the game's skeleton.nif) the item goes on that skeleton, every node at its rest
	// transform; without, on one made from the bones the item's skins name, at their bind poses.
	Item Load(const std::string& nifPath, const std::string& skeletonPath = {});

	// What hdt::readAllFile returns for a path the physics names (the test and the previewer each set it).
	extern std::function<std::string(const char* path)> gReadFile;

	// A vertex in the world as the game skins it: sum of weight * bone world * skin-to-bone * vertex.
	using Weights = std::vector<std::vector<std::pair<int, float>>>;
	RE::NiPoint3 SkinVertex(RE::NiGeometry* g, int v, const Weights& weights);
	RE::NiPoint3 SkinNormal(RE::NiGeometry* g, const nif::Vec3& n, int v, const Weights& weights);
	Weights WeightsOf(RE::NiGeometry* g);

	RE::NiMatrix3 RotX(float a);
	RE::NiMatrix3 RotY(float a);
	RE::NiMatrix3 RotZ(float a);
}
