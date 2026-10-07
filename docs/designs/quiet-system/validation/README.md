# Shader preview evidence

**PASS:** eight required general-rendering gates in [evidence.json](evidence.json).
This validates the browser decoration and collapsed-state interaction, not a
native GTK shader or hardware GPU performance.

The numeric fixture renders at 96×32 and 288×96, at fixed times 0 and 8 seconds.
Raw buffers are in [gpu.json](gpu.json); [reference.json](reference.json) reports
comparison with an independent complex-plane harmonic reference for the flow kernel. RGBA8 tolerance is
two code values. Actual maximum kernel error is one. Final composition is checked for opacity
bounds, protected text and premultiplied output. All three studies are exercised.
Resolution scaling and repeated frames match exactly. Deliberately zeroed field
amplitude breaks the kernel reference; frozen time and incorrect resolution
break the expected rendered output. Eight ownership cycles return program/buffer/shader counts
to zero. [preview.json](preview.json) records browser state and layout checks,
including typing-light hold/fade, continuous-input coalescing, timer cleanup,
reduced motion, stable query geometry and keyboard clearing without a clear icon.

Replay from the repository root with the existing Playwright CLI installation
and Chrome. Serve the preview with Python's HTTP server at `127.0.0.1:8767`,
then open it in CLI session `shader-study`. The browser check functions are:

```sh
python3 -m http.server 8767 --bind 127.0.0.1 --directory docs/designs/quiet-system
```

In another shell:

```sh
npm exec --yes --package=@playwright/cli -- playwright-cli -s=shader-study open http://127.0.0.1:8767 --browser chrome
npm exec --yes --package=@playwright/cli -- playwright-cli -s=shader-study run-code --filename docs/designs/quiet-system/validation/check-preview.js
npm exec --yes --package=@playwright/cli -- playwright-cli -s=shader-study run-code --filename docs/designs/quiet-system/validation/check-shader.js
```

Save each CLI `### Result` JSON object as `preview.json` and `gpu.json`
respectively, then run `python3 docs/designs/quiet-system/validation/reference.py`.
This regenerates the numeric report and evidence manifest from those buffers.
The recorded run used Chrome 152 and SwiftShader; timer queries were unavailable.
CPU dispatch samples are below the browser timer's resolution and do not provide
a performance measurement. Background visibility checks dispatch a controlled
visibility event; context loss/restoration uses the real WebGL test extension.
