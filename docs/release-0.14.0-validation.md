# SteadyXR 0.14.0 — D3D12 OpenXR capture

## Behavior

The capture DLL automatically recognizes D3D11 and D3D12 OpenXR sessions. D3D12 uses a native GPU snapshot before the application releases its swapchain image. After a successful frame submission, that snapshot is paired with the submitted pose, FOV, eye, rectangle and display time. The existing standalone renderer and OBS source receive the same IPC 4 frames as before. There is no CPU video readback or encoder in this path.

A companion D3D11 device on the same adapter waits for capture completion on its own GPU queue and copies to the existing shared output. The game's D3D12 queue never waits for the viewer. At most three slots per swapchain are allocated, with a shared 512 MiB snapshot budget including retired sessions. If resources are still busy or the budget is exhausted, capture skips rather than waiting in the game path. The slot ring is reused after both the game copy and companion read have completed.

Array textures, mip-zero selection, supported SDR/float formats and MSAA resolve retain the same view handling as D3D11. Static images, selected overlays, backpressure, pause, reconnect and delayed framing use the existing pipeline. Protected content is not captured. An unsupported interop driver or removed device disables capture and reports status while OpenXR calls continue to the runtime.

The 0.13.1 changes remain included: no build expiry, system-wide registration with legacy-user migration, safe VectorXR startup, and SteadyXR ordered before VectorXR. VectorXR's generated intermediate frames are not promised: the installed order captures the game's original submissions, whether D3D11 or D3D12.

## Verification

The final Windows x64 Release build and all 41 CTest checks passed on 2026-10-22. The candidate includes `automated-tests.log` with the complete run (70.89 seconds).

Native GPU tests use Windows WARP and the installed hardware adapter. They cover all six accepted typed formats and their typeless source families, array slices with multiple mips, MSAA resolve, subrect pixels, immediate runtime overwrites, bounded pool backpressure and delayed completion, invalid bindings/formats, and resource retirement. The existing full layer fixture also runs in D3D12 mode, checking release-time pixels, submitted pose metadata, overlays, pause, ownership/reconnect, failed runtime calls and unchanged forwarding. The D3D11 fixture remains enabled.

A separate GPU test blocks only the companion reader after capture has completed: the snapshot remains pinned while the application queue continues to progress. A teardown test holds capture work beyond the 250 ms cleanup deadline, verifies that the source survives in quarantine, then verifies release after completion. Device-loss policy tests independently cover each device and unknown fence values; actual GPU device removal is not deliberately triggered. Code review identified and corrected a case where companion-only removal could incorrectly discharge pending game-queue work.

The D3D12 layer fixture fails against the preserved 0.13.1 layer with its unsupported-backend message and passes with the new backend. A deliberately incorrect mip-stride implementation is detected by the native array test. This machine does not have the optional D3D12 SDK debug validation layer; tests verify real resource/pixel behavior but do not claim a debug-layer validation pass.

Automated checks do not establish compatibility with every runtime, GPU driver or anti-cheat game. Live LMU + VectorXR and a native D3D12 OpenXR game test remain pending a signed capture DLL. Real game performance is not yet measured.

## Delivery and signing

The candidate is isolated under `Needs-signing/SteadyXR-0.14.0`, with app, OBS drop-in folders, complete and split ZIPs, corresponding source and SHA-256 checksums. Sign the candidate's `Gaming-PC/SteadyXR-0.14.0/SteadyXR-OpenXR.dll`, then regenerate packages/checksums from the exact signed result. Install using **Install for all users / Upgrade connection**, approve the Windows administrator prompt, and restart the OpenXR game. The installed signed 0.12.0 DLL and delivered 0.13.0/0.13.1 packages remain unchanged.

## API references

- [OpenXR D3D12 swapchain image state](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_D3D12_enable-swapchain-image-state.html): runtime color images must be restored to render-target state and available on the binding queue before release.
- [D3D12 shared handles](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-createsharedhandle) and [sharing tiers](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_shared_resource_compatibility_tier): shared typed snapshots and fences bridge the two graphics APIs.
- [D3D11 GPU wait](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-wait) and [signal](https://learn.microsoft.com/en-us/windows/win32/api/d3d11_3/nf-d3d11_3-id3d11devicecontext4-signal): synchronization is queued on the companion device.
