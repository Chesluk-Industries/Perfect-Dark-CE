<p align="center">
  <img src="images/Carrington_Evolved_Logo.png" width="64%"
       alt="logo by redvox57"></p>

Mod for Perfect Dark's PC port by fgsfds. Focus on physics interactions such as ragdolls and general improvements to the port.

## Status

This fork provides updates to the main PC port for general stability and new features such as:

* Dynamic ragdoll physics with reactions (Toggle);
* Dismemberment (Toggle);
* New collision system for the player (experimental) and ragdolls;
* Guards are alerted by and investigate dead bodies (Toggle);
* Immersive first person view and death camera (Toggle);
* Gravity settings;
* Restored Dolby Surround and added 5.1 support;
* Proper audio panning support;
* Major improvements related to high framerates (up to 240FPS tested):
  * Interpolation improvements (doors, weapon animations, camera);
  * Turret accuracy and fire rate (laptop gun, helicopter, interceptor, stationary turrets, drone);
  * Visual and particle effect duration;
  * Slow motion;
  * NPC footstep spam;
  * General NPC timings;
  * Off-screen NPC timing;
  * Object physics;
  * WAR! Maian friendly fire;
  * Scripted walk sections;
  * Explosion lasting damage;
  * Menu text;
  * Simulants now act the same at any framerates;
* Fixed guards detecting bodies behind them (Air Base);
* Fixed somersault animation jitter;
* Fixed guards waking up when shot during a knockout animation;
* General fixes with simulants;
* Restored single player effects in Co-Op (stars, rain, flying cars, light flares, etc.);
* Shiny texture alpha fix (thanks murkantor);
* Viewmodels properly block lights (thanks joshuarwood);
* Decal clamping on corners;
* Nametag support during gameplay (thanks murkantor);
* Probably more stuff I've forgotten;

**The following platforms are officially supported and tested:**
* Windows 32-bit, 64-bit
* Linux 32-bit, 64-bit

macOS should work as well, but it is currently untested.

## Demo
| Pure Ragdolls | Animation Blending | Combat Simulator |
| --- | --- | --- |
| [![Pure ragdolls example](https://img.youtube.com/vi/4reRyDyE9c4/0.jpg)](https://youtu.be/4reRyDyE9c4) | [![Animation Blending example](https://img.youtube.com/vi/J5uhK9KV-CI/hqdefault.jpg)](https://youtu.be/J5uhK9KV-CI) | [![Combat Simulator showcase](https://img.youtube.com/vi/alAaEL_e-XA/hqdefault.jpg)](https://youtu.be/alAaEL_e-XA) |
| [Watch on YouTube](https://youtu.be/4reRyDyE9c4) | [Watch on YouTube](https://youtu.be/J5uhK9KV-CI) | [Watch on YouTube](https://youtu.be/alAaEL_e-XA)

## Note

This fork is made with AI coding, specifically Claude Opus 5.5, and Fable 5.1 for clean-up. All the changes were carefully tested and tweaked by real, dedicated players to ensure that everything is working as intended, without breaking any of the original code. The new code is not going to be the cleanest there is, but I hope that at least the port fixes are possible to be adapted and used for the main branch if needed.

Generative images, textures or other assets will ***never*** be present in this fork. All the text will always be written by a person. AI is only used as a tool to code things that would've taken months to years of research to pull off, possibly preventing things like this ever existing otherwise.

## Credits

* The original [PC port](https://github.com/perfect-dark-pc-port/perfect_dark) authors;
* Jorrit Rouwe for creating the [Jolt Physics library](https://github.com/jrouwe/joltphysics) used in this project;
* murkantor for the [shiny surface alpha fix](https://github.com/murkantor/perfect_dark_netplay/blob/be4671764bbd97fb698c41852edeec2b4710ead8/docs/PORT_SHINY_ALPHA.md#L4);
* Graslu00 for extensive testing, bug reporting and ideas;
* Redvox57 for feedback and logo artwork;
* froexiaSim for Combat Simulator testing and bug reporting.
