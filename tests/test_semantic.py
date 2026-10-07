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


def decode_response(line):
    assert line and line.endswith(b'\n'), 'incomplete response frame'
    response = json.loads(line)
    for field in ('indexing', 'history', 'semantic'):
        assert field in response, f'{response["phase"]} response omitted {field}'
    return response


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
        self.busy_marker = base / 'busy-marker'
        self.lexical_marker = base / 'lexical-marker'
        self.process = None

    def start(self):
        env = dict(os.environ, HOME=str(self.base), XDG_DATA_HOME=str(self.base / 'data'),
                   XDG_DATA_DIRS=str(self.base / 'none'), XDG_CONFIG_HOME=str(self.base / 'cfg'),
                   XDG_CURRENT_DESKTOP='TorchlightTest')
        if STALL:
            preload = [STALL]
            if 'sanitized' in BINARY:
                preload.insert(0, subprocess.check_output(['cc', '-print-file-name=libasan.so'], text=True).strip())
            env.update(LD_PRELOAD=':'.join(preload), TORCHLIGHT_TEST_SEMANTIC_STALL=str(self.marker),
                       TORCHLIGHT_TEST_SEMANTIC_BUSY=str(self.busy_marker),
                       TORCHLIGHT_TEST_LEXICAL_STALL=str(self.lexical_marker))
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

    def stop(self, timeout=10):
        if self.process is None:
            return
        self.process.send_signal(signal.SIGTERM)
        self.process.wait(timeout=timeout)
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
                phases.append(decode_response(line))
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


def large_exchange(service, expected, slow):
    response_limit = 1024 * 1024
    request = dict(version=1, request_id=str(next(SEQUENCE)), op='query',
                   query='invoice', limit=len(expected))
    with socket.socket(socket.AF_UNIX) as connection:
        connection.settimeout(5)
        connection.connect(str(service.path))
        connection.sendall(json.dumps(request).encode() + b'\n')
        if slow:
            # Force the first frame to remain queued while inference completes.
            time.sleep(.25)
        stream = connection.makefile('rb')
        lines = [stream.readline(), stream.readline()]
        phases = [decode_response(line) for line in lines]
        assert [(p['phase'], p['reason']) for p in phases] == [
            ('lexical', 'semantic_pending'), ('final', 'hybrid')], phases
        assert all(response_limit // 2 < len(line) < response_limit for line in lines)
        for field in ('request_id', 'search_id', 'catalog_gen', 'emb_gen'):
            assert phases[0][field] == phases[1][field], field
        for phase in phases:
            assert len(phase['results']) == len(expected)
            assert {row['path'] for row in phase['results']} == expected


def check_large_responses(base):
    service = Service(base, deadline=1000)
    parent = service.root / ('b' * 125)
    parent.mkdir()
    expected = set()
    for index in range(1000):
        path = parent / ('invoice-' + 'a' * 145 + f'-{index:04d}.pdf')
        path.touch()
        expected.add(str(path))
    try:
        service.start()
        service.hybrid('invoice')
        large_exchange(service, expected, slow=False)
        large_exchange(service, expected, slow=True)
        if CLI:
            completed = subprocess.run([CLI, 'query', '--socket', str(service.path),
                '--json', '--limit', '1000', 'invoice'], capture_output=True, timeout=5)
            assert completed.returncode == 0, completed.stderr
            final = decode_response(completed.stdout)
            assert final['phase'] == 'final' and len(final['results']) == len(expected)
            assert {row['path'] for row in final['results']} == expected
    finally:
        service.stop()


def check_degraded_status(base):
    service = Service(base)
    (service.root / 'invoice.pdf').touch()
    with service.config.open('a') as config:
        config.write(f'root = {base / "offline-root"}\n')
    try:
        service.start()
        wait(lambda: service.query('status', operation='status')[0]['indexing']['degraded'])
        for text in ('bill', 'tax'):
            phases = service.hybrid('bill') if text == 'bill' else service.query(text)
            assert len(phases) == 2, phases
            for phase in phases:
                assert phase['indexing']['degraded'] and phase['indexing']['offline_roots'] == 1
    finally:
        service.stop()


def check_phase_order(base):
    """A final never overtakes its lexical frame, even when the semantic worker
    finishes while the search thread is still encoding that frame, and a newer
    query's cancellation of it waits for the frame too."""
    if not STALL:
        return
    service = Service(base, deadline=1000)  # longer than the encoding stall
    (service.root / 'ordering-stall-invoice.pdf').touch()
    (service.root / 'receipt.pdf').touch()
    try:
        service.start()
        service.hybrid('invoice')
        service.lexical_marker.touch()
        phases = service.query('invoice')
        assert not service.lexical_marker.exists()  # the encoding stall fired
        assert [(p['phase'], p['reason']) for p in phases] == [
            ('lexical', 'semantic_pending'), ('final', 'hybrid')], phases
        # A newer query on the same connection while the first frame is stalled.
        service.lexical_marker.touch()
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(5)
            connection.connect(str(service.path))
            ids = [str(next(SEQUENCE)) for _ in range(2)]
            for request_id in ids:
                connection.sendall(json.dumps(dict(version=1, request_id=request_id, op='query',
                                                   query='invoice', limit=10)).encode() + b'\n')
                time.sleep(.1)
            stream = connection.makefile('rb')
            frames = []
            while not frames or (frames[-1]['request_id'], frames[-1]['phase']) != (ids[1], 'final'):
                frames.append(decode_response(stream.readline()))
        assert not service.lexical_marker.exists()
        first = [frame['phase'] for frame in frames if frame['request_id'] == ids[0]]
        assert first[0] == 'lexical' and first[-1] == 'final', frames
        assert frames.index(next(f for f in frames if f['request_id'] == ids[1])) > 1, frames
    finally:
        service.stop()


def check_cache_contention(base):
    if not STALL:
        return
    service = Service(base)
    for index in range(10000):
        (service.root / f'invoice-{index:05d}.pdf').touch()
    try:
        service.start()
        def partial():
            status = service.query('status', operation='status')[0]
            semantic = status['semantic']
            return status if (semantic['total'] >= 10000 and
                              0 < semantic['processed'] < semantic['total'] and
                              semantic['building']) else None
        before = wait(partial)
        service.busy_marker.touch()
        def blocked():
            status = service.query('status', operation='status')[0]
            return status if status['semantic']['last_error'] == 'I/O failure' else None
        failed = wait(blocked)
        assert failed['catalog_gen'] == before['catalog_gen']
        assert failed['semantic']['building'], failed
        assert failed['semantic']['total'] == before['semantic']['total'], failed
        assert failed['semantic']['processed'] >= before['semantic']['processed'], failed
        time.sleep(.15)  # More than one retry must preserve the same progress.
        retried = service.query('status', operation='status')[0]['semantic']
        assert retried['building'] and retried['processed'] == failed['semantic']['processed'], retried
        assert service.query('invoice')[0]['results']
        service.busy_marker.unlink()
        service.hybrid('bill')
    finally:
        service.busy_marker.unlink(missing_ok=True)
        service.stop()


def check_fifo_model(base):
    service = Service(base)
    invoice = service.root / 'invoice.pdf'
    invoice.touch()
    service.model.unlink()
    os.mkfifo(service.model, 0o600)
    try:
        service.start()
        wait(lambda: service.query('status', operation='status')[0]['semantic']['last_error']
             == 'I/O failure')
        def indexed():
            phases = service.query('invoice.pdf')
            return phases if phases[-1]['results'] else None
        phases = wait(indexed)
        assert len(phases) == 1 and phases[0]['reason'] == 'semantic_unavailable', phases
        assert phases[0]['results'][0]['path'] == str(invoice), phases
        # No producer ever opens the FIFO; SIGTERM must still join the worker.
        service.stop(timeout=3)
    finally:
        if service.process is not None:
            if service.process.poll() is None:
                service.process.kill()
                service.process.wait(timeout=5)
            service.log.close()


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
            assert decode_response(stream.readline())['phase'] == 'lexical'
            assert decode_response(stream.readline())['phase'] == 'final'
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
                assert decode_response(stream.readline())['phase'] == 'lexical'
                second = dict(first, request_id='cancel-2', query='tax')
                connection.sendall(json.dumps(second).encode() + b'\n')
                cancelled = decode_response(stream.readline())
                assert cancelled['request_id'] == 'cancel-1' and cancelled['status'] == 'cancelled', cancelled
                while decode_response(stream.readline())['phase'] != 'final':
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
                assert decode_response(stream.readline())['phase'] == 'lexical'
                wait(lambda: not service.marker.exists())  # Inference entered the injected stall.
                second = dict(first, request_id='running-2', query='tax')
                connection.sendall(json.dumps(second).encode() + b'\n')
                cancelled = decode_response(stream.readline())
                assert cancelled['request_id'] == 'running-1' and cancelled['status'] == 'cancelled', cancelled
                saturated = decode_response(stream.readline())
                assert saturated['request_id'] == 'running-2' and saturated['reason'] == 'semantic_queue_full', saturated
                time.sleep(.35)
                connection.sendall(json.dumps(dict(first, request_id='running-3')).encode() + b'\n')
                assert decode_response(stream.readline())['phase'] == 'lexical'
                final = decode_response(stream.readline())
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
for check in (check_large_responses, check_phase_order, check_degraded_status, check_cache_contention,
              check_fifo_model):
    with tempfile.TemporaryDirectory(prefix='torchlight-semantic-regression-') as temp:
        check(Path(temp))
print('Semantic daemon lifecycle checks passed.')
