# SnowRunner VR

![SnowRunner VR](Snowrunner_VR.jpg)

A 6-DoF VR mod for **SnowRunner** (Steam, DX11), injected as a proxy `dxgi.dll`. It is developed with Claude, and a security audit is periodically performed by the free models of Codex and Gemini Pro without findings (excluding that DLL Proxying can be used as DLL Hijacking in a malicious context, but it's standard practice for modding).
No game files are modified and nothing is patched on disk — delete one DLL and the install is stock again.

## Install

**Set the game to windowed mode.** On the first launch the mod reads your headset's native per-eye resolution, writes it into the config and says so; press OK and the game resizes to it. After that, resolution and upscaling changes apply in the running game. If the game cannot be resized, in fullscreen for one, the notice asks you to restart instead.

1. Download from releases or build `dxgi.dll` (below).
2. Drop it next to `SnowRunner.exe` — on a Steam install that is
   `steamapps\common\SnowRunner\Sources\Bin\`.
3. Start your OpenXR runtime and headset, then launch the game from Steam.

**To uninstall, delete `dxgi.dll`.** That is the whole of it.

Two files appear beside it:

- `snowrunner_vr.log` — truncated each launch.
- `Snowrunner_VR_config.txt` — The settings.

**After a game update** the mod finds the game functions it hooks again by signature, so most updates need no new mod version. The log lists them in a `game addresses` block near the top. If one is missing, the features that need it switch off rather than patch the wrong code, and the log says which. Attach the log when you report it.

## Using it

Open the menu with **Insert**, or **L3+R3** on a gamepad — both rebindable.
**HOME** recenters, as does the button at the top of the menu: face the
direction you want as "forward", then press it.

## Render options

The first setting under **Recenter view** is the **render method**. Both render one
eye per game frame; they differ in which moment each eye shows.

| | what it does |
|---|---|
| **Synched Sequential** | The game is paused between eye renders, so both eyes on the same game update. Most accurate but demanding to run. |
| **Alternate Eye Rendering (AER)** | Every frame is a new moment, for one eye; the other eye is supplied by the two options below. |

AER has two options to improve smoothness; they are hidden while Synched Sequential is selected:

| | what it does |
|---|---|
| **Stale-eye warp** | Brings that eye's own last real render forward to now. Four modes: headset rotation only; plus the game camera's rotation; plus a 6-DoF reprojection by the camera's translation; plus the game's own motion vectors (the default). Motion vectors is the most accurate and needs the game's anti-aliasing set to TAA (can also be used with DLSS or FSR) |
| **DIBR shift** | Reprojects the eye that *was* rendered into the other one using the scene depth buffer, with disocclusion holes filled up with the stale eye warp. Has artefacts and needed workarounds, default off. With the introduction of the motion vector stale eye warp I don't consider this as usefull as it once was.|

## Upscaling (optional)

The **Upscaling** tab can replace the game's own TAA with NVIDIA DLSS or AMD
FSR: the frame, depth and motion vectors the game already renders for its TAA go
through the upscaler instead. Every method needs the game's anti-aliasing set to
**TAA** in its graphics settings, and **no upscaler DLL is included with the
mod** — each method's are named below, to put next to `SnowRunner.exe`.

**Upscaling method**:

| | what it needs |
|---|---|
| **Off** (default) | Nothing: the game's own TAA, as it is. |
| **DLSS** | An NVIDIA RTX card, and `nvngx_dlss.dll` from NVIDIA's DLSS SDK on GitHub — [`lib/Windows_x86_64/rel/nvngx_dlss.dll`](https://github.com/NVIDIA/DLSS/blob/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll) ("Download raw file"), version 310.9.16 or newer. NVIDIA's licence applies to it. |
| **FSR 3** | Any DirectX 12 GPU, NVIDIA and Intel included, and `amd_fidelityfx_loader_dx12.dll` and `amd_fidelityfx_upscaler_dx12.dll` from AMD's FSR SDK on GitHub — both in [`Kits/FidelityFX/signedbin`](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v2.4.0/Kits/FidelityFX/signedbin) ("Download raw file" on each), SDK 2.4.0 or newer. AMD's licence applies to them. |
| **FSR 4** | The same two DLLs, on a Radeon RX 9000, 7000 or 6000 card or a supported Ryzen APU. On any other GPU FSR 3 runs instead. |

FSR is DirectX 12 only, so the mod runs it on a DirectX 12 device of its own
beside the game's DirectX 11 one; the two hand the images over through shared
textures.

**Quality**, for whichever method runs:

| | what it does |
|---|---|
| **DLAA / Native AA** | The upscaler at the game's own resolution, as anti-aliasing. Switches live. |
| **Quality / Balanced / Performance / Ultra Performance** | The game renders smaller — 67%, 58%, 50% or 33% of your render resolution on each side — and the upscaler brings each eye back up to the full size. VR only. Applied as soon as you pick one: the game rebuilds at the new size, which takes a moment. DIBR shift is off while it runs. |

**Preset** (DLSS only) picks the DLSS model: Default (DLSS decides), J, K, L or M.
**Sharpness** (default 30%, 0% is off) runs AMD's FidelityFX CAS for a sharper
picture, for every method — with Off, on the game's own TAA.

### VR controllers

The VR controllers work as an Xbox gamepad: triggers are LT/RT, grips are
LB/RB, A/B/X/Y are A/B/X/Y, and the sticks and their clicks are the sticks and
L3/R3. **The left menu button is Start on a tap and Back (Select) when held for
half a second.** **Hold the left stick down (L3) to use the right stick as the
D-pad** — the menu button then acts as Back straight away. A quick tap of L3
still reaches the game as L3. A real gamepad keeps working alongside; the
"VR controllers as gamepad" setting turns the mapping off.

### Mouse cursor

The Windows hardware cursor is not part of the game's D3D11 backbuffer, so the
mod composites a copy into VR. Normal menus use the UI plane. Winch attachment
points are drawn in world space and need a cursor in the eye images instead:

- press **F8** before aiming at winch attachment points;
- press **F8** again to return the cursor to normal menus;
- **Page Up/Page Down** adjust horizontal cursor alignment;
- **Shift+Page Up/Page Down** adjust vertical cursor alignment.

The world cursor compensates for headset rotation so it stays on the selected
attachment point. F8 is currently manual because no reliable game-state signal
for "selecting a winch point" has been identified. Automatic switching based on
retained UI draws produced false positives in normal menus.

### Headsets

Tested mostly with my Quest 3 over Virtual Desktop (VDXR and Steam runtime). Anything with an OpenXR runtime should work. I'm curious to see experiences with canted eye headsets, there are probably issues so let me know.

---

## Build

Requires **Visual Studio 2022 Build Tools** with the MSVC toolset, MASM, and the
"C++ CMake tools for Windows" component. CMake ships inside that component.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The DLL lands at `build\Release\dxgi.dll`. Copy it next to `SnowRunner.exe`.
Visual Studio 2026 works the same way with `-G "Visual Studio 18 2026"`.

If CMake is not on your PATH it is inside the Build Tools install, typically at
`C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
`CMakePresets.json` is included too, so `cmake --preset vs2022` followed by
`cmake --build --preset release` works.

Dependencies are fetched on the first configure and need no setup:

| | |
|---|---|
| [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4 | function detours |
| [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) 1.1.36 | linked **statically**, so only `dxgi.dll` ships |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b | the settings overlay |
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) 310.9.16 | headers and library, downloaded into the build directory and checked against pinned SHA-256s — nothing of NVIDIA's is in this repository. No network, or `-DSRVR_DLSS=OFF`: the DLL builds without DLSS. The runtime DLL is never part of the build (see Upscaling above). |
| [AMD FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.4.0 | four API headers, downloaded into the build directory and checked against pinned SHA-256s — nothing of AMD's is in this repository, and nothing is linked. No network, or `-DSRVR_FSR=OFF`: the DLL builds without FSR. The runtime DLLs are never part of the build (see Upscaling above). |

x64 only — the build fails fast on anything else. Uses the dynamic VC++ runtime
(MSVCP140), which is present on any machine that runs the game.

---

## Layout

```
src/proxy/    the proxy dxgi.dll itself: forwards every export to the real
              System32 one -- documented entry points through C++, the
              undocumented ones through asm jump stubs
src/hooks/    the engine hooks -- swapchain/Present, the camera and view
              builders, the constant buffer, draw calls, depth, input,
              resolution, and the settings menu -- plus the signature
              scanner that finds the game's functions in each build
              (game_addresses.cpp)
src/render/   D3D11 work the mod does itself: the DIBR reprojection, the
              6-DoF reprojection, the UI/smudge/winch composition layers,
              the truck and mirror masks, frame dumps, and DLSS and FSR in
              place of the game's TAA
src/xr/       OpenXR session, swapchains, eye views, layer submission, and
              the AER scheduling
src/common/   logging, the config file, and the pure decision rules that are
              unit-testable without a game or a headset
```

Source comments carry reasoning, including what was tried and did not work.
Several of them cost a session in a headset to establish and are written to stop the same ground being covered twice. Comments may reference `docs/` notes not published here. Many of those contain outdated information on their own and would take a while to clean up.

---

## Acknowledgements

SnowRunner is made by Saber Interactive and published by Focus Entertainment. This is an unofficial, unaffiliated mod. 

The reason this kind of stuff can be made with AI at all is because legends have figured out how to reverse engineer games, engines, and make them into VR experiences. Praydog, Raicuparta, Mutar, Luke Ross, Team Beef and many many more shared code and concepts these models learned from.

---

## License

The mod's own source code is under the [MIT License](LICENSE), Copyright (c) 2026 Timguin.

That covers the code in this repository, except the sharpening filter ported from AMD (see
[AMD FidelityFX CAS](#amd-fidelityfx-cas)). It does not cover everything inside a built `dxgi.dll`: a build
contains the third-party code below, and each part stays under its own license. **In particular the NVIDIA
DLSS SDK portions of a DLSS build are not MIT licensed**: see below.

### Third-party software

| Component | Used for | License |
|---|---|---|
| [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS) 310.9.16 | DLSS. Its loader library is linked into `dxgi.dll` | [NVIDIA RTX SDKs License](https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt) (proprietary) |
| [MinHook](https://github.com/TsudaKageyu/minhook) 1.3.4, with Hacker Disassembler Engine | function detours | BSD 2-Clause |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9b | the settings overlay | MIT |
| [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) 1.1.36 | the OpenXR loader, linked statically | Apache 2.0 |
| [JsonCpp](https://github.com/open-source-parsers/jsoncpp), as bundled with the OpenXR loader | JSON parsing inside the loader | Public domain / MIT |
| [AMD FidelityFX CAS](https://github.com/GPUOpen-Effects/FidelityFX-CAS) 1.20190610 | the Upscaling tab's Sharpness: its filter, ported into the mod's shader in `src/render/sharpen.cpp` | MIT |
| [AMD FSR SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.4.0 | FSR 3 and 4: its API headers (`ffx_api.h`, `ffx_api_types.h`, `dx12/ffx_api_dx12.h`, `ffx_upscale.h`), compiled into `dxgi.dll` | MIT |

### NVIDIA DLSS

This mod uses the NVIDIA DLSS SDK. NVIDIA, NVIDIA RTX and DLSS are trademarks of NVIDIA Corporation.
This mod is not sponsored or endorsed by NVIDIA.

No NVIDIA file is in this repository: the build downloads the SDK's headers and library from NVIDIA,
and players download the runtime, `nvngx_dlss.dll`, themselves. A `dxgi.dll` built with DLSS does contain
object code from the SDK's library. That code is NVIDIA's, it is distributed under the
[NVIDIA RTX SDKs License](https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt) and not under the MIT
License, and the license grant above does not extend to it. `nvngx_dlss.dll` is covered by the same license.
Building with `-DSRVR_DLSS=OFF` leaves all of it out, along with DLSS.

### AMD FSR

This mod uses the AMD FSR SDK. AMD, FSR, FidelityFX and Radeon are trademarks of Advanced Micro Devices, Inc.
This mod is not sponsored or endorsed by AMD.

No AMD file is in this repository: the build downloads four of the SDK's API headers from AMD, which are
MIT licensed (text below), and players download the runtime, `amd_fidelityfx_loader_dx12.dll` and
`amd_fidelityfx_upscaler_dx12.dll`, themselves. Those two DLLs are AMD's prebuilt, signed binaries and come
under the terms in the SDK's own [license file](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.4.0/docs/license.md).
Building with `-DSRVR_FSR=OFF` leaves the headers out, along with FSR.

### AMD FidelityFX CAS

The Upscaling tab's Sharpness filter is ported from AMD's FidelityFX Contrast Adaptive Sharpening (`ffx_cas.h`,
version 1.20190610) into the mod's own shader in `src/render/sharpen.cpp`. That filter keeps AMD's copyright
notice and MIT license, below. AMD and FidelityFX are trademarks of Advanced Micro Devices, Inc. This mod is
not sponsored or endorsed by AMD.

### License texts

<details>
<summary>MinHook and Hacker Disassembler Engine (BSD 2-Clause)</summary>

```text
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

================================================================================
Portions of this software are Copyright (c) 2008-2009, Vyacheslav Patkov.
================================================================================
Hacker Disassembler Engine 32 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

-------------------------------------------------------------------------------
Hacker Disassembler Engine 64 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

</details>

<details>
<summary>Dear ImGui (MIT)</summary>

```text
The MIT License (MIT)

Copyright (c) 2014-2026 Omar Cornut

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

</details>

<details>
<summary>JsonCpp (public domain / MIT)</summary>

```text
The JsonCpp library's source code, including accompanying documentation,
tests and demonstration applications, are licensed under the following
conditions...

Baptiste Lepilleur and The JsonCpp Authors explicitly disclaim copyright in all
jurisdictions which recognize such a disclaimer. In such jurisdictions,
this software is released into the Public Domain.

In jurisdictions which do not recognize Public Domain property (e.g. Germany as of
2010), this software is Copyright (c) 2007-2010 by Baptiste Lepilleur and
The JsonCpp Authors, and is released under the terms of the MIT License (see below).

In jurisdictions which recognize Public Domain property, the user of this
software may choose to accept it either as 1) Public Domain, 2) under the
conditions of the MIT License (see below), or 3) under the terms of dual
Public Domain/MIT License conditions described here, as they choose.

The MIT License is about as close to Public Domain as a license can get, and is
described in clear, concise terms at:

   http://en.wikipedia.org/wiki/MIT_License

The full text of the MIT License follows:

========================================================================
Copyright (c) 2007-2010 Baptiste Lepilleur and The JsonCpp Authors

Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
========================================================================
(END LICENSE TEXT)

The MIT license is compatible with both the GPL and commercial
software, affording one all of the rights of Public Domain with the
minor nuisance of being required to keep the above copyright notice
and license text in the source code. Note also that by accepting the
Public Domain "license" you can re-license your copy using whatever
license you like.
```

</details>

<details>
<summary>OpenXR-SDK (Apache 2.0)</summary>

```text
                                 Apache License
                           Version 2.0, January 2004
                        http://www.apache.org/licenses/

   TERMS AND CONDITIONS FOR USE, REPRODUCTION, AND DISTRIBUTION

   1. Definitions.

      "License" shall mean the terms and conditions for use, reproduction,
      and distribution as defined by Sections 1 through 9 of this document.

      "Licensor" shall mean the copyright owner or entity authorized by
      the copyright owner that is granting the License.

      "Legal Entity" shall mean the union of the acting entity and all
      other entities that control, are controlled by, or are under common
      control with that entity. For the purposes of this definition,
      "control" means (i) the power, direct or indirect, to cause the
      direction or management of such entity, whether by contract or
      otherwise, or (ii) ownership of fifty percent (50%) or more of the
      outstanding shares, or (iii) beneficial ownership of such entity.

      "You" (or "Your") shall mean an individual or Legal Entity
      exercising permissions granted by this License.

      "Source" form shall mean the preferred form for making modifications,
      including but not limited to software source code, documentation
      source, and configuration files.

      "Object" form shall mean any form resulting from mechanical
      transformation or translation of a Source form, including but
      not limited to compiled object code, generated documentation,
      and conversions to other media types.

      "Work" shall mean the work of authorship, whether in Source or
      Object form, made available under the License, as indicated by a
      copyright notice that is included in or attached to the work
      (an example is provided in the Appendix below).

      "Derivative Works" shall mean any work, whether in Source or Object
      form, that is based on (or derived from) the Work and for which the
      editorial revisions, annotations, elaborations, or other modifications
      represent, as a whole, an original work of authorship. For the purposes
      of this License, Derivative Works shall not include works that remain
      separable from, or merely link (or bind by name) to the interfaces of,
      the Work and Derivative Works thereof.

      "Contribution" shall mean any work of authorship, including
      the original version of the Work and any modifications or additions
      to that Work or Derivative Works thereof, that is intentionally
      submitted to Licensor for inclusion in the Work by the copyright owner
      or by an individual or Legal Entity authorized to submit on behalf of
      the copyright owner. For the purposes of this definition, "submitted"
      means any form of electronic, verbal, or written communication sent
      to the Licensor or its representatives, including but not limited to
      communication on electronic mailing lists, source code control systems,
      and issue tracking systems that are managed by, or on behalf of, the
      Licensor for the purpose of discussing and improving the Work, but
      excluding communication that is conspicuously marked or otherwise
      designated in writing by the copyright owner as "Not a Contribution."

      "Contributor" shall mean Licensor and any individual or Legal Entity
      on behalf of whom a Contribution has been received by Licensor and
      subsequently incorporated within the Work.

   2. Grant of Copyright License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      copyright license to reproduce, prepare Derivative Works of,
      publicly display, publicly perform, sublicense, and distribute the
      Work and such Derivative Works in Source or Object form.

   3. Grant of Patent License. Subject to the terms and conditions of
      this License, each Contributor hereby grants to You a perpetual,
      worldwide, non-exclusive, no-charge, royalty-free, irrevocable
      (except as stated in this section) patent license to make, have made,
      use, offer to sell, sell, import, and otherwise transfer the Work,
      where such license applies only to those patent claims licensable
      by such Contributor that are necessarily infringed by their
      Contribution(s) alone or by combination of their Contribution(s)
      with the Work to which such Contribution(s) was submitted. If You
      institute patent litigation against any entity (including a
      cross-claim or counterclaim in a lawsuit) alleging that the Work
      or a Contribution incorporated within the Work constitutes direct
      or contributory patent infringement, then any patent licenses
      granted to You under this License for that Work shall terminate
      as of the date such litigation is filed.

   4. Redistribution. You may reproduce and distribute copies of the
      Work or Derivative Works thereof in any medium, with or without
      modifications, and in Source or Object form, provided that You
      meet the following conditions:

      (a) You must give any other recipients of the Work or
          Derivative Works a copy of this License; and

      (b) You must cause any modified files to carry prominent notices
          stating that You changed the files; and

      (c) You must retain, in the Source form of any Derivative Works
          that You distribute, all copyright, patent, trademark, and
          attribution notices from the Source form of the Work,
          excluding those notices that do not pertain to any part of
          the Derivative Works; and

      (d) If the Work includes a "NOTICE" text file as part of its
          distribution, then any Derivative Works that You distribute must
          include a readable copy of the attribution notices contained
          within such NOTICE file, excluding those notices that do not
          pertain to any part of the Derivative Works, in at least one
          of the following places: within a NOTICE text file distributed
          as part of the Derivative Works; within the Source form or
          documentation, if provided along with the Derivative Works; or,
          within a display generated by the Derivative Works, if and
          wherever such third-party notices normally appear. The contents
          of the NOTICE file are for informational purposes only and
          do not modify the License. You may add Your own attribution
          notices within Derivative Works that You distribute, alongside
          or as an addendum to the NOTICE text from the Work, provided
          that such additional attribution notices cannot be construed
          as modifying the License.

      You may add Your own copyright statement to Your modifications and
      may provide additional or different license terms and conditions
      for use, reproduction, or distribution of Your modifications, or
      for any such Derivative Works as a whole, provided Your use,
      reproduction, and distribution of the Work otherwise complies with
      the conditions stated in this License.

   5. Submission of Contributions. Unless You explicitly state otherwise,
      any Contribution intentionally submitted for inclusion in the Work
      by You to the Licensor shall be under the terms and conditions of
      this License, without any additional terms or conditions.
      Notwithstanding the above, nothing herein shall supersede or modify
      the terms of any separate license agreement you may have executed
      with Licensor regarding such Contributions.

   6. Trademarks. This License does not grant permission to use the trade
      names, trademarks, service marks, or product names of the Licensor,
      except as required for reasonable and customary use in describing the
      origin of the Work and reproducing the content of the NOTICE file.

   7. Disclaimer of Warranty. Unless required by applicable law or
      agreed to in writing, Licensor provides the Work (and each
      Contributor provides its Contributions) on an "AS IS" BASIS,
      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
      implied, including, without limitation, any warranties or conditions
      of TITLE, NON-INFRINGEMENT, MERCHANTABILITY, or FITNESS FOR A
      PARTICULAR PURPOSE. You are solely responsible for determining the
      appropriateness of using or redistributing the Work and assume any
      risks associated with Your exercise of permissions under this License.

   8. Limitation of Liability. In no event and under no legal theory,
      whether in tort (including negligence), contract, or otherwise,
      unless required by applicable law (such as deliberate and grossly
      negligent acts) or agreed to in writing, shall any Contributor be
      liable to You for damages, including any direct, indirect, special,
      incidental, or consequential damages of any character arising as a
      result of this License or out of the use or inability to use the
      Work (including but not limited to damages for loss of goodwill,
      work stoppage, computer failure or malfunction, or any and all
      other commercial damages or losses), even if such Contributor
      has been advised of the possibility of such damages.

   9. Accepting Warranty or Additional Liability. While redistributing
      the Work or Derivative Works thereof, You may choose to offer,
      and charge a fee for, acceptance of support, warranty, indemnity,
      or other liability obligations and/or rights consistent with this
      License. However, in accepting such obligations, You may act only
      on Your own behalf and on Your sole responsibility, not on behalf
      of any other Contributor, and only if You agree to indemnify,
      defend, and hold each Contributor harmless for any liability
      incurred by, or claims asserted against, such Contributor by reason
      of your accepting any such warranty or additional liability.

   END OF TERMS AND CONDITIONS

   APPENDIX: How to apply the Apache License to your work.

      To apply the Apache License to your work, attach the following
      boilerplate notice, with the fields enclosed by brackets "[]"
      replaced with your own identifying information. (Don't include
      the brackets!)  The text should be enclosed in the appropriate
      comment syntax for the file format. We also recommend that a
      file or class name and description of purpose be included on the
      same "printed page" as the copyright notice for easier
      identification within third-party archives.

   Copyright [yyyy] [name of copyright owner]

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
```

</details>

<details>
<summary>AMD FidelityFX CAS (MIT)</summary>

```text
Copyright (c) 2017-2019 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

</details>

<details>
<summary>AMD FSR SDK API headers (MIT)</summary>

```text
Copyright (C) 2026 Advanced Micro Devices, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files(the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
```

</details>
