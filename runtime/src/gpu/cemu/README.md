# Code vendored from Cemu

Files under this folder (except `Common/precompiled.h`, Windfall's compatibility header) are copied from
[Cemu](https://github.com/cemu-project/Cemu), commit `29f2f1fae4631e2317977debd9eacd55cc28dd7d`, and remain
under the **Mozilla Public License 2.0** (`LICENSE.txt` in this folder). Folder layout mirrors Cemu's `src/`
so its include paths work unchanged. Local modifications are marked with `(windfall)` comments; line endings
were normalized to LF.

Windfall-authored stand-ins for the Cemu subsystems this code expects (coreinit, the PPC interpreter state,
renderer, settings, ...) live in `../cemu_shim/` with the same paths, and the glue is in
`runtime/src/hle/cemu_bridge.cpp` and `runtime/src/hle/gx2_windfall.cpp`.

Vendored:
- `Cafe/OS/libs/gx2/` — GX2 (graphics API) HLE that emits the Latte PM4 command stream.
  `GX2_Event.cpp`, `GX2_Memory.cpp` and `GX2_TilingAperture.cpp` are kept for reference but not compiled;
  `hle/gx2_windfall.cpp` replaces them on top of Windfall's scheduler.
- `Cafe/OS/libs/TCL/` — the command ring between GX2 and the GPU thread
- `Cafe/OS/libs/snd_core/` — AX audio (voice mixing, ADPCM, sample-rate conversion, aux effects, device remix);
  its interrupt-service thread is provided by `hle/ax_windfall.cpp`
- `Cafe/HW/Latte/Core/` — command processor, GPU thread, texture/buffer/render-target caches, surface copies,
  shader cache, overlay, timing
- `Cafe/HW/Latte/LegacyShaderDecompiler/`, `Transcompiler/`, `ShaderInfo/`, `ISA/`, `Common/` — R600 shader
  analysis and GLSL generation, shader/register serialization
- `Cafe/HW/Latte/Renderer/` and `Renderer/Vulkan/` — the renderer interface and Vulkan backend
- `Cafe/HW/Latte/LatteAddrLib/` — surface size/tiling address library
- `Cafe/OS/common/`, `Cafe/OS/RPL/COSModule.h`, `Cafe/HW/Espresso/PPCCallback.h`, `Const.h`
- `Common/` (`betype.h`, `MemPtr.h`, `enumFlags.h`, `SysAllocator.*`, `StackAllocator.h`, `FileStream`,
  `cpu_features`, `unix/fast_float.h`), `config/ConfigValue.h`, `Cemu/FileCache/`, `imgui/` (Cemu's ImGui
  Vulkan backend and helpers), `resource/IconsFontAwesome5.h`, and `util/` helpers (containers, Zir IR,
  timers, system info, DXGI wrapper, TGA/boot-sound readers)

Not vendored: Cemu's embedded console system font (`resource/CafeDefaultFont.cpp`); the overlay uses the
Windows UI font instead, so no console data is shipped.

Local modifications (`(windfall)` comments):
- `Common/MemPtr.h` (allocator includes), `Cafe/HW/Espresso/PPCCallback.h` (include path)
- `Common/cpu_features.h` (target attributes under clang-cl), `Cafe/HW/Latte/Core/LatteTextureCache.cpp` (AVX2
  hashing loop moved into a target-attributed helper)
- `Cafe/OS/libs/snd_core/ax_ist.cpp`, `ax_internal.h` (the IST thread is Windfall's, see above)
- `Cafe/HW/Latte/Core/LatteRenderTarget.cpp` (GamePad picture-in-picture overlay)
- `Cafe/HW/Latte/Renderer/Vulkan/VulkanRendererCore.cpp` (`WF_GPUDUMP` hook)
