# AI usage

This plugin was built with generative AI.

- **Tool:** Claude Code, Anthropic's coding assistant, using Anthropic's Claude models (Opus).
- **Human role:** Davidian set the requirements and the design direction, supplied the reference schematic, chose the name and decides what is released.
- **What the AI did:**
  - Read the component values and voltages off the reference schematic.
  - Wrote the circuit model, the LV2 plugin and GUI code and the panel layout. The stroke font, renderer and colour scheme are the Davidian house style, reused from the Bi-Lumen.
  - Wrote the build files for Windows, macOS and Linux, the CI workflow, the LV2 metadata, the tests and the documentation.
  - Looked up published tube model parameters and the schematic's background online to check the model.
- **How it was checked:**
  - Automated tests in `test/`: tube operating points against the voltages marked on the schematic, every passive network against an independent frequency-domain solution, tone-control direction, distortion, presence, bypass latency, silence and CPU cost, plus GUI rendering, GUI interaction checks on Windows and macOS, load checks on Linux and macOS, and the lilv LV2 validator.
  - `build.bat test` (Windows) and `make test` (macOS, Linux) run them, and GitHub Actions runs them on every platform for each push.
  - No listening test in a DAW has been recorded yet.
- **Training data and copyright:** no third-party code was knowingly copied. The only reproduced material is the LV2 interface declarations, credited in [NOTICE.md](NOTICE.md). The GUI artwork and font are generated from code, not from images.

The AI's output is released under the project licence, GPL-3.0-or-later (see `LICENSE`). Under the GPL, there is no warranty.
