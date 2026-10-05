#!/usr/bin/env python3
"""Run the labeled English fixture through the actual native two-phase daemon.

Requires an exported local model and release daemon. Creates isolated file and
XDG application catalogs; no user's files, daemon, applications or DB are touched.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from evaluate_semantic import ROOT, quality


def query(path, sequence, text):
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(str(path))
        request = dict(version=1, request_id=str(sequence), op='query', query=text, limit=10)
        start = time.perf_counter()
        connection.sendall(json.dumps(request).encode() + b'\n')
        stream = connection.makefile('rb')
        responses, times = [], []
        while True:
            response = json.loads(stream.readline())
            responses.append(response)
            times.append((time.perf_counter() - start) * 1000)
            if response['phase'] == 'final':
                return responses, times


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--daemon', default='build/torchlightd-release')
    parser.add_argument('--model', required=True)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    fixture = json.loads((ROOT / 'tests/quality/semantic.json').read_text())
    with tempfile.TemporaryDirectory(prefix='torchlight-model-evaluation-') as temporary:
        base = Path(temporary)
        root = base / 'home/demo'
        apps = base / 'xdg/applications'
        apps.mkdir(parents=True)
        keys = {}
        for entry in fixture['entries']:
            if entry['kind'] == 'app':
                path = apps / (entry['key'] + '.desktop')
                path.write_text('[Desktop Entry]\nType=Application\nExec=/bin/true\n' +
                    'Name=' + entry['path'].rsplit('/', 1)[1] + '\n' +
                    'GenericName=' + entry.get('generic', '') + '\n' +
                    'Keywords=' + entry.get('keywords', '') + '\n')
            else:
                path = root / entry['path'].removeprefix('/home/demo/')
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b'')
            keys[str(path)] = entry['key']
        config = base / 'config'
        config.write_text(f'root = {root}\n')
        path = base / 'daemon.sock'
        env = dict(os.environ, HOME=str(base), XDG_DATA_HOME=str(base / 'xdg'),
                   XDG_DATA_DIRS=str(base / 'none'), XDG_CONFIG_HOME=str(base / 'cfg'))
        log = tempfile.TemporaryFile()
        process = subprocess.Popen([str(Path(args.daemon).resolve()), '--db', str(base / 'db'),
            '--config', str(config), '--socket', str(path), '--model', str(Path(args.model).resolve()),
            '--no-history', '--rescan-ms', '3600000'], stdout=log, stderr=log, env=env)
        sequence = 0
        try:
            start = time.monotonic()
            deadline = start + 30
            while time.monotonic() < deadline:
                sequence += 1
                if process.poll() is not None:
                    log.seek(0)
                    raise RuntimeError(log.read().decode())
                try:
                    phases, times = query(path, sequence, 'household budget')
                    if len(phases) == 2:
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(.025)
            else:
                raise TimeoutError('semantic catalog publication')
            ready_ms = (time.monotonic() - start) * 1000
            rows, initial, final = [], [], []
            for item in fixture['queries']:
                sequence += 1
                phases, times = query(path, sequence, item['text'])
                assert len(phases) == 2, phases
                for field in ('request_id', 'search_id', 'catalog_gen', 'emb_gen'):
                    assert phases[0][field] == phases[1][field], phases
                results = [keys.get(row.get('path', ''), '<unlabeled-directory>') for row in phases[-1]['results']]
                lexical = [keys.get(row.get('path', ''), '<unlabeled-directory>') for row in phases[0]['results']]
                rows.append({**item, 'top10': results, 'lexical_top10': lexical,
                             'metrics': quality.metrics(results, item['labels']),
                             'lexical_metrics': quality.metrics(lexical, item['labels']),
                             'phase_ms': times, 'reason': phases[-1]['reason']})
                initial.append(times[0])
                final.append(times[-1])
            memory = {}
            for line in Path(f'/proc/{process.pid}/status').read_text().splitlines():
                if line.startswith(('VmRSS:', 'VmHWM:')):
                    key, value, _ = line.split()
                    memory[key[:-1]] = int(value)
            aggregate = {}
            for split in ('tuning', 'heldout'):
                for kind in ('all', 'semantic', 'name', 'no_match'):
                    chosen = [r for r in rows if r['split'] == split and (kind == 'all' or r['class'] == kind)]
                    if not chosen:
                        continue
                    for variant in ('metrics', 'lexical_metrics'):
                        measures = set().union(*(r[variant].keys() for r in chosen))
                        aggregate[f'{split}/{kind}/{variant}'] = {key: sum(r[variant][key] for r in chosen if key in r[variant]) / sum(key in r[variant] for r in chosen) for key in measures}
            def percentiles(values):
                values = sorted(values)
                return {str(p): values[min(len(values) - 1, (len(values) * p + 99) // 100 - 1)] for p in (50, 95, 99)}
            report = dict(queries=rows, aggregate=aggregate, ready_ms=ready_ms,
                          lexical_ms=percentiles(initial), final_ms=percentiles(final), memory_kib=memory,
                          model_sha256=__import__('hashlib').sha256(Path(args.model).read_bytes()).hexdigest(),
                          scope='Native end-to-end small fixture; directories included, no file contents.')
            Path(args.output).write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps({key: value for key, value in report.items() if key != 'queries'}))
        finally:
            process.terminate()
            process.wait(timeout=10)
            log.seek(0)
            assert process.returncode == 0, log.read().decode()
            log.close()


if __name__ == '__main__':
    main()
