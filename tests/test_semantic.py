"""M4 two-phase lifecycle using an independent analytic static model, no download."""
import hashlib
import itertools
import json
import os
from pathlib import Path
import signal
import socket
import sqlite3
import struct
import subprocess
import sys
import tempfile
import time

BINARY = str(Path(sys.argv[1]).resolve())
SEQUENCE = itertools.count()
STALL = str(Path(sys.argv[2]).resolve()) if len(sys.argv) > 2 else None
CLI = str(Path(sys.argv[3]).resolve()) if len(sys.argv) > 3 else None


def model_bytes(replace=False):
    words = ['[PAD]', '[UNK]', '[CLS]', '[SEP]', '[MASK]', 'invoice', 'bill', 'tax',
             'return', 'browser', 'surf', 'pdf', 'finance', 'receipt']
    table = [[0., 0., 0., 0.] for _ in words]
    for index in (5, 6, 13):
        table[index][1 if replace and index == 5 else 0] = 1
    for index in (7, 8):
        table[index][1] = 1
    for index in (9, 10):
        table[index][2] = 1
    vocabulary = b''
    offsets = []
    for word in words:
        offsets.append(len(vocabulary))
        vocabulary += word.encode() + b'\0'
    offsets.append(len(vocabulary))
    body = struct.pack(f'<{len(offsets)}I', *offsets) + vocabulary
    body += struct.pack(f'<{len(words) * 4}f', *sum(table, []))
    return b'TLSTAT01' + struct.pack('<III', len(words), 4, len(vocabulary)) + b'0' * 168 + hashlib.sha256(body).digest() + body


def wait(predicate, timeout=15):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        last = predicate()
        if last:
            return last
        time.sleep(.025)
    raise AssertionError(f'timed out: {last!r}')


class Service:
    def __init__(self, base, deadline=200):
        self.base = base
        self.root = base / 'root'
        self.root.mkdir(exist_ok=True)
        self.apps = base / 'data/applications'
        self.apps.mkdir(parents=True, exist_ok=True)
        self.config = base / 'config'
        self.config.write_text(f'root = {self.root}\n')
        self.path = base / 'service.sock'
        self.database = base / 'catalog.db'
        self.model = base / 'model.tlm'
        self.model.write_bytes(model_bytes())
        self.deadline = deadline
        self.marker = base / 'stall-marker'
        self.process = None

    def start(self):
        env = dict(os.environ, HOME=str(self.base), XDG_DATA_HOME=str(self.base / 'data'),
                   XDG_DATA_DIRS=str(self.base / 'none'), XDG_CONFIG_HOME=str(self.base / 'cfg'),
                   XDG_CURRENT_DESKTOP='TorchlightTest')
        if STALL:
            preload = [STALL]
            if 'sanitized' in BINARY:
                preload.insert(0, subprocess.check_output(['cc', '-print-file-name=libasan.so'], text=True).strip())
            env.update(LD_PRELOAD=':'.join(preload), TORCHLIGHT_TEST_SEMANTIC_STALL=str(self.marker))
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([BINARY, '--config', str(self.config), '--db', str(self.database),
            '--socket', str(self.path), '--model', str(self.model), '--rescan-ms', '100',
            '--semantic-deadline-ms', str(self.deadline)], env=env, stdout=self.log, stderr=self.log)
        def ready():
            if self.process.poll() is not None:
                self.log.seek(0)
                raise AssertionError(self.log.read().decode())
            try:
                return self.query('status', operation='status')
            except (OSError, AssertionError):
                return None
        wait(ready)

    def stop(self):
        if self.process is None:
            return
        self.process.send_signal(signal.SIGTERM)
        self.process.wait(timeout=10)
        self.log.seek(0)
        log = self.log.read().decode()
        assert self.process.returncode == 0, log
        assert 'AddressSanitizer' not in log and 'runtime error:' not in log, log
        self.log.close()
        self.process = None

    def query(self, text, limit=10, operation='query'):
        request = dict(version=1, request_id=str(next(SEQUENCE)), op=operation)
        if operation == 'query':
            request.update(query=text, limit=limit)
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(5)
            connection.connect(str(self.path))
            connection.sendall(json.dumps(request).encode() + b'\n')
            stream = connection.makefile('rb')
            phases = []
            while True:
                line = stream.readline()
                assert line, phases
                phases.append(json.loads(line))
                if phases[-1]['phase'] == 'final':
                    return phases

    def hybrid(self, text):
        def ready():
            phases = self.query(text)
            return phases if len(phases) == 2 and phases[-1]['reason'] == 'hybrid' else None
        phases = wait(ready)
        assert phases[0]['phase'] == 'lexical'
        for key in ('request_id', 'search_id', 'catalog_gen', 'emb_gen'):
            assert phases[0][key] == phases[1][key], phases
        return phases


with tempfile.TemporaryDirectory(prefix='torchlight-semantic-') as temp:
    service = Service(Path(temp))
    invoice = service.root / 'invoice.pdf'
    invoice.write_text('content deliberately not embedded')
    app = service.apps / 'browser.desktop'
    app.write_text('[Desktop Entry]\nType=Application\nName=Browser\nExec=/bin/true\nGenericName=Web Browser\nKeywords=Internet;Web;\n')
    try:
        service.start()
        phases = service.hybrid('bill')
        assert phases[0]['results'] == [], phases
        assert phases[1]['results'][0]['path'] == str(invoice), phases
        first_gen = phases[1]['emb_gen']
        if CLI:
            completed = subprocess.run([CLI, 'query', '--socket', str(service.path), '--json', 'bill'],
                                       capture_output=True, text=True, timeout=5)
            assert completed.returncode == 0, completed.stderr
            terminal = json.loads(completed.stdout)
            assert terminal['phase'] == 'final' and terminal['results'][0]['path'] == str(invoice), terminal
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(3)
            connection.connect(str(service.path))
            connection.sendall(b'{"version":1,"request_id":"half-close","op":"query","query":"bill","limit":10}\n')
            connection.shutdown(socket.SHUT_WR)
            stream = connection.makefile('rb')
            assert json.loads(stream.readline())['phase'] == 'lexical'
            assert json.loads(stream.readline())['phase'] == 'final'
        assert service.hybrid('surf')[1]['results'][0]['kind'] == 'application'
        assert service.hybrid('invoice.pdf')[1]['results'][0]['path'] == str(invoice)
        # A held external cache write stalls background work, never lexical IPC.
        # Force a rebuild while the same catalog/model snapshot is still usable.
        with sqlite3.connect(service.database, timeout=1) as blocker:
            blocker.execute('BEGIN IMMEDIATE')
            replacement = service.model.with_suffix('.new')
            replacement.write_bytes(model_bytes())
            replacement.replace(service.model)
            time.sleep(.08)
            start = time.monotonic()
            stalled = service.query('bill')
            assert len(stalled) == 2 and stalled[-1]['reason'] == 'semantic_deadline', stalled
            assert stalled[0]['emb_gen'] == stalled[1]['emb_gen'] == first_gen
            assert time.monotonic() - start < 1
            # New requests cancel obsolete jobs even while the worker is blocked.
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(2)
                connection.connect(str(service.path))
                stream = connection.makefile('rb')
                first = dict(version=1, request_id='cancel-1', op='query', query='bill', limit=10)
                connection.sendall(json.dumps(first).encode() + b'\n')
                assert json.loads(stream.readline())['phase'] == 'lexical'
                second = dict(first, request_id='cancel-2', query='tax')
                connection.sendall(json.dumps(second).encode() + b'\n')
                cancelled = json.loads(stream.readline())
                assert cancelled['request_id'] == 'cancel-1' and cancelled['status'] == 'cancelled', cancelled
                while json.loads(stream.readline())['phase'] != 'final':
                    pass
            blocker.rollback()
        service.hybrid('bill')
        if STALL:
            service.marker.touch()
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(3)
                connection.connect(str(service.path))
                stream = connection.makefile('rb')
                first = dict(version=1, request_id='running-1', op='query', query='bill', limit=10)
                connection.sendall(json.dumps(first).encode() + b'\n')
                assert json.loads(stream.readline())['phase'] == 'lexical'
                wait(lambda: not service.marker.exists())  # Inference entered the injected stall.
                second = dict(first, request_id='running-2', query='tax')
                connection.sendall(json.dumps(second).encode() + b'\n')
                cancelled = json.loads(stream.readline())
                assert cancelled['request_id'] == 'running-1' and cancelled['status'] == 'cancelled', cancelled
                saturated = json.loads(stream.readline())
                assert saturated['request_id'] == 'running-2' and saturated['reason'] == 'semantic_queue_full', saturated
                time.sleep(.35)
                connection.sendall(json.dumps(dict(first, request_id='running-3')).encode() + b'\n')
                assert json.loads(stream.readline())['phase'] == 'lexical'
                final = json.loads(stream.readline())
                assert final['phase'] == 'final' and final['request_id'] == 'running-3', final
        with sqlite3.connect(service.database) as db:
            assert db.execute('SELECT count(*) FROM embedding_models WHERE active=1').fetchone()[0] == 1
            assert db.execute('SELECT count(*) FROM embedding_cache').fetchone()[0] == 2
        service.stop()
        service.start()
        assert service.hybrid('bill')[1]['emb_gen'] == first_gen
        # Changed text is embedded; old text is pruned after complete publication.
        tax = invoice.with_name('tax_return.pdf')
        invoice.rename(tax)
        phases = service.hybrid('tax')
        assert phases[1]['results'][0]['path'] == str(tax)
        wait(lambda: all(row.get('path') != str(invoice) for row in service.query('bill')[-1]['results']))
        app.unlink()
        wait(lambda: not any(row['kind'] == 'application' for row in service.query('surf')[-1]['results']))
        # Replacement changes generation, preserves coherent phases and rebuilds cache.
        invoice.write_text('new incarnation')
        replacement = service.model.with_suffix('.new')
        replacement.write_bytes(model_bytes(True))
        replacement.replace(service.model)
        wait(lambda: any(row['path'] == str(invoice) for row in service.hybrid('tax')[-1]['results']))
        assert service.hybrid('tax')[-1]['emb_gen'] != first_gen
        # A missing model still serves a terminal lexical response after restart.
        service.stop()
        service.model.unlink()
        service.start()
        phases = service.query('invoice.pdf')
        assert len(phases) == 1 and phases[0]['phase'] == 'final'
        assert phases[0]['reason'] == 'semantic_unavailable'
        assert phases[0]['results'][0]['path'] == str(invoice)
    finally:
        service.stop()
print('Semantic daemon lifecycle checks passed.')
