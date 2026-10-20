FM2 Vulkan

The active build uses the same source SDK and ABI-2 GPU plugin as the new FM4
build in ../../fm4-recomp/work/vulkan-trial. The release SDK (ABI 1, D3D12-only)
cannot load this plugin. Never mix their runtime DLLs.

Build with build-vulkan.bat, optionally passing another FM4 Vulkan tree path.
Output: out/build/win-amd64-vulkan. Start with ../launch.cmd or run.bat.
The FM4 host CMakeLists includes cmake/vulkan-targets.cmake when FM2_ROOT is set.
The existing release build's pinned ImGui/miniz sources are reused.

Both SDL launcher UI and the game explicitly select Vulkan. Legacy user config
backend/plugin selections are overridden before GPU setup. Launcher saves use
fh1/Vulkan, async pipelines, 720p guest mode, High 2x/AF16x/bilinear quality, matching the FM2 Xenia profile.
VSync defaults off like the FM4 Vulkan reference; 60/120 Hz remains selectable.
Only Vulkan FSI/FBO pipeline caches are merged; changing from D3D12 invalidates
the warm-up fingerprint. Profile/save locations are unchanged.

FM4-specific engine timing patches are not applied to FM2.

Validation: game and launcher compiled; Vulkan warm-up exited 0 on RTX 4070 SUPER; launcher graphics screenshot rendered successfully with SDL Vulkan. A race has not been validated.
The shared SDK SDL configuration now enables SDL_VULKAN on Windows when REXGLUE_USE_VULKAN is enabled, for the launcher's SDL_GPU renderer.

FM2 bodywork correction (2026-10-19): use render_target_path_vulkan=fbo and vulkan_fh1_native_executor=false. The host must not force FSI. FSI caused visible bodywork artifacts; the user confirmed correct rendering after switching to generic FBO. Keep both settings in the base config and launcher saves. The warm-up fingerprint is vulkan-fbo-fm2-v3.

## Pipeline stalls at 120 FPS

FM2 enables vulkan_async_pipeline_submission_wait=false and
vulkan_async_pipeline_skip_pending=true. The shared SDK keeps its previous
defaults for other applications. Its patch is saved at
cmake/patches/vulkan-async-pipelines.patch (apply to the referenced SDK source).

The GPU command processor already skips placeholder draws. Building a temporary
VkPipeline synchronously for those draws serves no rendering purpose. FM2 now
queues the final pipeline directly, and submission end no longer drains/waits
for that async work. Pending handles remain pending until the worker publishes
the complete pipeline/layout; failed creation leaves the pending state so the
existing error/retry path can run. Explicit blocking startup warm-up is retained.

Validation: game and launcher built; isolated sessions reached race scenes;
blocking warm-up reconstructed all 448 recorded pipelines successfully;
launcher settings round-trip preserves async flags, FBO and 120 Hz selection.
Frame statistics now also count intervals above 10 ms for 120 FPS diagnostics.

The initial frame-time runs showed jitter even when pipeline-submission waits
were under 1 ms. A guest-clock/presentation-limiter experiment did not establish
a clear improvement, so those settings remain unchanged. The two race captures
were different scenes; their frame-time figures are not a controlled benchmark
and do not establish a performance percentage. New shader translation and
content streaming may still cause first-use hitches. Confirm remaining hitching
on the same warmed track/car as Xenia before claiming full resolution.
