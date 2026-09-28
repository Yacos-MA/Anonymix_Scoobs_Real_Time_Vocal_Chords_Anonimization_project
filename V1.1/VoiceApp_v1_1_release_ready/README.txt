VoiceApp V1.1 - English source and developer documentation (portable release fixes)

Start with DEVELOPER_GUIDE.md for the project map, audio and UI flow, thread
ownership, known limitations and a validation checklist.

Visual Studio Community 2026 v18.10.2:
  1. Add all nine .cpp files as Source Files and all nine .h files as Header Files
     to a copy of your existing working Visual Studio project.
  2. Exclude the earlier monolithic .cpp from the build to avoid duplicate SDL
     entry points and shared-state definitions.
  3. Preserve C++20, your SDL3, SDL3_ttf and PortAudio include/library paths,
     linker options and runtime DLL configuration.
  4. Do not add main(): App.cpp uses SDL_MAIN_USE_CALLBACKS and provides the
     four SDL app callbacks.

Files: App.cpp, AppRender.cpp, AppUpdate.cpp, Audio.cpp/.h, Common.h,
Dsp.cpp/.h, State.cpp/.h, Stream.cpp/.h, Visualization.cpp/.h, Wav.cpp/.h,
Widgets.h and AppFrame.h.

This package translates project-specific identifiers, comments, user-visible
strings and documentation into English. It keeps the previous algorithms and
DSP settings. The documented V1.1 version already initialized numeric widget
members read during construction. Windows compilation and live audio testing
must be performed in your Visual Studio environment; this workspace lacks
that SDK and hardware setup.

Portable release corrections: App.cpp disposes of the logo surface once and
uses SDL3_ttf's boolean initialization result while owning one font handle.
Wav.cpp creates recordings/ beside the executable, so extract the archive into
a writable folder before recording. Build and test Release x64, then package
the executable, all required runtime DLLs and optional logo.bmp together.
