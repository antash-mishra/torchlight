## Apps, generated from every app's name

| Launcher | Found first | Top 5 | full name | first letters | one typo |
|---|---|---|---|---|---|
| Torchlight | 92% | 99% | 100% / 100% | 78% / 99% | 99% / 99% |
| GNOME Shell search | 60% | 69% | 98% / 100% | 74% / 99% | 5% / 5% |
| Ulauncher | 86% | 99% | 96% / 100% | 66% / 99% | 98% / 99% |
| rofi | 45% | 63% | 89% / 99% | 39% / 82% | 5% / 5% |
| fzf | 65% | 74% | 99% / 100% | 74% / 99% | 20% / 20% |
261 queries. Per kind: found first / top 5.

## Everyday app queries

| Launcher | Found first | Top 5 |
|---|---|---|
| Torchlight | 94% | 97% |
| GNOME Shell search | 86% | 89% |
| Ulauncher | 81% | 86% |
| rofi | 69% | 86% |
| fzf | 83% | 83% |
36 queries.

## Files in a 50k-path home

| Launcher | Found first | Top 5 | Without skipped folders | exact name | first letters | abbreviation | one typo | partial typo | folder + name |
|---|---|---|---|---|---|---|---|---|---|
| Torchlight | 45% | 58% | 45% / 58% | 96% / 100% | 22% / 42% | 41% / 63% | 73% / 86% | 6% / 7% | 30% / 50% |
| rofi | 24% | 29% | 25% / 29% | 88% / 94% | 20% / 28% | 2% / 4% | 1% / 1% | 0% / 1% | 35% / 47% |
| fzf | 35% | 46% | 35% / 46% | 90% / 94% | 22% / 32% | 32% / 55% | 27% / 34% | 1% / 2% | 36% / 60% |
| plocate | 25% | 31% | 25% / 31% | 86% / 94% | 24% / 34% | 1% / 2% | 1% / 1% | 0% / 0% | 39% / 53% |
1200 queries. Per kind: found first / top 5. Targets in folders Torchlight skips by default (hidden folders, build output, package folders; ADR 0062): 1; the third column leaves them out (found first / top 5).

## Time per query

| Launcher | Section | Its time p50 / p95 (ms) | Torchlight on the same box (ms) |
|---|---|---|---|
| GNOME Shell search | apps | 0.007 / 0.017 | 0.316 / 2.316 |
| GNOME Shell search | everyday | 0.007 / 0.013 | 0.35 / 1.629 |
| Ulauncher | apps | 0.239 / 0.305 | 0.115 / 0.67 |
| Ulauncher | everyday | 0.238 / 0.346 | 0.127 / 0.514 |
| rofi | apps | 16.522 / 24.986 | 0.211 / 1.219 |
| rofi | everyday | 16.065 / 23.307 | 0.211 / 1.089 |
| rofi | files | 33.404 / 47.988 | 0.188 / 1.033 |
| fzf | apps | 1.327 / 4.953 | 0.184 / 1.14 |
| fzf | everyday | 1.3 / 5.157 | 0.278 / 1.16 |
| fzf | files | 15.685 / 50.732 | 0.201 / 1.2 |
| plocate | files | 0.903 / 1.586 | 0.131 / 0.693 |

Torchlight's daemon: 49.7 MiB resident (peak 56.2 MiB) with the 89 apps and 50k paths; ready 1.1 s after starting.

## Versions and boxes

- Torchlight: torchlightd 0.1.6 (3a858d4)
- GNOME Shell search: GLib 2.80.0 (as in GNOME Shell 46) (box gnome, AMD Ryzen 7 9700X 8-Core Processor)
- Ulauncher: Ulauncher 5.16.2 (box ulauncher, AMD Ryzen 9 9950X 16-Core Processor)
- rofi: Version: 1.7.5 (box rofi, AMD Ryzen 7 9700X 8-Core Processor)
- fzf: fzf 0.74.5 (box fzf, AMD Ryzen 9 9950X 16-Core Processor)
- plocate: plocate 1.1.19 (box plocate, AMD Ryzen 9 9950X 16-Core Processor)
