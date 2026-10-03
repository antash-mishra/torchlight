"""Explicit native Cinnamon acceptance: open panels and close only new test windows.

This is separate from make test/test-ui because it launches real settings panels.
"""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
xdotool = os.environ.get('TORCHLIGHT_XDOTOOL', 'xdotool')

def xdo(*args, check=True):
    return subprocess.run([xdotool, *map(str, args)], capture_output=True, text=True,
                          check=check).stdout.strip()

def wait_for(predicate, timeout=15):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(.04)
    raise AssertionError('timed out')

def windows():
    return set(xdo('search', '--onlyvisible', '--name', '.*', check=False).splitlines())

with tempfile.TemporaryDirectory(prefix='torchlight-native-') as directory:
    base = Path(directory)
    (base/'files').mkdir()
    raw_path = os.fsencode(base/'files') + b"/fixture-target-\xff\nquote'$.txt"
    descriptor = os.open(raw_path, os.O_WRONLY | os.O_CREAT, 0o600)
    os.close(descriptor)
    (base/'config').write_text(f'root = {base}/files\n')
    path = base/'socket'
    log = tempfile.TemporaryFile()
    daemon = subprocess.Popen([str(root/'build/torchlightd'), '--config', str(base/'config'),
        '--db', str(base/'catalog.db'), '--socket', str(path)], stdout=log, stderr=log)
    gui = None
    def query(text):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(5)
            connection.connect(str(path))
            connection.sendall(json.dumps(dict(version=1,request_id='probe',op='query',query=text,limit=10)).encode()+b'\n')
            with connection.makefile('rb') as stream:
                return json.loads(stream.readline())['results']
    try:
        wait_for(lambda:path.exists())
        for text, desktop_id in [('display','cinnamon-display-panel.desktop'),
                                 ('screen','cinnamon-display-panel.desktop'),
                                 ('resolution','cinnamon-display-panel.desktop'),
                                 ('sound','cinnamon-settings-sound.desktop'),
                                 ('keyboard','cinnamon-settings-keyboard.desktop')]:
            rows=query(text)
            assert any(row.get('desktop_id')==desktop_id for row in rows), (text,rows)
            print(text, 'finds', desktop_id, flush=True)
        gui = subprocess.Popen([str(root/'build/torchlight-gtk'),'--socket',str(path),'--toggle'],stdout=log,stderr=log)
        for text, expected in [('display','cinnamon-display-panel.desktop'),
                               ('sound','cinnamon-settings-sound.desktop'),
                               ('keyboard','cinnamon-settings-keyboard.desktop')]:
            if text!='display':
                subprocess.run([str(root/'build/torchlight-gtk'),'--toggle'],check=True,stdout=log,stderr=log)
            wait_for(lambda:xdo('getwindowname',xdo('getactivewindow'))=='Torchlight')
            xdo('key','ctrl+a')
            xdo('type','--clearmodifiers','--delay','1',text)
            time.sleep(.3)
            rows=query(text)
            selected=next(i for i,row in enumerate(rows) if row.get('desktop_id')==expected)
            for _ in range(selected):
                xdo('key','Down')
            before=windows()
            xdo('key','Return')
            def panel():
                for window in windows()-before:
                    title=xdo('getwindowname',window,check=False)
                    if text.casefold() in title.casefold():
                        return window,title
                return False
            try:
                window,title=wait_for(panel)
            except AssertionError:
                with sqlite3.connect(base/'catalog.db') as db:
                    print('history', db.execute('SELECT desktop_id FROM desktop_opens').fetchall(),flush=True)
                print('new window titles', [(w,xdo('getwindowname',w,check=False)) for w in windows()-before],flush=True)
                log.seek(0)
                print('launch log',log.read().decode(errors='replace'),flush=True)
                raise
            print(text,'opened native panel',repr(title),flush=True)
            xdo('windowactivate','--sync',window)
            xdo('key','alt+F4')
            time.sleep(.3)
        subprocess.run([str(root/'build/torchlight-gtk'),'--toggle'],check=True,stdout=log,stderr=log)
        wait_for(lambda:xdo('getwindowname',xdo('getactivewindow'))=='Torchlight')
        xdo('type','--clearmodifiers','--delay','1','fixture-target')
        time.sleep(.3)
        rows=query('fixture-target')
        assert rows[0]['kind']=='file' and 'path_b64' in rows[0]
        before=windows()
        xdo('key','ctrl+Return')
        def revealed():
            for window in windows()-before:
                if 'files' in xdo('getwindowname',window,check=False).lower():
                    return window
            return False
        try:
            window=wait_for(revealed)
        except AssertionError:
            print('reveal windows',[(w,xdo('getwindowname',w,check=False)) for w in windows()-before],flush=True)
            with sqlite3.connect(base/'catalog.db') as db:
                print('file history',db.execute('SELECT event_id,file_id FROM opens').fetchall(),flush=True)
            print('active',xdo('getwindowname',xdo('getactivewindow')),flush=True)
            log.seek(0)
            print('action log',log.read().decode(errors='replace'),flush=True)
            raise
        print('Ctrl+Enter made non-UTF-8 file parent reachable in native file manager',flush=True)
        xdo('windowactivate','--sync',window)
        xdo('key','alt+F4')
    finally:
        if gui is not None:
            gui.terminate();gui.wait(timeout=8)
        daemon.terminate();daemon.wait(timeout=15)
        log.seek(0)
        output=log.read().decode(errors='replace')
        assert 'Gtk-WARNING' not in output and 'CRITICAL' not in output,output
        log.close()
