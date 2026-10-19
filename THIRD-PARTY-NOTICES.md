# Third-party notices

This renderer is a modified, heavily rewritten descendant of an existing open-source renderer, and it links code from
AMD's FidelityFX SDK. The notices below are part of the distribution: they must ship with the binary and stay in the
source tree.

---

## Original renderer: D3D11Drv by Magnus Norddahl (dpjudas)

This project started from the Deus Ex / Unreal Engine 1 Direct3D 11 renderer in the `UT99VulkanDrv` repository by
Magnus Norddahl (dpjudas), which is released under the zlib licence. Since then the codebase has been rewritten and
restructured for Direct3D 12 and extended far beyond the original (own backend, shadow maps, ambient occlusion, FSR,
HDR output, settings and menu system). **This is an altered version. It is not the original software, and it must not
be represented as dpjudas' work or confused with any official release of it.** The original groundwork is gratefully
acknowledged.

The original notice, preserved as required:

	// Copyright (c) 2020-2023 Magnus Norddahl
	//
	// This software is provided 'as-is', without any express or implied
	// warranty.  In no event will the authors be held liable for any damages
	// arising from the use of this software.
	//
	// Permission is granted to anyone to use this software for any purpose,
	// including commercial applications, and to alter it and redistribute it
	// freely, subject to the following restrictions:
	//
	// 1. The origin of this software must not be misrepresented; you must not
	//    claim that you wrote the original software. If you use this software
	//    in a product, an acknowledgment in the product documentation would be
	//    appreciated but is not required.
	// 2. Altered source versions must be plainly marked as such, and must not be
	//    misrepresented as being the original software.
	// 3. This notice may not be removed or altered from any source distribution.

Note on scope: in that repository the zlib licence covers the `VulkanDrv`, `D3D11Drv` and `HRTFAudio` folders. The
`D3D12Drv` folder is **not** listed by its licence file, so nothing from it is used here (see
`docs/szabaly-licenc-tisztasag.md`).

---

## AMD FidelityFX SDK (FSR 3.1 upscaler)

	This file is part of the FidelityFX SDK.

	Copyright (C) 2024 Advanced Micro Devices, Inc.

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

---

## AMD FSR SDK 2.3 (the signed FSR DLLs SecondRevisionHost64 loads)

`amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll` and `amd_fidelityfx_framegeneration_dx12.dll` in the
`SecondRevisionHost64` folder are
AMD's own signed binaries from the AMD FSR SDK v2.3.0 (`Kits/FidelityFX/signedbin`), unchanged. The SDK's licence
file (`Kits/FidelityFX/docs/license.md`) names them among the files under this licence:

	Copyright (C) Advanced Micro Devices, Inc.

	Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the “Software”), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

---

## AMD Radeon Anti-Lag 2 SDK (AntiLag; src/RevisionDX12/AntiLag2.h)

`AntiLag2.h` is AMD's `ffx_antilag2_dx12.h` from the Anti-Lag 2 SDK, made to work in a 32-bit process as well (the
driver's 32-bit extension, the 32-bit size of one state block).

	Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.

	Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
	documentation files (the "Software"), to deal in the Software without restriction, including without limitation
	the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
	permit persons to whom the Software is furnished to do so, subject to the following conditions:
	The above copyright notice and this permission notice shall be included in all copies or substantial portions of
	the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
	THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
	TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.

## NVIDIA DLSS SDK 310.9.1 (AntiAliasing=DLSS and its frame generation, in SecondRevisionHost64)

`nvngx_dlss.dll` and `nvngx_dlssg.dll` in the `SecondRevisionHost64` folder are NVIDIA's DLSS runtimes - the upscaler's
and frame generation's - from the NVIDIA DLSS SDK v310.9.1 (`lib/Windows_x86_64/rel`), unchanged;
`SecondRevisionHost64.exe` is linked with the SDK's NGX library (`nvsdk_ngx_s.lib`). All three are used under the NVIDIA
RTX SDKs License (v. March 14, 2024), which the SDK ships as `LICENSE.txt`; they are not covered by this project's own
licence. DLSS runs only on NVIDIA RTX cards, and its frame generation only on those that have it; everywhere else the
host refuses it and the renderer's built-in upscaler runs.

	NVIDIA DLSS SDK. Copyright (c) NVIDIA Corporation. All rights reserved.
	NVIDIA, NVIDIA RTX and DLSS are trademarks and/or registered trademarks of NVIDIA Corporation.

The DLSS runtime carries third-party code of its own. NVIDIA's DLSS Programming Guide (4.2.3) asks for its
notices in the documentation of the product that ships the DLL; they follow as the guide of SDK v310.9.1 gives
them ("3rd Party Software").

### CURL

	COPYRIGHT AND PERMISSION NOTICE
	Copyright © 1996 - 2018, Daniel Stenberg, daniel@haxx.se, and many contributors, see the THANKS file.
	All rights reserved.
	Permission to use, copy, modify, and distribute this software for any purpose with or without fee is
	hereby granted, provided that the above copyright notice and this permission notice appear in all
	copies.
	THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS. IN NO EVENT SHALL THE AUTHORS OR
	COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
	ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
	SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
	Except as contained in this notice, the name of a copyright holder shall not be used in advertising or
	otherwise to promote the sale, use or other dealings in this Software without prior written authorization
	of the copyright holder.

### 8x13 BITMAP FONT

	https://courses.cs.washington.edu/courses/cse457/98a/tech/OpenGL/font.c
	* (c) Copyright 1993, Silicon Graphics, Inc.
	* ALL RIGHTS RESERVED
	* Permission to use, copy, modify, and distribute this software for
	* any purpose and without fee is hereby granted, provided that the above
	* copyright notice appear in all copies and that both the copyright notice
	* and this permission notice appear in supporting documentation, and that
	* the name of Silicon Graphics, Inc. not be used in advertising
	* or publicity pertaining to distribution of the software without specific,
	* written prior permission.
	*
	* THE MATERIAL EMBODIED ON THIS SOFTWARE IS PROVIDED TO YOU "AS-IS"
	* AND WITHOUT WARRANTY OF ANY KIND, EXPRESS, IMPLIED OR OTHERWISE,
	* INCLUDING WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY OR
	* FITNESS FOR A PARTICULAR PURPOSE.  IN NO EVENT SHALL SILICON
	* GRAPHICS, INC.  BE LIABLE TO YOU OR ANYONE ELSE FOR ANY DIRECT,
	* SPECIAL, INCIDENTAL, INDIRECT OR CONSEQUENTIAL DAMAGES OF ANY
	* KIND, OR ANY DAMAGES WHATSOEVER, INCLUDING WITHOUT LIMITATION,
	* LOSS OF PROFIT, LOSS OF USE, SAVINGS OR REVENUE, OR THE CLAIMS OF
	* THIRD PARTIES, WHETHER OR NOT SILICON GRAPHICS, INC.  HAS BEEN
	* ADVISED OF THE POSSIBILITY OF SUCH LOSS, HOWEVER CAUSED AND ON
	* ANY THEORY OF LIABILITY, ARISING OUT OF OR IN CONNECTION WITH THE
	* POSSESSION, USE OR PERFORMANCE OF THIS SOFTWARE.

### d3dx12.h

	https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12Multithreading/src/d3dx12.h
	*     Copyright © 2017 NVIDIA Corporation.  All rights reserved.
	*
	*  NOTICE TO USER:
	*
	*  This software is subject to NVIDIA ownership rights under U.S. and international Copyright laws.
	*
	*  This software and the information contained herein are PROPRIETARY and CONFIDENTIAL to NVIDIA
	*  and are being provided solely under the terms and conditions of an NVIDIA software license agreement
	*  and / or non-disclosure agreement.  Otherwise, you have no rights to use or access this software in any manner.
	*
	*  If not covered by the applicable NVIDIA software license agreement:
	*  NVIDIA MAKES NO REPRESENTATION ABOUT THE SUITABILITY OF THIS SOFTWARE FOR ANY PURPOSE.
	*  IT IS PROVIDED "AS IS" WITHOUT EXPRESS OR IMPLIED WARRANTY OF ANY KIND.
	*  NVIDIA DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
	*  INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE.
	*  IN NO EVENT SHALL NVIDIA BE LIABLE FOR ANY SPECIAL, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES,
	*  OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,  WHETHER IN AN ACTION OF CONTRACT,
	*  NEGLIGENCE OR OTHER TORTIOUS ACTION,  ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOURCE
	CODE.
	*
	*  U.S. Government End Users.
	*  This software is a "commercial item" as that term is defined at 48 C.F.R. 2.101 (OCT 1995),
	*  consisting  of "commercial computer  software"  and "commercial computer software documentation"
	*  as such terms are  used in 48 C.F.R. 12.212 (SEPT 1995) and is provided to the U.S. Government only as a commercial
	end item.
	*  Consistent with 48 C.F.R.12.212 and 48 C.F.R. 227.7202-1 through 227.7202-4 (JUNE 1995),
	*  all U.S. Government End Users acquire the software with only those rights set forth herein.
	*
	*  Any use of this software in individual and commercial software must include,
	*  in the user documentation and internal comments to the code,
	*  the above Disclaimer (as applicable) and U.S. Government End Users Notice.
	*

	* Original code by Microsoft Corporation was obtained from
	*  https://github.com/Microsoft/DirectX-Graphics-Samples/tree/master/Samples/Desktop/D3D12Multithreading
	*  and licensed under MIT license (below)
	*  NVIDIA license applies to changes made relative to original code.

	* Copyright (c) Microsoft. All rights reserved.
	* This code is licensed under the MIT License (MIT).
	* THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
	* ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
	* IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
	* PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.

### xml

	https://pugixml.org/license.html
	* This library is available to anybody free of charge, under the terms of MIT License:
	* Copyright (c) 2006-2018 Arseny Kapoulkine
	* Permission is hereby granted, free of charge, to any person obtaining a copy of this
	* software and associated documentation files (the “Software”), to deal in the Software
	* without restriction, including without limitation the rights to use, copy, modify, merge,
	* publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
	* to whom the Software is furnished to do so, subject to the following conditions:
	* The above copyright notice and this permission notice shall be included in all copies or
	* substantial portions of the Software.
	* THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
	* INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
	* PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
	* FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
	* OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	* DEALINGS IN THE SOFTWARE.
	* This means that you can freely use pugixml in your applications, both open-source and
	* proprietary. If you use pugixml in a product, it is sufficient to add an acknowledgment
	* like this to the product distribution:
	* This software is based on pugixml library (http://pugixml.org). pugixml is Copyright (C) *
	2006-2018 Arseny Kapoulkine.

### npy

	https://github.com/llohse/libnpy
	https://github.com/llohse/libnpy/blob/master/LICENSE
	* MIT License

	* Copyright (c) 2021 Leon Merten Lohse

	* Permission is hereby granted, free of charge, to any person obtaining a copy
	* of this software and associated documentation files (the "Software"), to deal
	* in the Software without restriction, including without limitation the rights
	* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	* copies of the Software, and to permit persons to whom the Software is
	* furnished to do so, subject to the following conditions:

	* The above copyright notice and this permission notice shall be included in all
	* copies or substantial portions of the Software.

	* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	* SOFTWARE.

### stb

	https://github.com/nothings/stb/blob/master/LICENSE
	/*
	* ALTERNATIVE A - MIT License
	* Copyright (c) 2017 Sean Barrett
	* Permission is hereby granted, free of charge, to any person obtaining a copy of
	* this software and associated documentation files (the "Software"), to deal in
	* the Software without restriction, including without limitation the rights to
	* use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
	* of the Software, and to permit persons to whom the Software is furnished to do
	* so, subject to the following conditions:
	* The above copyright notice and this permission notice shall be included in all
	* copies or substantial portions of the Software.
	* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	* SOFTWARE.
	*/

### DirectX-Graphics-Samples

	https://github.com/Microsoft/DirectX-Graphics-
	Samples/tree/master/Samples/Desktop/D3D12Multithreading

	/*
	* Copyright (c) Microsoft. All rights reserved.
	* This code is licensed under the MIT License (MIT).
	* THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
	* ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
	* IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
	* PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
	*/

### Vulkan-Headers

	https://github.com/KhronosGroup/Vulkan-Headers/
	  Copyright 2015-2023 The Khronos Group Inc.

	   Licensed under the Apache License, Version 2.0 (the "License");
	   you may not use this file except in compliance with the License.
	   You may obtain a copy of the License at

	       http://www.apache.org/licenses/LICENSE-2.0

	   Unless required by applicable law or agreed to in writing, software
	   distributed under the License is distributed on an "AS IS" BASIS,
	   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	   See the License for the specific language governing permissions and
	   limitations under the License.

---

## Intel XeSS SDK 3.0.2 (AntiAliasing=XeSS, in SecondRevisionHost64)

libxess.dll, Intel's XeSS upscaler, beside SecondRevisionHost64.exe, which loads it at run time; it is shipped as Intel
ships it. The header files it is used through are the SDK's. Beside it are the SDK's own licence (libxess-LICENSE.txt)
and its list of the third-party components inside the library and their terms (libxess-third-party-programs.txt).

	Intel Simplified Software License (Version October 2022)

	Intel(R) Xe Super Sampling (XeSS) SDK: Copyright (C) 2025 Intel Corporation

	Use and Redistribution. You may use and redistribute the software, which is provided in binary form only, (the "Software"), without modification, provided the following conditions are met:

	* 	Redistributions must reproduce the above copyright notice and these terms of use in the Software and in the documentation and/or other materials provided with the distribution.
	* 	Neither the name of Intel nor the names of its suppliers may be used to endorse or promote products derived from this Software without specific prior written permission.
	* 	No reverse engineering, decompilation, or disassembly of the Software is permitted, nor any modification or alteration of the Software or its operation at any time, including during execution.

	No other licenses. Except as provided in the preceding section, Intel grants no licenses or other rights by implication, estoppel or otherwise to, patent, copyright, trademark, trade name, service mark or other intellectual property licenses or rights of Intel.

	Third party software.  "Third Party Software" means the files (if any) listed in the "third-party-software.txt" or other similarly-named text file that may be included with the Software. Third Party Software, even if included with the distribution of the Software, may be governed by separate license terms, including without limitation, third party license terms, open source software notices and terms, and/or other Intel software license terms. These separate license terms solely govern Your use of the Third Party Software.

	DISCLAIMER. THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT ARE DISCLAIMED. THIS SOFTWARE IS NOT INTENDED FOR USE IN SYSTEMS OR APPLICATIONS WHERE FAILURE OF THE SOFTWARE MAY CAUSE PERSONAL INJURY OR DEATH AND YOU AGREE THAT YOU ARE FULLY RESPONSIBLE FOR ANY CLAIMS, COSTS, DAMAGES, EXPENSES, AND ATTORNEYS' FEES ARISING OUT OF ANY SUCH USE, EVEN IF ANY CLAIM ALLEGES THAT INTEL WAS NEGLIGENT REGARDING THE DESIGN OR MANUFACTURE OF THE SOFTWARE.

	LIMITATION OF LIABILITY. IN NO EVENT WILL INTEL BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

	No support.  Intel may make changes to the Software, at any time without notice, and is not obligated to support, update or provide training for the Software.

	Termination. Your right to use the Software is terminated in the event of your breach of this license.

	Feedback.  Should you provide Intel with comments, modifications, corrections, enhancements or other input (“Feedback”) related to the Software, Intel will be free to use, disclose, reproduce, license or otherwise distribute or exploit the Feedback in its sole discretion without any obligations or restrictions of any kind, including without limitation, intellectual property rights or licensing obligations.

	Compliance with laws.  You agree to comply with all relevant laws and regulations governing your use, transfer, import or export (or prohibition thereof) of the Software.

	Governing law.  All disputes will be governed by the laws of the United States of America and the State of Delaware without reference to conflict of law principles and subject to the exclusive jurisdiction of the state or federal courts sitting in the State of Delaware, and each party agrees that it submits to the personal jurisdiction and venue of those courts and waives any objections. THE UNITED NATIONS CONVENTION ON CONTRACTS FOR THE INTERNATIONAL SALE OF GOODS (1980) IS SPECIFICALLY EXCLUDED AND WILL NOT APPLY TO THE SOFTWARE.

---

## AMD GPU Services (AGS)

Used to ask an AMD driver whether a screen runs with variable refresh (FreeSync). Linked into the renderer.

	Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE.

---

## NVIDIA NVAPI

Used to ask an NVIDIA driver whether a screen runs with variable refresh (adaptive sync / G-Sync), and for NVIDIA
Reflex (AntiLag on NVIDIA's cards; src/RevisionDX12/Reflex.h). Linked into the renderer.

	nvapi.lib, nvapi64.lib, nvapia64.lib and nvapia64ec.lib are licensed under the following terms:

	SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
	SPDX-License-Identifier: MIT

	Permission is hereby granted, free of charge, to any person obtaining a 
	copy of this software and associated documentation files (the "Software"),
	to deal in the Software without restriction, including without limitation
	the rights to use, copy, modify, merge, publish, distribute, sublicense,
	and/or sell copies of the Software, and to permit persons to whom the
	Software is furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
	THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
	FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
	DEALINGS IN THE SOFTWARE.

---

## Intel Graphics Control Library (header only)

The renderer includes Intel's header to ask an Intel driver whether a screen runs with adaptive sync; the library
itself is the driver's own ControlLib32.dll, loaded at run time and not shipped with this renderer.

	Intel Software License Agreement
	10.07.21 

	Use and Redistribution. You may use and redistribute Intel’s Control API software and the header files for the Intel® Graphics Control Library (collectively, the “Software”), solely for use on Intel platforms, provided the following conditions are met:
	* Redistributions must reproduce the above copyright notice and the following terms of use in the Software and in the documentation and/or other materials provided with the distribution.
	* Neither the name of Intel nor the names of its suppliers may be used to endorse or promote products derived from this Software without specific prior written permission.
	* No reverse engineering, decompilation, or disassembly of Software provided to you in binary form is permitted.

	Third Party Software. The Software may contain Third Party Software. “Third Party Software” is third party software, open source software or other Intel software listed in the “third-party-software.txt” or other similarly named text file that is included with the Software. Third Party Software, even if included with the distribution of the Software, may be governed by separate license terms, including without limitation, third party license terms, open source software notices and terms, and/or other Intel software license terms. These separate license terms solely govern your use of the Third Party Software.

	Ownership. Title to the Software and all copies remain with Intel or its suppliers. 

	DISCLAIMER. THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, AND NON-INFRINGEMENT ARE DISCLAIMED. THIS SOFTWARE IS NOT INTENDED FOR USE IN SYSTEMS OR APPLICATIONS WHERE FAILURE OF THE SOFTWARE MAY CAUSE PERSONAL INJURY OR DEATH AND YOU AGREE THAT YOU ARE FULLY RESPONSIBLE FOR ANY CLAIMS, COSTS, DAMAGES, EXPENSES, AND ATTORNEYS’ FEES ARISING OUT OF ANY SUCH USE, EVEN IF ANY CLAIM ALLEGES THAT INTEL WAS NEGLIGENT REGARDING THE DESIGN OR MANUFACTURE OF THE SOFTWARE.

	LIMITATION OF LIABILITY; INDEMNITY. IN NO EVENT WILL INTEL BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. YOU AGREE TO INDEMNIFY, DEFEND, AND HOLD INTEL HARMLESS AGAINST ANY CLAIMS, DAMAGES, AND EXPENSES RESULTING FROM YOUR USE OR UNAUTHORIZED USE OF THE SOFTWARE.

	No support. Intel may make changes to the Software, at any time without notice, and is not obligated to support, update or provide training for the Software.

	Termination. Your right to use the Software is terminated immediately in the event of your breach of this agreement.

	Feedback. Should you provide Intel with comments, modifications, corrections, enhancements or other input (“Feedback”) related to the Software Intel will be free to use, disclose, reproduce, license or otherwise distribute or exploit the Feedback in its sole discretion without any obligations or restrictions of any kind, including without limitation, intellectual property rights or licensing obligations.

	Compliance with laws. You agree to comply with all applicable laws and regulations governing your use, transfer, import or export (or prohibition thereof) of the Software.

	Governing law. All disputes will be governed by the laws of the United States of America and the State of Delaware without reference to conflict of law principles and subject to the exclusive jurisdiction of the state or federal courts sitting in the State of Delaware, and each party agrees that it submits to the personal jurisdiction and venue of those courts and waives any objections. The United Nations Convention on Contracts for the International Sale of Goods (1980) is specifically excluded and will not apply to the Software.

	Severability. The parties intend that if a court holds that any provision of this agreement is invalid or unenforceable under applicable law, the court will modify the provision to the minimum extent necessary to make it valid and enforceable or sever and delete the provision from this agreement. Any change to or deletion of a provision of this agreement under this Section will not affect the validity or enforceability of the remainder of this agreement, which will continue in full force and effect.

---

## Deus Ex SDK (engine headers and import libraries)

The renderer is built against the Deus Ex SDK headers and import libraries (Epic Games / Ion Storm). They are a build
dependency only: they are not part of this repository and are not redistributed with it.

---

## Infinigen (Princeton University): the bark's relief (Trees3D)

The grown trees' bark - `BarkMap` and `BarkEdge` in `src/RevisionDX12/TreeGen.h` - follows the recipe of Infinigen's
random bark material (`infinigen/assets/materials/plant/bark_random.py`, https://github.com/princeton-vl/infinigen):
a cell net laid in a space squeezed along the trunk and wavered by noise, the distance to a cell's edge as the
crevice, a second, finer net for cracks, and noise of many octaves on the plates; the depth a share of the wood's
radius. The code here is a re-implementation in C++, not a copy of the Python node graph; the notice is kept as
its licence asks for derived work:

	BSD 3-Clause License

	Copyright (c) 2023, Princeton University

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	1. Redistributions of source code must retain the above copyright notice, this
	   list of conditions and the following disclaimer.

	2. Redistributions in binary form must reproduce the above copyright notice,
	   this list of conditions and the following disclaimer in the documentation
	   and/or other materials provided with the distribution.

	3. Neither the name of the copyright holder nor the names of its
	   contributors may be used to endorse or promote products derived from
	   this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
	AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
	IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
	DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
	FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
	DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
	SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
	CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
	OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
	OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

---

## EZ-Tree (Daniel Greenheck): the trees' growth and their leaf cards (Trees3D)

`src/RevisionDX12/TreeGen.h` (`GrowEz`, `EzPresetOf`, `FEzRng`) ports EZ-Tree's tree growth from its `tree.js`, and its
presets' numbers; `assets/SecondRevision-leafcards.bin` is made by `scripts/make_leaf_cards.py` from its oak, ash and
aspen leaf pictures (the aspen's yellow turned green). https://github.com/dgreenheck/ez-tree

```
MIT License

Copyright (c) 2024 Daniel Greenheck

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

## ambientCG: the grown trees' bark (Trees3D)

`assets/SecondRevision-bark.bin` is made by `scripts/make_bark.py` from the colour map of Bark001 by ambientCG
(https://ambientcg.com/a/Bark001), released under the Creative Commons CC0 1.0 Universal licence - in the public domain,
no attribution required; named here all the same.

## New Vision (Dave Watts): the height maps of its textures (Geometry)

The height maps in `heightmaps/NewVision` (in the release: `NewVision\Textures`) are AI generated from the New Vision
textures by Dave Watts, with textures by David Guttridge, Thijs Hagens and Helder Pinto
(https://www.moddb.com/mods/new-vision). New Vision's textures may be used in Deus Ex mods, with credit to New Vision.
These maps are derived from them, so they are for use with Deus Ex and its mods only, under the same terms. They are
not covered by the zlib licence of the renderer's code.

## Not part of any release

The `external/` directory holds reference material that is read, not shipped: other renderers' sources and the game
SDK. No file from it belongs in a release.
