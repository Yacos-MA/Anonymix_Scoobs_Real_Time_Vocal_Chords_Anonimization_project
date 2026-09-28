# V1.1 Windows portable release

## Build and test in Visual Studio

1. Replace the nine source files and nine headers in your working Visual Studio
   project with the files in this package. Do not compile the earlier monolithic
   source file as well. Keep your existing C++20, SDL3, SDL3_ttf and PortAudio
   project settings.
2. Select **Release** and **x64**, then use **Build > Rebuild Solution**.
3. Find the resulting `.exe` in the project's Release x64 output folder.
   Create a new portable distribution folder, copy the `.exe` into it, and copy
   every runtime DLL required by that particular build, including the SDL3,
   SDL3_ttf and PortAudio DLLs when linked dynamically. Check any transitive
   runtime dependencies of those DLLs. Include `logo.bmp` if you use the icon.
4. If your build depends on the Microsoft Visual C++ runtime, install the
   matching or newer supported Visual C++ Redistributable on test machines,
   or configure and distribute the permitted app-local runtime components.
5. Run the app from the extracted distribution folder on a Windows machine
   without your development environment. Test recording, playback, live mode,
   and saving WAV files. WAV files should appear under `recordings/` beside the
   `.exe`. This folder must be writable.
6. Compress the distribution folder as `Anonymix-Scoobs-v1.1-win-x64.zip`.
   Inspect its contents and extract it once more to test the final archive.

## Publish through the GitHub website

Commit the matching sources to the repository first. Under **Releases**, choose
**Draft a new release**, create the `v1.1` tag from that source commit, and
attach the tested Windows ZIP as a release asset. Describe supported Windows
architecture, required Visual C++ runtime, the `recordings/` location, and any
known limitations. Use a prerelease while validating this build with users.

Keep the project source files in the repository. The compiled distribution
belongs to the GitHub Release asset; an `.exe` alone does not contain all of the
runtime libraries required by a dynamically linked build.
