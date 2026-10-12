<p align="center">
  <img src="images/Carrington_Evolved_Logo.png" width="64%"
       alt="logo by redvox57"></p>

Work-In-Progress mod for Perfect Dark's PC port by fgsfds. Focus on physics interactions such as ragdolls and general improvements to the port.
This will be updated on release.

## Status

This fork provides updates to the main PC port for general stability and new features such as:

* Dynamic ragdoll physics with reactions (Toggle);
* Dismemberment (Toggle);
* New collision system for the player (experimental) and ragdolls;
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
* Fixed guards detecting bodies behind them (Air Base);
* Fixed somersault animation jitter;
* Fixed guards waking up when shot during a knockout animation;
* Restored single player effects in Co-Op (stars, rain, flying cars, light flares, etc.);
* Shiny texture alpha fix (thanks murkantor);
* Viewmodels properly block lights (thanks joshuarwood);
* Decal clamping on corners;

**The following platforms are officially supported and tested:**
* Windows
* Linux

** More tests pending

## Demo
| Pure Ragdolls | Animation Blending |
| --- | --- |
| [![Pure ragdolls example](https://img.youtube.com/vi/4reRyDyE9c4/0.jpg)](https://youtu.be/4reRyDyE9c4) | [![Animation Blending example](https://img.youtube.com/vi/J5uhK9KV-CI/hqdefault.jpg)](https://youtu.be/J5uhK9KV-CI) |
| [Watch on YouTube](https://youtu.be/4reRyDyE9c4) | [Watch on YouTube](https://youtu.be/J5uhK9KV-CI) |

## Credits

* The original [PC port](https://github.com/perfect-dark-pc-port/perfect_dark) authors;
* Jorrit Rouwe for creating the [Jolt Physics library](https://github.com/jrouwe/joltphysics) used in this project;
* murkantor for the [shiny surface alpha fix](https://github.com/murkantor/perfect_dark_netplay/blob/be4671764bbd97fb698c41852edeec2b4710ead8/docs/PORT_SHINY_ALPHA.md#L4)
* Graslu00 for extensive testing, bug reporting and ideas;
* Redvox57 for feedback and logo artwork.
