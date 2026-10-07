# Quiet System

**Status: archived browser design study. Its shader-free presentation is implemented in GTK.**

The [native implementation](../../adr/0027-quiet-system-native-popup.md) adopts
the minimal layout, cursor and typing feedback. Shader artwork remains an
experiment excluded from the native build. Statements below about GTK being
unchanged describe the original proposal stage; see [native captures](../../ui/README.md)
for the implemented UI.

- [Screenshot](preview.png)
- [Launcher close-up](launcher.png)
- [Search results](results.png)
- [Border while typing](typing.png)
- Shader comparison: [open sheet](art-studies.html) · [screenshot](art-studies.png)
- Artwork studies: [spectral silk](art-study-0.png), [ion cloud](art-study-1.png), [prism fold](art-study-2.png)
- [Editable launcher SVG](quiet-system.svg)
- [Editable results SVG](results.svg)
- [Empty and error state SVGs](states.svg)
- [Native Figma construction source](figma.js) (syntax checked, not executed)

Open [the interactive preview](index.html) in a browser. It starts with an empty
query and a collapsed popup. Type `config`, use Up/Down, Enter, Ctrl+Enter and Escape, or select the
preview states below the popup. Actions display a preview message and do not open
applications or files. Native focus-loss dismissal is outside this browser study.

This direction uses a 680-pixel graphite surface, a six-pixel outer radius,
two-pixel selection rail and a moss-green accent for controls. JetBrains Mono Medium at
24px carries the query; its regular face carries filenames, paths, the wordmark
and keyboard hints. Space Grotesk carries application names.
The header contains only the small wordmark; the former tagline is removed.
Icons have no tiles, selection has no enclosing card, and the search input is
part of the window surface. Empty search contains only the wordmark and search
field: no recommendations, results region, divider, footer or empty-state copy.
The popup is 116px tall when empty and grows downward once matches appear.
Clearing or entering whitespace collapses it. No-match and offline feedback are
one compact line inside the search area, with inline Retry for offline search;
they do not create a lower panel. The results footer appears only with matches.

The clear icon is removed, giving the query more room. Ctrl+A then Backspace
clears the text. On actual edits (including paste and deletion), a faint moss
edge appears around the search area over 120ms. It holds until 700ms after the
last edit, then fades over 420ms. Continuous typing extends one hold rather than
pulsing on every keystroke. The results panel stays quiet; arrow navigation does
not trigger the light. Blur and Escape cancel the hold. Reduced motion switches
the edge instantly with no transition. Border geometry and input position stay
fixed throughout.

The search field now previews a 12-by-2-pixel green underscore caret. It tracks
editing within the query, hides during selection or focus loss, and stays still
under reduced-motion preferences. The native input still owns text editing;
during IME composition the preview returns to the browser's native caret.
This is a browser design change; GTK needs custom drawing for the same shape.
Cursor checks cover Home/End movement, selection hiding, ordinary insertion and
reduced motion. The screenshots and launcher SVG include the new caret; the
state sheet shows each inactive state without focus.

The former concentric contours are replaced by three original fragment-shader
studies in [search-art.js](search-art.js). Use the artwork controls outside the
popup to compare spectral silk (iridescent woven filaments), ion cloud (soft
luminous vapor) and prism fold (sharp repeated facets). Spectral silk is the
default. Teal, violet and pale highlights give the artwork more depth while
controls retain their moss accent. These are procedural visual studies, not
physical simulations of refraction or volumetric lighting.

Research included the Shadertoy [Protean Clouds reference](https://www.shadertoy.com/view/3l23Rh),
its [public Cables port](https://cables.gl/p/eYwksD), and the
[domain-warping examples in The Book of Shaders](https://thebookofshaders.com/13/).
Direct Shadertoy pages and thumbnails returned access errors. The implementation
is original code; no Shadertoy shader source or third-party artwork is bundled.

Artwork sits behind the input and cannot intercept clicks. Its left 42% is
transparent, the right edge fades out, and opacity is capped at 78%.
Narrow previews halve the art's opacity. The pattern flows slowly rather than
blinking or reacting to every keystroke. Animation draws are throttled to 30 updates per second, with backing
resolution capped at device scale 2. The renderer pauses when dismissed or the
document is hidden. Reduced motion draws one still frame; unavailable WebGL,
compilation failure and context loss show a frozen PNG of the selected study.
Context restoration rebuilds resources. No application dependencies are added.

The popup grows downward from a fixed top edge, so the search field stays put
when results change or compact error messages appear. Preview controls reserve the
tallest fixture's space. Paths shorten by removing earlier ancestors while
preserving the home/root and as many trailing folders as fit, for example
`~/…/torchlight/src/core`. A single very long folder shortens in its middle.
Hover and accessible labels retain the full path; matching uses the original.
Resizing and font loading recalculate the display text.
Narrow application subtitles drop the redundant `Application ·` prefix first.

Motion is deliberately small: opening fades in with a six-pixel rise over
160ms; result updates fade from 65% to full opacity over 100ms; selection color
and its rail transition over 100ms without rebuilding rows. Rapid updates cancel
the previous fade. Reduced-motion preferences disable these effects and caret
blinking, including when the preference changes while the preview is open;
the shader remains a still image under this preference.

The state/artwork controls and outer captions are presentation aids, not application
controls. The data is illustrative. This explores custom colors and typography
instead of the current GTK theme, subject to approval and native accessibility
and scaling checks before implementation. No application dependencies change.

Figma's Starter-plan tool quota blocked creating new native Figma layers for
this revision. The editable SVG and native-layer construction script provide
the design source; the script can be submitted through `use_figma` when access
resumes. Importing the SVG into Figma preserves vector geometry but may convert
text to outlines; the script creates actual editable text. Figma/SVG sources
include a frozen PNG artwork layer, not a runnable shader. Window geometry and
text remain separate editable layers; live artwork is edited in the GLSL source.

Font assets are bundled for offline preview, under the accompanying OFL licenses:
[JetBrains Mono](https://github.com/JetBrains/JetBrainsMono) and
[Space Grotesk](https://github.com/floriankarsten/space-grotesk).

Verified in Chrome: both fonts load; arrow selection keeps input focus;
Ctrl+Enter identifies the selected reveal action; empty/no-match states have no
visible result rows; offline preserves the query; Retry restores results; Escape
dismisses; the 320- and 390-pixel previews have no horizontal overflow. Search
position is identical across all states and when filtering to one result. Paths
fit their columns and preserve their full accessible labels. Opening, result and
selection timing was checked; changing to reduced motion stops active effects
and disables effects on reopening. Collapsed states are 116px tall, including
no-match/offline feedback. Shader pause/resume and actual context loss/restoration
were exercised. An independent double-precision complex-plane harmonic reference
matched the actual flow kernel within one RGBA8 code value (tolerance: two).
Final composition is checked separately for premultiplication, opacity bounds
and a transparent query region. All three studies produce identical repeated
frames and preserve corresponding pixels under threefold resolution scaling.
Disabling strength clears output; zeroed field amplitude, frozen time and
incorrect resolution are detected. Eight resource create/destroy cycles leave no
owned GPU objects or GL errors. The evidence verifier passes all eight required
checks for its general-rendering profile.
The [evidence manifest](validation/evidence.json), raw RGBA output, reference
results and browser check functions are retained in [validation/](validation/).

These checks ran in Chrome with SwiftShader. Hardware GPU timing and native GTK
shader integration remain untested; no GPU performance claim is made. Desktop,
narrow and error-state screenshots were visually reviewed. The interactive
preview reports no browser console errors. Production code is unchanged, so C tests, lint and benchmarks
were not rerun for this visual proposal.
