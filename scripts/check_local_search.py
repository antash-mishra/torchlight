#!/usr/bin/env python3
"""Check real filename searches locally; report aggregates without private names.

Reads paths/stat metadata only, creates an isolated temporary daemon/cache, and
never modifies the indexed folder. These automatically derived exact-name checks
are a smoke test, not a human-labeled semantic relevance evaluation.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
from evaluate_semantic_daemon import query


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True)
    parser.add_argument('--model', required=True)
    parser.add_argument('--daemon', default='build/torchlightd-release')
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    root = Path(args.root).resolve()
    found = subprocess.run(['rg', '--files', '--hidden', '--no-ignore', '-0', str(root)], capture_output=True, check=False)
    assert found.returncode in (0, 1), 'cannot enumerate root'
    paths = [p.decode('utf-8', 'surrogateescape') for p in found.stdout.split(b'\0') if p]
    # Match the crawler's default policy: dot directories are not traversed.
    eligible = [p for p in paths if not any(part.startswith('.') for part in Path(p).relative_to(root).parts[:-1])]
    names = list(dict.fromkeys(Path(p).name for p in eligible if len(Path(p).name.encode('utf-8', 'surrogateescape')) <= 256))[:128]
    assert names, 'no eligible filenames to test'
    with tempfile.TemporaryDirectory(prefix='torchlight-private-search-') as temporary:
        base = Path(temporary)
        config = base / 'config'
        config.write_text(f'root = {root}\n')
        path = base / 'service.sock'
        log = tempfile.TemporaryFile()
        process = subprocess.Popen([str(Path(args.daemon).resolve()), '--db', str(base / 'db'),
            '--config', str(config), '--socket', str(path), '--model', str(Path(args.model).resolve()),
            '--no-history', '--rescan-ms', '3600000'], stdout=log, stderr=log,
            env=dict(os.environ, HOME=str(base), XDG_DATA_HOME=str(base / 'data'),
                     XDG_DATA_DIRS=str(base / 'none'), XDG_CONFIG_HOME=str(base / 'cfg')))
        try:
            sequence = 0
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                sequence += 1
                try:
                    phases, times = query(path, sequence, names[0])
                    if len(phases) == 2:
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.025)
            else:
                raise TimeoutError('semantic publication')
            lexical, final, correct, coherent = [], [], 0, 0
            for name in names:
                sequence += 1
                phases, times = query(path, sequence, name)
                lexical.append(times[0])
                final.append(times[-1])
                results = phases[-1]['results']
                correct += bool(results and Path(results[0].get('path', '')).name == name)
                coherent += len(phases) == 2 and all(phases[0][f] == phases[1][f] for f in ('request_id', 'search_id', 'catalog_gen', 'emb_gen'))
            def p95(values):
                return sorted(values)[min(len(values) - 1, (len(values) * 95 + 99) // 100 - 1)]
            report = dict(files=len(paths), eligible_files=len(eligible), skipped_hidden_directory_files=len(paths)-len(eligible), exact_name_queries=len(names), exact_top1=correct,
                coherent_two_phase=coherent, lexical_p95_ms=p95(lexical), final_p95_ms=p95(final),
                contents_read=False, private_names_recorded=False,
                limits='User Documents; automatically generated exact-name checks, not labeled semantic relevance.')
            Path(args.output).write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps(report))
        finally:
            process.terminate()
            process.wait(timeout=10)
            log.seek(0)
            assert process.returncode == 0, log.read().decode()
            log.close()


if __name__ == '__main__':
    main()
