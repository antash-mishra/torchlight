/* Original spectral flow studies for the isolated preview; no search work. */
"use strict";

const SEARCH_ART_VERTEX = `
attribute vec2 a_position;
void main() { gl_Position = vec4(a_position, 0.0, 1.0); }
`;

const SEARCH_ART_FRAGMENT = `
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform vec2 u_resolution;
uniform float u_time;
uniform float u_strength;
uniform float u_variant;

// Four differently oriented wave scales form an irregular, continuous field.
float flow(vec2 p) {
  float value = 0.0;
  float amplitude = 0.52;
  for (int octave = 0; octave < 4; octave++) {
    value += amplitude * sin(p.x + 0.7 * cos(p.y))
                       * cos(p.y - 0.6 * sin(p.x));
    p = mat2(0.8, 0.6, -0.6, 0.8) * p * 2.03 + vec2(1.7, -2.6);
    amplitude *= 0.48;
  }
  return value;
}

vec4 spectralSilk(vec2 p, float time) {
  vec2 drift = vec2(time * 0.065, -time * 0.04);
  vec2 q = vec2(flow(p + drift), flow(p + vec2(4.8, -3.2) - drift));
  vec2 r = vec2(flow(p + 1.4 * q + vec2(2.8, 5.3) - drift),
                flow(p + 1.9 * q + vec2(-4.6, 1.7) + drift));
  float f = flow(p + 2.1 * r + 0.5 * q);
  float thread = pow(1.0 - abs(sin((f + 0.35 * r.x) * 14.0 + p.y * 3.0)), 12.0);
  float body = smoothstep(-0.55, 0.65, f);
  float tint = 0.5 + 0.5 * sin(f * 3.0 + r.y * 2.0 + time * 0.055);
  vec3 color = mix(vec3(0.39, 0.20, 0.73), vec3(0.13, 0.73, 0.63), tint);
  color = mix(color, vec3(0.84, 0.58, 0.67), smoothstep(0.2, 0.65, r.x) * 0.55);
  color = mix(color, vec3(0.86, 0.81, 0.98), thread * 0.85);
  return vec4(color, 0.08 + 0.38 * body + 0.4 * thread);
}

vec4 ionCloud(vec2 p, float time) {
  p *= 1.15;
  vec2 drift = vec2(time * 0.035, time * 0.055);
  vec2 warp = vec2(flow(p + drift), flow(p + vec2(5.1, 2.7) - drift));
  float f = flow(p + warp * 2.4 + vec2(1.2, -3.1));
  float mist = pow(clamp(0.55 + 0.9 * f, 0.0, 1.0), 2.0);
  float edge = exp(-abs(f - 0.08) * 22.0);
  float vein = pow(1.0 - abs(sin(f * 24.0 + warp.y * 2.0)), 18.0);
  vec3 color = mix(vec3(0.32, 0.17, 0.53), vec3(0.16, 0.61, 0.76), mist);
  color = mix(color, vec3(0.77, 0.90, 0.88), edge * 0.55 + vein * 0.35);
  return vec4(color, 0.03 + 0.54 * mist + 0.18 * edge + 0.1 * vein);
}

vec4 prismFold(vec2 p, float time) {
  float angle = 0.32 + 0.08 * sin(time * 0.07);
  mat2 turn = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
  vec2 crystal = p + vec2(0.1 * sin(time * 0.09), 0.06 * cos(time * 0.13));
  float nearest = 5.0;
  float scale = 1.0;
  float facet = 0.0;
  for (int fold = 0; fold < 5; fold++) {
    crystal = turn * (abs(crystal) - vec2(0.72, 0.38));
    nearest = min(nearest, abs(crystal.y) / scale);
    facet += exp(-abs(crystal.x * crystal.y) * 6.0) / scale;
    crystal *= 1.6;
    scale *= 1.6;
  }
  float edge = exp(-nearest * 110.0);
  float glass = clamp(facet * 0.3, 0.0, 1.0);
  vec3 color = mix(vec3(0.25, 0.20, 0.50), vec3(0.08, 0.65, 0.73), glass);
  color = mix(color, vec3(0.84, 0.76, 0.59), edge * 0.75);
  return vec4(color, 0.05 + 0.16 * glass + 0.58 * edge);
}

void main() {
  vec2 uv = gl_FragCoord.xy / u_resolution;
  vec2 p = (uv - vec2(0.79, 0.5)) * vec2(5.0, 2.3);
  vec4 art;
  if (u_variant < 0.5) art = spectralSilk(p, u_time);
  else if (u_variant < 1.5) art = ionCloud(p, u_time);
  else art = prismFold(p, u_time);
  // Protect the left-hand query area; fade away before the clear button.
  float mask = smoothstep(0.42, 0.68, uv.x)
             * (1.0 - smoothstep(0.93, 1.0, uv.x))
             * smoothstep(0.0, 0.16, uv.y)
             * (1.0 - smoothstep(0.82, 1.0, uv.y));
  float alpha = clamp(u_strength, 0.0, 1.0) * mask * min(art.a, 0.78);
  gl_FragColor = vec4(art.rgb * alpha, alpha);
}
`;

/** Owns one canvas's GPU resources and animation subscriptions until destroy(). */
class SearchArt {
  /** Create the decoration; failed WebGL initialization keeps the static SVG. */
  static create(canvas) { return new SearchArt(canvas); }

  constructor(canvas) {
    this.canvas = canvas;
    this.surface = canvas.parentElement;
    this.showFallback();
    this.gl = canvas.getContext("webgl", {
      alpha: true, premultipliedAlpha: true, antialias: false,
      depth: false, stencil: false, powerPreference: "low-power",
    });
    this.program = null;
    this.buffer = null;
    this.frame = null;
    this.time = 0;
    this.variant = 0;
    this.lastTick = null;
    this.visible = true;
    this.destroyed = false;
    this.motion = matchMedia("(prefers-reduced-motion: reduce)");
    this.onMotion = () => this.refresh();
    this.onVisibility = () => this.refresh();
    this.onLost = (event) => {
      event.preventDefault();
      this.stop(); this.program = null; this.buffer = null;
      this.showFallback();
    };
    this.onRestored = () => { if (this.initialize()) this.resize(); };
    this.observer = new ResizeObserver(() => this.resize());
    this.observer.observe(this.surface);
    this.motion.addEventListener("change", this.onMotion);
    document.addEventListener("visibilitychange", this.onVisibility);
    canvas.addEventListener("webglcontextlost", this.onLost);
    canvas.addEventListener("webglcontextrestored", this.onRestored);
    if (this.initialize()) this.resize();
  }

  compile(type, source) {
    const gl = this.gl, shader = gl.createShader(type);
    if (!shader) return null;
    gl.shaderSource(shader, source); gl.compileShader(shader);
    if (gl.getShaderParameter(shader, gl.COMPILE_STATUS)) return shader;
    gl.deleteShader(shader); return null;
  }

  initialize() {
    const gl = this.gl;
    if (!gl || gl.isContextLost() || this.destroyed) return false;
    const vertex = this.compile(gl.VERTEX_SHADER, SEARCH_ART_VERTEX);
    const fragment = this.compile(gl.FRAGMENT_SHADER, SEARCH_ART_FRAGMENT);
    const program = vertex && fragment ? gl.createProgram() : null;
    if (program) {
      gl.attachShader(program, vertex); gl.attachShader(program, fragment);
      gl.linkProgram(program);
    }
    if (vertex) gl.deleteShader(vertex);
    if (fragment) gl.deleteShader(fragment);
    if (!program || !gl.getProgramParameter(program, gl.LINK_STATUS)) {
      if (program) gl.deleteProgram(program);
      this.showFallback(); return false;
    }
    this.program = program;
    this.buffer = gl.createBuffer();
    if (!this.buffer) { gl.deleteProgram(program); this.program = null; return false; }
    gl.useProgram(program); gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    this.position = gl.getAttribLocation(program, "a_position");
    this.uniforms = Object.fromEntries(["resolution", "time", "strength", "variant"].map(name => [name, gl.getUniformLocation(program, `u_${name}`)]));
    this.canvas.hidden = false;
    this.surface.dataset.art = "webgl";
    return true;
  }

  showFallback() {
    this.canvas.hidden = true;
    this.surface.dataset.art = "fallback";
  }

  resize() {
    if (!this.program || this.destroyed) return;
    const size = this.surface.getBoundingClientRect();
    // Bound the decorative backing store independently of native monitor scale.
    const scale = Math.min(devicePixelRatio || 1, 2);
    this.canvas.width = Math.max(1, Math.round(size.width * scale));
    this.canvas.height = Math.max(1, Math.round(size.height * scale));
    this.refresh();
  }

  /** Draw a given time in seconds; numeric readback is confined to validation. */
  draw(seconds) {
    const gl = this.gl;
    if (!this.program || gl.isContextLost() || this.destroyed) return;
    gl.viewport(0, 0, this.canvas.width, this.canvas.height);
    gl.useProgram(this.program); gl.bindBuffer(gl.ARRAY_BUFFER, this.buffer);
    gl.enableVertexAttribArray(this.position);
    gl.vertexAttribPointer(this.position, 2, gl.FLOAT, false, 0, 0);
    gl.uniform2f(this.uniforms.resolution, this.canvas.width, this.canvas.height);
    gl.uniform1f(this.uniforms.time, seconds);
    gl.uniform1f(this.uniforms.strength, 1);
    gl.uniform1f(this.uniforms.variant, this.variant);
    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  tick(now) {
    this.frame = null;
    if (!this.canAnimate()) return;
    if (this.lastTick === null || now - this.lastTick >= 1000 / 30) {
      if (this.lastTick !== null) this.time += Math.min((now - this.lastTick) / 1000, .1);
      this.lastTick = now;
      this.draw(this.time);
    }
    this.frame = requestAnimationFrame(time => this.tick(time));
  }

  canAnimate() {
    return !!this.program && !this.destroyed && this.visible && !document.hidden && !this.motion.matches;
  }

  refresh() {
    this.stop();
    if (!this.program || !this.visible || document.hidden || this.destroyed) return;
    this.draw(this.motion.matches ? 0 : this.time);
    if (this.canAnimate()) this.frame = requestAnimationFrame(time => this.tick(time));
  }

  stop() {
    if (this.frame !== null) cancelAnimationFrame(this.frame);
    this.frame = null; this.lastTick = null;
  }

  /** Pause when dismissed; resuming continues the last active animation time. */
  setVisible(visible) {
    if (this.visible === visible) return;
    this.visible = visible;
    this.refresh();
  }

  /** Select one of three original studies without reallocating GPU resources. */
  setVariant(variant) {
    if (!Number.isInteger(variant) || variant < 0 || variant > 2) return;
    this.variant = variant;
    this.surface.dataset.artVariant = String(variant);
    this.refresh();
  }

  /** Idempotently release GPU resources and all observers/listeners. */
  destroy() {
    if (this.destroyed) return;
    this.destroyed = true; this.stop(); this.observer.disconnect();
    this.motion.removeEventListener("change", this.onMotion);
    document.removeEventListener("visibilitychange", this.onVisibility);
    this.canvas.removeEventListener("webglcontextlost", this.onLost);
    this.canvas.removeEventListener("webglcontextrestored", this.onRestored);
    if (this.buffer) this.gl.deleteBuffer(this.buffer);
    if (this.program) this.gl.deleteProgram(this.program);
    this.buffer = null; this.program = null; this.showFallback();
  }
}
