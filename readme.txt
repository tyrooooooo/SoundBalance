Sound Balance
=============

A tiny Windows 11 system-tray slider to balance Left and Right audio.
Made for broken or uneven earphones where one side is quieter than the other.

Works system-wide on both Bluetooth and wired headphones without lowering
your master volume.


How to Use
----------
1. Run SoundBalance.exe and click OK on the startup popup.
2. Click the headphone icon in your system tray to open the slider.
3. Drag the slider toward Left or Right until both ears sound equal.
   - Use Left/Right arrow keys or mouse wheel for 1% fine-tuning.
   - Double-click the slider (or press 0) to reset to center.
   - Hold down the "Left" or "Right" label to solo that ear.

Your balance is saved per audio device, so plugging in different
headphones automatically loads their own setting.


Calibration & Quick Controls
----------------------------
- Test button: Plays soft mono pink noise so you can center the sound
  without being thrown off by stereo music. Double-click Test to
  alternate Left -> Right -> Both.
- Headphone icon: Click to temporarily turn balancing Off and hear your
  original audio. Middle-clicking the tray icon does the same thing.
- Move window: Drag the top header to place the popup anywhere on screen.
  Double-click the header to snap it back above the tray.


Tray Right-Click Menu
---------------------
- Audio Graph: Optional subtle live visualizer (Off, Background Horizon,
  Track L/R Glow, or Thumb Beat Halo).
- Start with Windows: Launch automatically on login.
- Exit: Closes the app and resets audio back to equal.


Build from Source
-----------------
1. Install Visual Studio with the "Desktop development with C++" workload.
2. Double-click build.bat.
3. SoundBalance.exe will be built as a standalone executable.
