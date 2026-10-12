#pragma once
// The plugin's settings (NVSMPPhysics.ini next to the DLL), editable in game from the menu.

namespace nvsmp
{
	struct Settings
	{
		bool enabled = true;
		float maxDistance = 4000.0f;   // actors farther from the player than this are not simulated
		int maxActors = 10;            // most actors simulated at once, nearest first

		// FSMP's world settings (its configs.xml defaults).
		int minFps = 60;               // the simulation's substep rate
		int maxSubSteps = 4;           // a frame longer than this many substeps drops the rest
		int solverIterations = 10;     // Bullet's constraint solver iterations
		float gravityScale = 1.0f;
		bool unclampedResets = true;   // turning faster than the limit below resets the physics
		float resetAngle = 120.0f;     // per second (FSMP's unclamped-reset-angle)

		bool wind = true;
		float windStrength = 2.0f;     // FSMP's wind strength: at the windiest weather, compared with 9.8 for gravity
		bool windInInteriors = false;

		bool collisions = true;         // off: the cloth passes through the body (for finding what pushes it)
		float waistCollision = 0.0f;    // 0: bones the physics file keeps out of collision stay out; 1: they collide fully
		bool ground = true;             // a ground plane at the actor's feet the cloth rests on (crouching, sitting)

		int menuKey = 0x76;            // F7
		int logLevel = 2;              // 0 trace .. 4 errors only
	};

	extern Settings gSettings;

	void LoadSettings();
	void SaveSettings();
	void DefaultSettings();
	void ApplySettings();   // pushes the world settings into the physics world
}
