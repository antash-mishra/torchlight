/* Query-independent place prior for file ranking: a small penalty from where
 * an entry lives (its depth below an indexed root, and whether it lies inside
 * a dependency or toolchain tree), computed per directory from normalized
 * names alone. Pure functions: no allocation, I/O or errors. */
#ifndef TORCHLIGHT_PLACE_H
#define TORCHLIGHT_PLACE_H
#include "torchlight/tokenize.h"
#include <stdbool.h>
#include <stdint.h>
/* Entries up to this many directories below an indexed root cost nothing:
 * ~/workspace/project/src/file is as easy to mean as ~/notes.txt. */
#define PLACE_FREE_DEPTH 3
/* Each directory beyond PLACE_FREE_DEPTH: enough that three extra levels
 * outweigh the whole short-name bonus (64) between equal matches. */
#define PLACE_DEPTH_PENALTY 24
/* Inside a dependency or toolchain tree (see place_dependency_name). */
#define PLACE_DEPENDENCY_PENALTY 128
/* Upper bound of place_penalty. It stays below the smallest gap between
 * lexical channel tiers (initials 4500 to token prefix 5000), so a place
 * reorders matches of similar strength and never a stronger kind of match
 * below a weaker one. */
#define PLACE_PENALTY_MAX 256
/** Where a directory lies, as far as ranking cares. */
typedef struct {
    /* Usable directories on the path: the indexed root's own folder is 1,
     * directories above every indexed root are 0. */
    uint32_t level;
    /* The directory is a dependency marker or lies below one, so every
     * entry inside it is part of a dependency tree. */
    bool dependency;
} tl_place;
/** Whether a normalized (case-folded) directory name marks a dependency or
 * toolchain tree: a package folder (node_modules, bower_components,
 * site-packages, dist-packages), vendored code (vendor, third_party,
 * third-party, thirdparty), a compiler sysroot (sysroot), an SDK (sdk), a Go
 * module or module-cache folder (contains "@v" followed by a digit, or is
 * exactly "@v"), or a versioned install folder (an optional "v", one to three
 * digits, then one or more ".digits" groups, such as 27.0.12077973; four-digit
 * years such as 2024.05.01 do not count). Borrowed name; no errors. */
bool place_dependency_name(tl_text name);
/** Place of a directory named name (normalized) directly below parent.
 * usable is false for directories above every indexed root: they neither add
 * a level nor mark a dependency tree, as they say nothing about where a file
 * is. The tree root ("/") has place {0, false}. No errors. */
tl_place place_child(tl_place parent, tl_text name, bool usable);
/** Penalty in 0..PLACE_PENALTY_MAX for an entry whose parent directory has
 * place parent: PLACE_DEPTH_PENALTY per directory between the indexed root
 * and the entry beyond PLACE_FREE_DEPTH, plus PLACE_DEPENDENCY_PENALTY inside
 * a dependency tree, capped. No errors. */
int place_penalty(tl_place parent);
#endif
