# SecondRevision - An unofficial DirectX 12 renderer for Deus Ex

Version 0.01 - experimental.

**Experimental version!**\
Please keep in mind, that it was mainly developed on an AMD RX 9070 XT.\
NVIDIA: last tested on 2026-10-10
(RTX 5060 Laptop). If it crashes or freezes, try `SafeMode=On`, and please report it with `LogLevel=Debug` logs.

## Screenshots

Left: the Vanilla preset, the game's original look. Right: the same moment with the I Never Asked For This preset.
Click a picture to see it at full size.

| Vanilla | I Never Asked For This |
|:---:|:---:|
| ![Vanilla](screenshots/04-Vanilla.jpg) | ![I Never Asked For This](screenshots/04-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/08-Vanilla.jpg) | ![I Never Asked For This](screenshots/08-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/11-Vanilla.jpg) | ![I Never Asked For This](screenshots/11-INeverAskedForThis.jpg) |

<details>
<summary>9 more pairs</summary>

| Vanilla | I Never Asked For This |
|:---:|:---:|
| ![Vanilla](screenshots/01-Vanilla.jpg) | ![I Never Asked For This](screenshots/01-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/02-Vanilla.jpg) | ![I Never Asked For This](screenshots/02-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/03-Vanilla.jpg) | ![I Never Asked For This](screenshots/03-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/05-Vanilla.jpg) | ![I Never Asked For This](screenshots/05-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/06-Vanilla.jpg) | ![I Never Asked For This](screenshots/06-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/07-Vanilla.jpg) | ![I Never Asked For This](screenshots/07-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/09-Vanilla.jpg) | ![I Never Asked For This](screenshots/09-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/10-Vanilla.jpg) | ![I Never Asked For This](screenshots/10-INeverAskedForThis.jpg) |
| ![Vanilla](screenshots/12-Vanilla.jpg) | ![I Never Asked For This](screenshots/12-INeverAskedForThis.jpg) |

</details>

## 1. INTRODUCTION

(if you're not interested, please jump to [section 4, INSTALLATION](#4-installation))

Hello Guys!\
I am truly humbled and honored that you are here, and decided to try this renderer out for yourself.\
I'd like to welcome you to the first experimental release!\
It has been a very busy one and a half months. It consumed almost all of my free time outside work, but I think
it was worth it. The road is still long. There are many known bugs to fix, and I also have some ideas for
further improvements. But the current state is ready to be shown, I think.

Deus Ex is one of my most favorite games. It has been for a long time. I was part of the team that did a
localization to the Hungarian language many many years ago.\
I was really looking forward to the upcoming Remastered version. I preordered it immediately, and I was
so very sad to see it cancelled (though I understand the very valid reasons...)\
I just wanted to replay my beloved game, even if it's only a little nicer looking.

So about 2 months ago I started thinking about it... I may not be a graphics expert, but I have dedication,
a true love for the game, and all the experience and power of the AIs of 2026, that can actually fill in
all the holes my knowledge base lacks. I am not unfamiliar with coding. Even if my day job doesn't involve
much, I have a huge experience with computers (I am a DevOps Engineer), and gaming (around 30 years).

I know very well, that "vibe coding" is not the most popular these days. But I am personally asking you
please consider the following thing:

- This is a free project. It was born from the love of the game. I don't and never will ask for any money.
  I am not even providing any means to donate anything, as I would refuse to accept it. This is free!
  Totally free!
- Even though AI helped me with coding, every artistic decision was mine. The mod/renderer looks
  and feels exactly as I intended it. I would be beyond honored if you didn't consider it "slop".
  Though you are free to think as you like, and I respect if you refuse to give it a chance.
- There is (already) a ton of work in this. I think I've put more than 200 hours into this project.
  Not AI hours. Mine :D Testing, experimenting, coding, retesting, finding ideas to implement, ways to
  make it faster. I am hoping you'll enjoy the result!

## 2. WHAT IS THIS EXACTLY?

This is a new renderer for a really old game. It tries to improve on the original game in many-many ways.\
I recommend also installing the Revision mod. That is a very good overhaul of the game. It includes
other mods and fixes that make the game better and way more stable.\
You can find the Revision mod on Steam or GOG!

Please note, that all improvements can be turned off. That is true for both the Revision mod
and this renderer! The new revised Revision maps, New Vision textures, HDTP, Shifter and all other
parts can be turned off in Revision's menu. You can still enjoy the stability and compatibility
improvements and the original look and feel.

The same is true for this project. There is actually a "Vanilla" graphics preset exactly for these
reasons.

List of improvements, (just a quick list, you can read more about them in the settings.ini file)

- DebugMenu and DebugReadout options to easily set and check the renderer.
- Graphics presets!
  - Vanilla: Original look and feel of the game. For the fans of the original look, and for slower computers.
  - Recommended: The game looks much nicer, yet it's still reasonably fast.
  - "I Never Asked For This": The best this renderer can do. Please note: You'll need a strong GPU for this.
    (and/or framegen, and/or upscaling....)

**Game fixes:**

- HDTP holograms fix
- Revision Splash Screens fix
- Load Game stability fix
- DPI-fix
- Monitorfix

(again, details in the settings.ini)

**Rendering options:**

- Antialiasing: FSR3, FSR4 (AMD only), DLSS (Nvidia only), XeSS, FXAA, SMAA, SSAA, MSAA
- Upscaling: Native, Quality, Balanced, Performance (for FSR, DLSS and XeSS only)
- HDR Mode
- Frame Generation (for FSR4, DLSS and XeSS only)
- AntiLag/Reflex (for FSR4, DLSS)
- Automatic GPU selection (for laptops mostly)
- FPS limit
- VSync (and VRR detection)

**Visual Improvements:**

- Dynamic Shadows (including optional Raytraced shadows)
- Ambient Occlusion (including RT AO)
- New dynamic 3D water with caustics. Rain interacts with water. (and optional endless water fix)
- Reflections on the water and glass-surfaces (including RT reflections)\
  (also fixes for the original mirrors)
- New mapped 3D look for textures. (Parallax Object Mapping and Displacement Mapping)\
  Please note: New Vision 1.5 only (the default in Revision). Heightmaps are AI generated.
- Soft Particles for sprites.
- Volumetric gasses and smokes.
- Depth of Field (for conversations)
- A new 3D fire effect.
- Procedurally generated grass.
- Procedurally generated trees and bushes.
- Wind effect that interacts with trees, grass, and waters. Also blows some dust...
- Improved look of fogs.
- Improved look of forcefields.
- A new improved look of the flashlight. Real 3D beams with shadows. Optionally also for enemies in combat mode.

**Useless stupid stuff:**

- God Rays (not tested much yet)
- Motion Blur
- Chromatic Aberration
- Vignette mode
- Film Grain (to be improved though)
- Lens Flare (this one actually looks cool, even if it's stupid :D )
- Bloom (this one isn't even that stupid...)

## 3. REQUIREMENTS

- Windows 11\
  I believe there's a pretty good chance it runs on older OSs too, but I have Windows 11, and only tested on that.
- A DirectX 12 compatible graphics chip. This is a DirectX 12 renderer after all. :D
- HDR display for the HDR feature.
- Raytracing capable hardware for the RT features.
- The 64 bit companion app for the RT options, FSR4, DLSS, XeSS and Frame-Generation.\
  Again, not tested with older OSs, or 32bit OSs, but for these features you'll definitely need a 64bit OS.

* 8 GB RAM (it's a guesstimate, but it should be fine)
* 8 GB VRAM (again guesstimate, and for 'all the features turned on', so 6 GB also may be enough)
* Try to install the game to an SSD if you can. May eliminate weird issues.

Please note! Although this game is old, the renderer adds features
that can be demanding for your hardware, and they are not perfectly
optimized either. Some areas are even more demanding: the Ocean Lab,
for example, is full of glass windows, and RT reflections on those
decimate the FPS :D What you can try instead of turning those off:

- select a lower Upscale setting, Performance for example,
- try FrameGeneration together with AntiLag (Reflex on NVIDIA).

## 4. INSTALLATION

Extract the release zip into the game's folder (`Deus Ex\Revision` for Revision):\
This will add files to the game's `System` and `NewVision` folders.\
If you don't have New Vision, you don't need the `NewVision` folder, but it won't hurt if you copy it.

- `SecondRevision.dll`, `SecondRevision.int` = the renderer
- `SecondRevision-leafcards.bin`, `SecondRevision-bark.bin` = the grown trees' leaves and bark
- `SecondRevisionHost64` folder = the 64-bit companion app, with AMD's, NVIDIA's and Intel's upscaler DLLs
- `SecondRevision-THIRD-PARTY-NOTICES.md` = the licences of the third-party parts

Then set the renderer in the game's ini (`Revision.ini` for Revision), section `[Engine.Engine]`:

```ini
GameRenderDevice=SecondRevision.RevisionRenderDevice
```

(or select it from the menu)

On its first start the renderer writes `SecondRevision.settings.ini` beside itself
(if missing), with every setting explained.\
Ideally, the only setting you may want to edit is `Preset`.\
The default hotkey for the renderer's menu is ScrollLock. If you don't have that on
your keyboard, set it to something different!

> **IMPORTANT!**
>
> Windows may block the companion from starting.\
> As of now, the only solution I can give you is to disable "Smart App Control".\
> Just search for "disable smart app control", and you'll find ways to do it.
>
> In case Windows blocks the companion app, the renderer will fall back to FSR3.1,
> and you'll lose much of the advanced features!
>
> I'll try to solve this issue by digitally signing it in later releases. Much later releases...

## 5. LEGAL STUFF

I based my work on the work of Magnus Norddahl (dpjudas). I adapted his dx11 renderer.\
(as his DirectX12 renderer did not have a licence file, and "assuming" that it's also
under zlib was not an option)\
This renderer too is under zlib licence. Refer to the [LICENCE](LICENSE) file.\
For the code: I tried to add all required dependencies, and I actually added the ones
I was allowed to in their licences. If any component or part has a different licence,
it's noted in [THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES.md).

## 6. CONTACT ME & SPECIAL THANKS

In case of issues you can use [GitHub Issues](https://github.com/takysoft/SecondRevision/issues).\
Or you can find my email address in the settings.ini file!\
Also ways to generate useful logs if you'd like to help with the development with some testing!\
More on this in the settings.ini

**Special Thanks:**

- Kinga Sorban for providing a computer with an Nvidia chip for testing and implementation.
- Her beautiful son Vencel for not crying during testing <3

* Magnus Norddahl (dpjudas) for his fantastic work with the base renderer.
* Marijn Kentie, for his fantastic work on previous renderer and his awesome launcher.
* Chris Dohnal for the OpenGL renderer. I never used it, but it was still an inspiration.
* Literally anybody who ever made any kind of mod to this game!

- Daniel Greenheck, for the EZ-Tree tree generating method.
- Princeton University, Infinigen, for the tree bark generation process
- ambientCG, for the tree bark photo

* The New Vision team:
  - Dave Watts - Lead artist
  - David Guttridge - Additional Textures
  - Thijs Hagens - Additional Textures
  - Helder Pinto - Additional Textures
  - Hanfling - Game Launcher
  - Bjorn Ehrby - Installer
  - Georg Fritzsche - Installer

- The Revision team (Caustic Creative):
  - Haris Buzis - Design
  - Bjorn Ehrby - Production
  - John French - Music & Creative
  - Logen Felber - Music
  - Cecilia Raffaeli - Graphics & Web
  - Sebastian Kaufel - Programming
  - Hawk - Gameplay

* HDTP team
  - Metche_steele
  - DDL
  - Lofaesofa
  - Phasmatis
  - Laputan Machine
  - SnipaMasta
  - Fred
  - JayJay
  - Beenjamin
  - Smooth
  - Mr_Cyberpunk
  - ThalesA7X
  - JC12
  - Nate/Darkshade
  - Master_Kale
  - Trestkon
  - Ajare
  - EL
  - Tek
  - Pedro
  - JF
  - Vodunloas
  - Pointman
  - GIMG

- Everybody in Shifter, BioMod and Human Revolution.
- Everybody else I forgot to mention. If you feel you are left out, contact me, and I'll add you in the next release!

And clearly:

- Warren Spector
- Ion Storm
- Eidos
- Square Enix (for being super mod friendly)
- Embracer Group (for hopefully remaining so)
- Epic Games (for the Unreal 1 engine)
- Nvidia\*
- AMD\*
- Intel\*

\*for the fantastic SDKs and tools provided, but I have to legally add that
this project was not endorsed or sponsored by these companies (or anybody else in this Special Thanks list.)

- Anthropic\
  for Claude AI, my companion in this development.

And last but not least

- You!\
  Yes, you! For reading this, taking an interest, and maybe even downloading it,
  trying it out, and hopefully enjoying it!\
  You are awesome!
