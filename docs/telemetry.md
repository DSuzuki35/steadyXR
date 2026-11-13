# Speed framing and in-car capture

SteadyXR's speed framing reads the local player's speed from iRacing or the
native Le Mans Ultimate shared-memory interface. Enable it in the viewer camera
controls. Automatic selection prefers valid iRacing telemetry; a game can also
be selected explicitly. No additional telemetry plugin is installed or enabled,
and no game, runtime, registry, or plugin configuration is changed.

Speed framing and in-car capture each poll telemetry only while enabled. Each
reader samples at most 20 times per second and retries unavailable mappings at
most once per second per game. Speed framing accepts a reading after observing
source time advance and expires it after 500 ms without further progress.
Disconnected, paused, off-track, malformed, or contended data returns an invalid
reading so the camera can ease back to its ordinary framing. Disabling the
feature closes its mappings.

In-car capture checks the game that produced the OpenXR frames. It accepts
OpenXR only when that simulator reports the local driver in the car and its
telemetry advances. A stopped car qualifies. Menus, missing telemetry, an
unsupported producer and expired data select the configured fallback. A brief
LMU lock miss preserves a confirmed cockpit until its original 500 ms freshness
deadline. An explicit exit takes effect on the next poll. The speed-framing
game selection is independent.

The capture consumer must be on the same Windows computer/session as the game
for these named mappings to be available. They are not transported through a
capture card or the existing two-PC image stream.

## iRacing

The reader opens `Local\IRSDKMemMapFileName` with `FILE_MAP_READ`. It independently
parses the published IRSDK v1/v2 byte layout, discovers `Speed`, `IsOnTrack`,
`SessionState`, `SessionNum`, and `SessionTime` in the variable descriptors, and
selects the newest data-buffer tick. Speed is converted from metres per second
to kilometres per hour; reverse speed uses its magnitude.

For in-car capture, the reader uses the SDK's `IsOnTrack` flag for the local
driver and checks `IsGarageVisible` for the garage screen. It confirms that the
session time and buffer tick advance. This check works while parked and requires
no speed data. [IRSDK variable descriptions](https://github.com/kutu/pyirsdk/blob/master/vars.txt)

Metadata, scalar types, counts, names, offsets, session state, and numerical
values are checked before use. After copying the selected fields, the reader
checks the variable descriptors, header metadata, and the selected buffer's
tick/offset again to reject an overwritten or changing snapshot. No broadcast
control commands, telemetry logging commands, or game writes are used.

Protocol provenance:
[iRacing-authored IRSDK definitions](https://github.com/vipoo/irsdk/blob/master/irsdk_defines.h)
as published in the SDK source mirror. This implementation contains an
independent byte reader rather than copied SDK implementations or headers.

## Le Mans Ultimate

This implementation targets the native x64 `LMU_Data` interface described by
the installed Studio 397 SDK in
`Le Mans Ultimate\Support\SharedMemoryInterface`. It requires an LMU build that
already provides that interface and its synchronization objects. An older build
or absent objects leaves speed unavailable. The rFactor 2 third-party
shared-memory plugin is not used.

`LMU_Data` is mapped with `FILE_MAP_READ`. The native SDK protects snapshots with
a dedicated shared lock. SteadyXR opens the existing
`LMU_SharedMemoryLockData` and `LMU_SharedMemoryLockEvent`, makes one nonblocking
compare-and-exchange attempt on the lock's busy word, reads the required player
fields, and releases the lock. If another participant has registered waiters,
it signals the existing wake event. It never spins, waits for a game frame,
creates/resets these objects, or changes the waiter count. **Only the SDK's
separate synchronization word is writable; telemetry and game state stay
read-only.** Brief lock ownership can delay another SDK participant for the
duration of this small read; it is not a game-thread hook.

The player index and vehicle counts must fit the SDK's 104-entry array. The
application must be on track, scoring must be in realtime in an active session,
and `playerHasVehicle` must be true. Speed is the magnitude of the selected
player's three local-velocity components, converted from metres per second.
Freshness follows that vehicle's elapsed session time. No foreign pointers
embedded in the shared memory are dereferenced.

In-car capture checks application location (main UI, loading, monitor or on
track), realtime scoring, session phase and player vehicle availability. It
matches the telemetry vehicle ID to the scoring vehicle ID, checks
`mIsPlayer` and requires `mControl` to designate the local driver. Scoring
and telemetry can list cars in different orders. Pause, monitor, spectator,
AI, remote and replay states all select the fallback. The shared-memory
interface exposes driver participation but omits the active camera angle.

The SDK header prohibits redistribution, so it is not included in this source
or delivery. Numeric layout facts were obtained by compiling a local
`sizeof`/`offsetof` probe against the original installed headers, without
modifying or copying them into the project:

| x64 layout item | Byte size or offset |
| --- | ---: |
| `SharedMemoryLayout` size | 324824 |
| Application options-location byte | 96 |
| Scoring block | 1632 |
| Scoring session / vehicle count / phase / realtime, within scoring | 64 / 104 / 108 / 115 |
| Scoring vehicle array, within scoring | 560 |
| Scoring vehicle stride | 584 |
| ID / is player / control, within scoring vehicle | 0 / 196 / 197 |
| Telemetry block | 128464 |
| Active vehicles / player index / has vehicle, within telemetry | 0 / 1 / 2 |
| Vehicle array, within telemetry | 4 |
| Vehicle record stride | 1888 |
| Vehicle ID / elapsed time / local velocity, within vehicle | 0 / 12 / 184 |

The probe used Windows x64, the SDK's four-byte packing for internal plugin
records, and normal outer-structure alignment. The verified local files were:

- `SharedMemoryInterface.hpp`, modified 2026-09-28; SHA-256
  `A82833D5E9E277A7C3AF8802518E35EA62F4EC7C7C7C9083650282E4EDF1F8BF`.
- `InternalsPlugin.hpp`, modified 2026-03-31; SHA-256
  `9B6EE8CF610FA5049B18DF580A9A9BC9EBB91346FC466584D576A6442ABCF68F`.

This native interface does not publish a self-describing layout/version header.
Future SDK layout changes require another ABI check; the documented support is
for this verified layout. Size, range, state, and value validation cannot prove
compatibility with every future game build. Live game validation is still needed
before claiming tested operation with a particular running release.

## Verification

`preview/speed_telemetry_tests.cpp` uses independent synthetic byte fixtures and
isolated Windows named mappings. It covers dynamic iRacing variable positions,
buffer rotation and tick wrap, torn snapshots, invalid bounds/types/states,
player selection, 20 Hz polling, one-second reconnect attempts, 500 ms expiry,
source/session changes, LMU lock contention/release/wake behavior, missing native
objects, and mapping cleanup. In-car fixtures cover a stationary driver, a
moving unoccupied car, menus, pause, replay, producer changes, mismatched
vehicle IDs, and cockpit expiry during sustained lock contention. Tests neither attach to real game mappings nor
require a running game.

The test translation unit includes `speed_telemetry.cpp` to reach the internal
clocked polling and parser boundaries. Compile it on its own; do not additionally
link another copy of the implementation. The production renderer compiles
`speed_telemetry.cpp` normally. No libraries beyond the existing Windows system
libraries are required.
