## Apps, generated from every app's name

| Launcher | Found first | Top 5 | full name | first letters | one typo |
|---|---|---|---|---|---|
| Torchlight | 92% | 99% | 100% / 100% | 78% / 99% | 99% / 99% |
| GNOME Shell search | 58% | 69% | 98% / 100% | 68% / 99% | 5% / 5% |
| Ulauncher | 89% | 99% | 97% / 100% | 72% / 99% | 98% / 99% |
| rofi | 45% | 63% | 89% / 99% | 39% / 82% | 5% / 5% |
| fzf | 65% | 74% | 99% / 100% | 74% / 99% | 20% / 20% |
261 queries. Per kind: found first / top 5.

## Everyday app queries

| Launcher | Found first | Top 5 |
|---|---|---|
| Torchlight | 94% | 97% |
| GNOME Shell search | 83% | 89% |
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
| GNOME Shell search | apps | 0.009 / 0.017 | 0.234 / 0.886 |
| GNOME Shell search | everyday | 0.008 / 0.014 | 0.24 / 0.712 |
| Ulauncher | apps | 0.436 / 0.542 | 0.247 / 0.902 |
| Ulauncher | everyday | 0.442 / 0.68 | 0.237 / 0.764 |
| rofi | apps | 62.917 / 73.973 | 0.236 / 0.866 |
| rofi | everyday | 64.71 / 76.796 | 0.262 / 0.709 |
| rofi | files | 88.016 / 100.361 | 0.233 / 0.889 |
| fzf | apps | 1.974 / 2.528 | 0.229 / 0.895 |
| fzf | everyday | 1.88 / 2.243 | 0.244 / 0.721 |
| fzf | files | 17.035 / 54.614 | 0.233 / 0.876 |
| plocate | files | 2.033 / 3.831 | 0.219 / 0.842 |

Torchlight's daemon: 40.9 MiB resident (peak 43.8 MiB) with the 89 apps and 50k paths; ready 0.4 s after starting.

## Versions and boxes

- Torchlight: torchlightd 0.1.6 (46216b0)
- GNOME Shell search: GLib 2.80.0 (as in GNOME Shell 46) (box gnome, Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz)
- Ulauncher: Ulauncher 5.16.2 (box ulauncher, Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz)
- rofi: Version: 1.7.5 (box rofi, Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz)
- fzf: fzf 0.74.5 (box fzf, Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz)
- plocate: plocate 1.1.19 (box plocate, Intel(R) Core(TM) i7-8700K CPU @ 3.70GHz)
