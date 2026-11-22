# SteadyXR changelog

## 1.1.0

- A signed Windows installer is available alongside the portable ZIP, with a Start menu entry, optional desktop shortcut and uninstaller. Both downloads contain the same 1.1.0 application.
- **Render / window resolution** now sizes the Stream Output window's client area. Choose 2560 × 1440 and the window opens at 2560 × 1440 pixels, including on ultrawide monitors. **Apply output** resizes an already open window.
- Choose **Fullscreen**, **Windowed** or **Borderless** as the startup/output mode. Fullscreen keeps the selected aspect ratio centered with black borders. Borderless windows can be moved and resized by dragging the image and its edges.
- Added **Direct OBS output (plugin)**: send the finished image to a **SteadyXR** OBS source without a preview window or Window Capture. Includes smoothing, framing, effects and monitor fallback.
- **Install OBS integration…** offers an OBS folder picker with the standard installation path. The OBS source can optionally start SteadyXR and close the instance it started when the last source using that option is removed or OBS closes.

## 1.0.4

- Refresh the output monitor list when displays connect or disconnect. Remember the selected output when it is unavailable and return to it when it reconnects.
- If Windows interrupts monitor capture during a display change, Stream Output holds the last clone image for up to two seconds while reconnecting. Longer interruptions show a waiting screen. The technical error stays in the dashboard under **Connection and capture details**.

## 1.0.3

- Choose what viewers see when OpenXR frames stop. Open **Stream Output → OpenXR fallback…** to keep the waiting screen, hide the output window until frames return, or clone a monitor.
- In iRacing and Le Mans Ultimate, enable **Use fallback outside the car** to show the game menu on your main display and switch back to VR when you enter the car. A stationary car in the pits still counts.
- Monitor cloning uses Windows Desktop Duplication without a yellow capture border. SteadyXR releases the monitor capture when VR output resumes.

## 1.0.2

- Added **Advanced smoothing** with independent horizontal and vertical controls, as requested by **Coltangent**. Enable it under **Camera** for OpenXR or window capture. Custom presets remember both values.
- Renamed Dan’s original 4K preset to **Dan Suzuki (Motion)**. The new **Dan Suzuki (No Motion)** preset uses 40% smoothing and a 10% mirror limit for stationary rigs.
- Added a **Tutorial** button to the dashboard header. [Watch Dan’s SteadyXR tutorial](https://youtu.be/kNgVMJ82mQU)

[Earlier releases](https://github.com/DSuzuki35/steadyXR/releases)
