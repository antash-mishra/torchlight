/* Canned-data interaction for the Quiet System proposal; never launches files. */
"use strict";
const fixtures = [
  { name: "config.h", path: "~/workspace/torchlight/include/torchlight", kind: "file", label: "header" },
  { name: "config.c", path: "~/workspace/torchlight/src/core", kind: "file", label: "source" },
  { name: "config", path: "~/workspace/dotfiles", kind: "folder", label: "folder" },
  { name: "Configuration Editor", path: "Application · System settings", kind: "app", label: "app" },
  { name: "config.toml", path: "~/Documents/project-kit", kind: "file", label: "toml" },
];
const icons = {
  file: '<path d="M5 3h8l4 4v14H5z"/><path d="M13 3v5h4M8 12h6M8 16h6"/>',
  folder: '<path d="M3 7a2 2 0 0 1 2-2h5l2 2h7a2 2 0 0 1 2 2v10H3z"/>',
  app: '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>',
};
const query = document.querySelector("#query");
const results = document.querySelector("#results");
const message = document.querySelector("#message");
const status = document.querySelector("#status");
const retry = document.querySelector("#retry");
const note = document.querySelector("#action-note");
const launcher = document.querySelector(".launcher");
const searchSurface = document.querySelector(".search-surface");
const caret = document.querySelector("#underline-caret");
const searchArt = SearchArt.create(document.querySelector("#search-art"));
document.querySelectorAll(".art-controls button").forEach((button) => {
  button.addEventListener("click", () => {
    searchArt.setVariant(Number(button.dataset.artVariant));
    document.querySelectorAll(".art-controls button").forEach((choice) => {
      choice.setAttribute("aria-pressed", String(choice === button));
    });
    query.focus();
  });
});
const caretMeasure = document.createElement("span");
caretMeasure.className = "caret-measure";
caretMeasure.setAttribute("aria-hidden", "true");
document.body.append(caretMeasure);
const pathMeasure = caretMeasure.cloneNode();
document.body.append(pathMeasure);
const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");
let composing = false;
let caretFrame = null;
let pathFrame = null;
let hasOpened = false;
let visible = fixtures;
let selected = 0;
let currentState = "idle";
const TYPING_IDLE_MS = 700;
let typingTimer = null;

function stopTypingLight() {
  clearTimeout(typingTimer);
  typingTimer = null;
  searchSurface.classList.remove("is-typing");
}

// Extend a single hold on each edit so fast input never stacks animations.
function showTypingLight() {
  if (document.activeElement !== query || launcher.hidden) return;
  clearTimeout(typingTimer);
  searchSurface.classList.add("is-typing");
  typingTimer = setTimeout(stopTypingLight, TYPING_IDLE_MS);
}

// Mirror the browser's shaped text so moving the cursor within a word also works.
function updateCaret() {
  caretFrame = null;
  const active = document.activeElement === query && !launcher.hidden && !composing;
  const collapsed = query.selectionStart === query.selectionEnd;
  caret.hidden = !active || !collapsed;
  query.classList.toggle("custom-caret", !composing);
  if (caret.hidden) return;
  const style = getComputedStyle(query);
  caretMeasure.style.font = style.font;
  caretMeasure.style.letterSpacing = style.letterSpacing;
  caretMeasure.textContent = query.value || " ";
  const range = document.createRange();
  range.setStart(caretMeasure.firstChild, query.selectionStart || 0);
  range.collapse(true);
  const x = range.getBoundingClientRect().x - caretMeasure.getBoundingClientRect().x - query.scrollLeft;
  const limit = Math.max(0, query.clientWidth - caret.offsetWidth);
  caret.style.left = `${Math.max(0, Math.min(limit, x))}px`;
}

function scheduleCaret() {
  if (caretFrame === null) caretFrame = requestAnimationFrame(updateCaret);
}

// Trim the least useful ancestors first, keeping the home/root and final folders.
function shortenedPath(value, width) {
  const fits = (text) => {
    pathMeasure.textContent = text;
    return pathMeasure.getBoundingClientRect().width <= width;
  };
  if (fits(value)) return value;
  const root = value.startsWith("~/") ? "~/" : value.startsWith("/") ? "/" : "";
  const folders = value.slice(root.length).split("/");
  for (let first = 1; first < folders.length; first++) {
    const candidate = `${root}…/${folders.slice(first).join("/")}`;
    if (fits(candidate)) return candidate;
  }
  // An unusually long single folder still needs its beginning and end visible.
  const prefix = folders.length > 1 ? `${root}…/` : root;
  const letters = Array.from(folders[folders.length - 1]);
  for (let count = letters.length - 1; count > 0; count--) {
    const left = Math.ceil(count / 2), right = Math.floor(count / 2);
    const candidate = prefix + letters.slice(0, left).join("") + "…" + (right ? letters.slice(-right).join("") : "");
    if (fits(candidate)) return candidate;
  }
  return "…";
}

function updatePaths() {
  pathFrame = null;
  if (results.hidden) return;
  results.querySelectorAll(".result-path").forEach((path) => {
    const style = getComputedStyle(path);
    pathMeasure.style.font = style.font;
    pathMeasure.style.letterSpacing = style.letterSpacing;
    const full = path.dataset.fullPath;
    const shortened = shortenedPath(full, path.clientWidth);
    path.textContent = path.closest(".result").dataset.kind === "app" && shortened !== full
      ? shortenedPath(full.replace(/^Application · /, ""), path.clientWidth) : shortened;
  });
}

function schedulePaths() {
  if (pathFrame === null) pathFrame = requestAnimationFrame(updatePaths);
}

// Cancel prior fades so rapid typing cannot queue effects or delay new results.
function animateAppearance(node, opening = false) {
  node.getAnimations().forEach((animation) => animation.cancel());
  if (reducedMotion.matches) return;
  const frames = opening
    ? [{ opacity: 0, transform: "translateY(6px)" }, { opacity: 1, transform: "translateY(0)" }]
    : [{ opacity: .65 }, { opacity: 1 }];
  node.animate(frames, { duration: opening ? 160 : 100, easing: "ease-out" });
}

reducedMotion.addEventListener("change", () => {
  if (reducedMotion.matches) document.getAnimations().forEach((animation) => animation.cancel());
});

function markName(node, value) {
  const term = query.value.trim().toLowerCase();
  const at = term ? value.toLowerCase().indexOf(term) : -1;
  if (at < 0) { node.textContent = value; return; }
  node.append(document.createTextNode(value.slice(0, at)));
  const match = document.createElement("span");
  match.className = "match";
  match.textContent = value.slice(at, at + term.length);
  node.append(match, document.createTextNode(value.slice(at + term.length)));
}

function renderResults() {
  results.replaceChildren();
  visible.forEach((item, index) => {
    const row = document.createElement("div");
    row.className = "result";
    row.dataset.kind = item.kind;
    row.id = `result-${index}`;
    row.setAttribute("role", "option");
    row.setAttribute("aria-selected", String(index === selected));
    const icon = document.createElement("span");
    icon.className = "result-icon";
    icon.setAttribute("aria-hidden", "true");
    icon.innerHTML = `<svg viewBox="0 0 24 24">${icons[item.kind]}</svg>`;
    const copy = document.createElement("div");
    copy.className = "result-copy";
    const name = document.createElement("div");
    name.className = "result-name";
    markName(name, item.name);
    const path = document.createElement("div");
    path.className = "result-path";
    path.textContent = item.path;
    path.dataset.fullPath = item.path;
    path.title = item.path;
    path.setAttribute("aria-label", item.path);
    copy.append(name, path);
    const kind = document.createElement("span");
    kind.className = "result-kind";
    kind.textContent = index === selected ? "open ↵" : item.label;
    row.append(icon, copy, kind);
    row.addEventListener("mousedown", (event) => event.preventDefault());
    row.addEventListener("click", () => { selected = index; updateSelection(); previewAction(false); });
    results.append(row);
  });
  updateSelection();
  schedulePaths();
}

function updateSelection() {
  results.querySelectorAll(".result").forEach((row, index) => {
    row.setAttribute("aria-selected", String(index === selected));
    row.querySelector(".result-kind").textContent = index === selected ? "open ↵" : visible[index].label;
  });
  query.setAttribute("aria-activedescendant", `result-${selected}`);
  schedulePaths();
}

function setMessage(state) {
  const messages = {
    none: ["No matches.", "Try a filename or part of its folder path."],
    offline: ["Search offline.", "Your query is saved. Retry to reconnect."],
  };
  const [title, detail] = messages[state];
  document.querySelector("#message-title").textContent = title;
  message.title = detail;
  message.dataset.state = state;
  retry.hidden = state !== "offline";
  query.removeAttribute("aria-activedescendant");
}

function render(state) {
  const opening = launcher.hidden || !hasOpened;
  currentState = state;
  launcher.hidden = false;
  launcher.dataset.state = state;
  searchArt.setVisible(true);
  hasOpened = true;
  results.hidden = state !== "results";
  message.hidden = state === "results" || state === "idle";
  document.querySelector(".launcher-footer").hidden = state !== "results";
  if (state === "results") renderResults();
  else if (state !== "idle") setMessage(state);
  else query.removeAttribute("aria-activedescendant");
  status.textContent = state === "results" ? `${visible.length} matches` : { idle: "ready", none: "0 matches", offline: "offline" }[state];
  query.setAttribute("aria-expanded", String(state === "results"));
  document.querySelectorAll(".preview-controls button").forEach((button) => {
    button.setAttribute("aria-pressed", String(button.dataset.state === state));
  });
  scheduleCaret();
  if (opening) animateAppearance(launcher, true);
  else if (state !== "idle") animateAppearance(state === "results" ? results : message);
}

function updateQuery() {
  selected = 0;
  note.textContent = "";
  if (!query.value.trim()) { visible = []; render("idle"); return; }
  visible = fixtures.filter((item) => `${item.name} ${item.path}`.toLowerCase().includes(query.value.trim().toLowerCase()));
  render(visible.length ? "results" : "none");
}

function previewAction(reveal) {
  const item = visible[selected];
  if (!item || currentState !== "results") return;
  note.textContent = `Preview only · ${reveal ? "reveal" : "open"} ${item.name}`;
}

query.setAttribute("role", "combobox");
query.setAttribute("aria-controls", "results");
query.addEventListener("input", () => { updateQuery(); showTypingLight(); });
query.addEventListener("blur", stopTypingLight);
for (const event of ["input", "focus", "blur", "keyup", "click", "scroll"]) query.addEventListener(event, scheduleCaret);
document.addEventListener("selectionchange", scheduleCaret);
window.addEventListener("resize", () => { scheduleCaret(); schedulePaths(); });
query.addEventListener("compositionstart", () => { composing = true; scheduleCaret(); });
query.addEventListener("compositionend", () => { composing = false; scheduleCaret(); });
document.fonts.ready.then(() => { scheduleCaret(); schedulePaths(); });
retry.addEventListener("click", () => { updateQuery(); query.focus(); });
document.querySelectorAll(".preview-controls button").forEach((button) => {
  button.addEventListener("click", () => {
    query.value = { results: "config", idle: "", none: "xqzz", offline: "config" }[button.dataset.state];
    visible = fixtures; selected = 0; note.textContent = "";
    render(button.dataset.state); query.focus();
  });
});
query.addEventListener("keydown", (event) => {
  if ((event.key === "ArrowDown" || event.key === "ArrowUp") && currentState === "results") {
    event.preventDefault();
    selected = Math.max(0, Math.min(visible.length - 1, selected + (event.key === "ArrowDown" ? 1 : -1)));
    updateSelection();
  }
  if (event.key === "Enter") {
    event.preventDefault();
    if (currentState === "offline") retry.click(); else previewAction(event.ctrlKey);
  }
  if (event.key === "Escape") {
    stopTypingLight();
    launcher.hidden = true;
    searchArt.setVisible(false);
    caret.hidden = true;
    query.setAttribute("aria-expanded", "false");
    note.textContent = "Preview dismissed · choose a state below to reopen.";
  }
});
render("idle");
query.focus();
query.setSelectionRange(query.value.length, query.value.length);
scheduleCaret();
