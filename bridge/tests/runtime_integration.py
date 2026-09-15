#!/usr/bin/env python3
"""Real Bridge transport/startup tests using only a temp sandbox and a fake systemd runner."""
import argparse
import copy
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import tempfile
import time
import unittest
import threading
import hashlib
import struct
import zlib
from integration import pdf_bytes

BIN = PROBE = None


class ManagedBridgeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='repaper-bridge-runtime-')
        self.root = Path(self.temp.name)
        self.runtime = self.root/'runtime'; self.runtime.mkdir()
        self.endpoint = str(self.root/'core.sock')
        (self.runtime/'paper-bridge').symlink_to(BIN)
        self.script(self.runtime/'bridge-run', '#!/bin/sh\nexec '+shlex.join([
            BIN, '--sandbox', str(self.root/'sandbox'), '--sandbox-unvalidated',
            '--socket', self.endpoint, '--idle-timeout', '2'])+'\n')
        self.manager = self.root/'fake-systemd-run'
        self.script(self.manager, '''#!/usr/bin/python3
import json, os, pathlib, subprocess, sys
root=pathlib.Path(__file__).parent
with (root/'manager-calls.jsonl').open('a') as out:
 out.write(json.dumps({'args':sys.argv[1:], 'preload':os.getenv('LD_PRELOAD'), 'qtfb':os.getenv('QTFB_KEY')})+'\\n')
with (root/'worker.log').open('ab') as log:
 child=subprocess.Popen([sys.argv[-1]], stdout=log, stderr=log, start_new_session=True)
with (root/'workers').open('a') as out: out.write(str(child.pid)+'\\n')
''')

    def tearDown(self):
        if (self.root/'workers').exists():
            for value in (self.root/'workers').read_text().splitlines():
                try: os.kill(int(value), signal.SIGTERM)
                except ProcessLookupError: pass
        time.sleep(.05)
        self.temp.cleanup()

    def script(self, path, data):
        path.write_text(data); path.chmod(0o755)

    def command(self):
        return [PROBE, '--ensure', str(self.runtime), self.endpoint, str(self.root/'start.lock'), str(self.manager)]

    def ensure(self):
        process = subprocess.run(self.command(), capture_output=True, text=True, timeout=16)
        self.assertEqual(process.returncode, 0, process.stderr+process.stdout)
        return json.loads(process.stdout)

    def calls(self):
        return [json.loads(line) for line in (self.root/'manager-calls.jsonl').read_text().splitlines()]

    def request(self, method, route, starter=None):
        env = dict(os.environ, PAPER_BRIDGE_SOCKET=self.endpoint)
        if starter: env['REPAPER_BRIDGE_STARTER'] = str(starter)
        process = subprocess.run([PROBE, '--request', method, route], env=env,
                                 capture_output=True, text=True, timeout=18)
        self.assertIn(process.returncode, (0, 1), process.stderr)
        return json.loads(process.stdout)

    def test_start_reuse_real_health_and_unvalidated_capabilities(self):
        first = self.ensure(); second = self.ensure()
        self.assertEqual(first['status'], 'started')
        self.assertEqual(second['status'], 'already-running')
        self.assertEqual(first['health']['service'], 'paper-bridge')
        self.assertEqual(len(self.calls()), 1)
        args = self.calls()[0]['args']
        self.assertIn('--collect', args)
        self.assertIn('--service-type=exec', args)
        self.assertIn('--property=KillMode=control-group', args)
        self.assertEqual(args[-1], str(self.runtime/'bridge-run'))
        capabilities = self.request('GET', '/v1/capabilities')
        self.assertEqual(capabilities['connection'], 'connected')
        self.assertEqual(capabilities['document.list'], 'available')
        self.assertEqual(capabilities['document.createNotebook'], 'unavailable')
        refused = self.request('POST', '/v1/notebooks')
        self.assertEqual(refused['error']['code'], 'PAPER_FIRMWARE_UNSUPPORTED')
        self.assertIn('adaptateur de test', refused['error']['message'])
        self.assertEqual(list((self.root/'sandbox/store').glob('*.metadata')), [])

    def test_two_starters_create_one_worker(self):
        first = subprocess.Popen(self.command(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        second = subprocess.Popen(self.command(), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        for process in (first, second):
            out, err = process.communicate(timeout=16)
            self.assertEqual(process.returncode, 0, out+err)
            self.assertIn(json.loads(out)['status'], ('started', 'already-running'))
        self.assertEqual(len(self.calls()), 1)

    def test_client_starts_missing_service_and_restarts_after_idle(self):
        starter = self.root/'starter'
        self.script(starter, '#!/bin/sh\nexec '+shlex.join(self.command())+'\n')
        first = self.request('GET', '/v1/health', starter)
        self.assertEqual(first['status'], 'ok')
        time.sleep(2.4)
        second = self.request('GET', '/v1/capabilities', starter)
        self.assertEqual(second['connection'], 'connected')
        self.assertEqual(len(self.calls()), 2)

    def test_failed_starter_returns_its_structured_error(self):
        starter = self.root/'failed-starter'
        self.script(starter, '#!/bin/sh\nprintf \'%s\\n\' \'{"error":{"code":"START_TEST_FAILURE","message":"Service test unavailable"}}\'\nexit 1\n')
        result = self.request('GET', '/v1/health', starter)
        self.assertEqual(result['error']['code'], 'START_TEST_FAILURE')

    def test_singleton_prevents_second_worker_on_another_socket(self):
        self.ensure()
        process = subprocess.run([BIN, '--sandbox', str(self.root/'sandbox'), '--socket', str(self.root/'other.sock')],
                                 capture_output=True, text=True, timeout=5)
        self.assertEqual(process.returncode, 1)
        self.assertIn('BRIDGE_ALREADY_RUNNING', process.stderr)
        self.assertEqual(self.request('GET', '/v1/health')['status'], 'ok')


class NativeNotesBridgeTests(unittest.TestCase):
    document = '11111111-2222-4333-8444-555555555555'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='repaper-native-notes-')
        self.root = Path(self.temp.name)
        (self.root/'inputs').mkdir()
        (self.root/'store').mkdir()
        (self.root/'store/keep.metadata').write_text('untouched original document')
        self.endpoint = str(self.root/'native.sock')
        self.calls = []
        self.imports_available = False
        self.import_error = None
        self.import_count = 0
        self.imported = {}
        self.notebooks = {}
        self.listener = socket.socket(socket.AF_UNIX)
        self.listener.bind(self.endpoint); self.listener.listen(); self.listener.settimeout(.1)
        self.stopping = False
        self.thread = threading.Thread(target=self.serve, daemon=True); self.thread.start()

    def tearDown(self):
        self.stopping = True; self.thread.join(timeout=2); self.listener.close(); self.temp.cleanup()

    def serve(self):
        while not self.stopping:
            try: connection, _ = self.listener.accept()
            except socket.timeout: continue
            with connection:
                connection.settimeout(2); request = b''
                while b'\r\n\r\n' not in request: request += connection.recv(4096)
                header, body = request.split(b'\r\n\r\n', 1)
                length = int(next(line.split(b':', 1)[1] for line in header.split(b'\r\n') if line.lower().startswith(b'content-length:')))
                while len(body) < length: body += connection.recv(4096)
                method, route, _ = header.split(b'\r\n', 1)[0].decode().split(' ')
                self.calls.append((method, route, json.loads(body)))
                if route == '/v1/capabilities':
                    result = {'document.createNotebook':'available', 'document.open':'available'}
                    if self.imports_available:
                        result.update({'document.import.pdf':'available', 'document.import.image':'available'})
                elif route == '/v1/notebooks':
                    supplied = json.loads(body)
                    result = self.notebooks.setdefault(supplied['idempotencyKey'],
                        {'documentId':self.document, 'status':'succeeded', 'nativeIndexVerified':True})
                elif route == '/v1/imports':
                    supplied = json.loads(body)
                    if self.import_error:
                        result = self.import_error
                    elif supplied['idempotencyKey'] in self.imported:
                        result = self.imported[supplied['idempotencyKey']]
                    else:
                        self.import_count += 1
                        result = {'documentId':self.document, 'status':'succeeded', 'nativeIndexVerified':True}
                        self.imported[supplied['idempotencyKey']] = result
                else:
                    # Include documentId deliberately: opening must never emit another imported event.
                    result = {'documentId':self.document, 'status':'opened', 'message':'Native document opened'}
                encoded = json.dumps(result).encode()
                connection.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: '+str(len(encoded)).encode()+b'\r\nConnection: close\r\n\r\n'+encoded)

    def native(self, method, route, body=None):
        result = subprocess.run([PROBE, '--native-request', str(self.root), method, route, json.dumps(body or {})],
            env=dict(os.environ, REPAPER_NATIVE_DOCUMENTS_SOCKET=self.endpoint), capture_output=True, text=True, timeout=5)
        self.assertIn(result.returncode, (0, 1), result.stdout+result.stderr)
        return json.loads(result.stdout)

    def input(self, data, name='course.pdf', key='moodle:file:one'):
        path = self.root/'inputs'/name; path.write_bytes(data)
        return {'path':str(path), 'sha256':hashlib.sha256(data).hexdigest(),
                'displayName':'Native imported course', 'idempotencyKey':key}

    def assert_store_untouched(self):
        self.assertEqual([p.name for p in (self.root/'store').iterdir()], ['keep.metadata'])
        self.assertEqual((self.root/'store/keep.metadata').read_text(), 'untouched original document')
        self.assertFalse((self.root/'state/adapter-validation.json').exists())

    def test_native_pdf_import_without_writer_attestation_and_replay(self):
        self.imports_available = True
        capabilities = self.native('GET', '/v1/capabilities')
        self.assertEqual(capabilities['document.import.pdf'], 'available')
        self.assertEqual(capabilities['document.import.image'], 'available')
        self.assertEqual(capabilities['document.import.scene'], 'unavailable')
        source = self.input(pdf_bytes())
        first = self.native('POST', '/v1/imports', source)
        self.assertEqual(first['documentId'], self.document)
        prepared = self.calls[-1][2]
        path = Path(prepared['path'])
        self.assertTrue(path.is_relative_to(self.root/'state/native-imports'))
        self.assertEqual(path.read_bytes(), pdf_bytes())
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(path.parent.stat().st_mode & 0o777, 0o700)
        self.assertEqual(prepared['sha256'], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(self.native('POST', '/v1/imports', source), first)
        self.assertEqual(self.calls[-1][2], prepared)
        self.assertEqual(self.import_count, 1)
        self.assert_store_untouched()

    def test_native_image_conversion_retains_snapshot_after_timeout(self):
        def chunk(kind, value):
            return struct.pack('>I', len(value))+kind+value+struct.pack('>I', zlib.crc32(kind+value))
        png = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR', struct.pack('>IIBBBBB', 20, 10, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress((b'\0'+b'\x7f\x00\xff'*20)*10))+chunk(b'IEND', b'')
        source = self.input(png, 'course.png')
        self.import_error = {'error':{'code':'NATIVE_TIMEOUT', 'message':'Synthetic timeout', 'retryable':True}}
        result = self.native('POST', '/v1/imports', source)
        self.assertEqual(result['error']['code'], 'NATIVE_TIMEOUT')
        prepared = self.calls[-1][2]
        snapshot = Path(prepared['path']).read_bytes()
        self.assertTrue(snapshot.startswith(b'%PDF-'))
        self.import_error = None
        result = self.native('POST', '/v1/imports', source)
        self.assertEqual(result['status'], 'succeeded')
        self.assertEqual(self.calls[-1][2], prepared)
        self.assertEqual(Path(prepared['path']).read_bytes(), snapshot)
        self.assert_store_untouched()

    def test_invalid_imports_do_not_reach_native_service(self):
        invalid = self.input(b'%PDF-1.4\nnot a PDF')
        result = self.native('POST', '/v1/imports', invalid)
        self.assertEqual(result['error']['code'], 'INVALID_OR_ENCRYPTED_PDF')
        source = self.input(pdf_bytes())
        bad_hash = dict(source, sha256='0'*64)
        self.assertEqual(self.native('POST', '/v1/imports', bad_hash)['error']['code'], 'INPUT_HASH_MISMATCH')
        outside = self.root/'outside.pdf'; outside.write_bytes(pdf_bytes())
        self.assertEqual(self.native('POST', '/v1/imports', dict(source, path=str(outside)))['error']['code'], 'INPUT_PATH_NOT_ALLOWED')
        link = self.root/'inputs/link.pdf'; link.symlink_to(Path(source['path']))
        self.assertEqual(self.native('POST', '/v1/imports', dict(source, path=str(link)))['error']['code'], 'INPUT_PATH_NOT_ALLOWED')
        self.assertEqual(self.calls, [])
        self.assert_store_untouched()

    def test_import_key_conflict_and_unconfirmed_response_fail_closed(self):
        source = self.input(pdf_bytes())
        self.import_error = {'status':'succeeded', 'documentId':self.document, 'nativeIndexVerified':False}
        result = self.native('POST', '/v1/imports', source)
        self.assertEqual(result['error']['code'], 'NATIVE_DOCUMENT_UNCONFIRMED')
        before = len(self.calls)
        conflicting = self.input(pdf_bytes()+b'\n% modified')
        self.assertEqual(self.native('POST', '/v1/imports', conflicting)['error']['code'], 'IDEMPOTENCY_CONFLICT')
        self.assertEqual(len(self.calls), before)
        self.assert_store_untouched()

    def test_client_import_signal_once_through_real_bridge_and_idempotent_host(self):
        source = self.input(pdf_bytes())
        endpoint = str(self.root/'bridge.sock')
        service = subprocess.Popen([PROBE, '--native-serve', str(self.root), endpoint],
            env=dict(os.environ, REPAPER_NATIVE_DOCUMENTS_SOCKET=self.endpoint), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic()+3
            while not Path(endpoint).exists() and time.monotonic()<deadline: time.sleep(.01)
            for _ in range(2):
                result = subprocess.run([PROBE, '--import-once', source['path'], source['displayName'], source['idempotencyKey']],
                    env=dict(os.environ, PAPER_BRIDGE_SOCKET=endpoint), capture_output=True, text=True, timeout=6)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)['importedSignals'], 1)
                self.assertEqual(json.loads(result.stdout)['documentId'], self.document)
            self.assertEqual(self.import_count, 1)
        finally:
            service.terminate(); service.communicate(timeout=3)
        self.assert_store_untouched()

    def test_native_notes_work_without_enabling_filesystem_imports(self):
        capabilities = self.native('GET', '/v1/capabilities')
        self.assertEqual(capabilities['document.createNotebook'], 'available')
        self.assertEqual(capabilities['document.open'], 'available')
        self.assertEqual(capabilities['document.import.pdf'], 'unavailable')
        request = {'displayName':'Notes du cours', 'idempotencyKey':'reagenda:event:123', 'callerQtfbKey':1234}
        created = self.native('POST', '/v1/notebooks', request)
        self.assertEqual(created['documentId'], self.document); self.assertTrue(created['nativeIndexVerified'])
        opened = self.native('POST', '/v1/documents/'+self.document+'/open', {'callerQtfbKey':1234})
        self.assertEqual(opened['status'], 'opened')
        self.assertEqual(self.calls[-2], ('POST', '/v1/notebooks', request))
        self.assertEqual(self.calls[-1][2]['callerQtfbKey'], 1234)
        self.assertEqual([p.name for p in (self.root/'store').iterdir()], ['keep.metadata'])
        self.assertEqual((self.root/'store/keep.metadata').read_text(), 'untouched original document')

    def test_client_adds_caller_framebuffer_and_open_does_not_loop(self):
        env = dict(os.environ, PAPER_BRIDGE_SOCKET=self.endpoint, QTFB_KEY='2468')
        response = subprocess.run([PROBE, '--request', 'POST', '/v1/notebooks', json.dumps({'displayName':'Test', 'idempotencyKey':'note-key'})],
                                  env=env, capture_output=True, text=True, timeout=5)
        self.assertEqual(response.returncode, 0, response.stderr)
        self.assertEqual(self.calls[-1][2]['callerQtfbKey'], 2468)
        opened = subprocess.run([PROBE, '--open-once', self.document], env=env, capture_output=True, text=True, timeout=6)
        self.assertEqual(opened.returncode, 0, opened.stderr)
        self.assertEqual(json.loads(opened.stdout)['importedSignals'], 0)
        self.assertEqual(self.calls[-1][2]['callerQtfbKey'], 2468)

    @staticmethod
    def agenda_context():
        return {'schemaVersion':1, 'kind':'event', 'date':'2026-09-07', 'timeZone':'Europe/Paris',
                'event':{'id':'mycpe:course-17', 'title':'Travaux dirigés', 'subject':'Électronique',
                         'start':'2026-09-07T09:00:00+02:00', 'end':'2026-09-07T10:30:00+02:00',
                         'timeZone':'Europe/Paris', 'allDay':False}}

    def test_agenda_context_preserves_existing_notebook_key(self):
        body = {'displayName':'Notes existantes', 'idempotencyKey':'reagenda:event:mycpe:course-17'}
        original = self.native('POST', '/v1/notebooks', body)
        self.assertNotIn('agenda', self.calls[-1][2])
        body['agenda'] = self.agenda_context()
        body['displayName'] = 'Travaux dirigés'
        repeated = self.native('POST', '/v1/notebooks', body)
        self.assertEqual(repeated['documentId'], original['documentId'])
        self.assertEqual(self.calls[-1][2], body)
        self.assertEqual(list(self.notebooks), ['reagenda:event:mycpe:course-17'])
        self.assert_store_untouched()

    def test_client_forwards_agenda_snapshot_and_caller_through_real_bridge(self):
        endpoint = str(self.root/'bridge.sock')
        service = subprocess.Popen([PROBE, '--native-serve', str(self.root), endpoint],
            env=dict(os.environ, REPAPER_NATIVE_DOCUMENTS_SOCKET=self.endpoint), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic()+3
            while not Path(endpoint).exists() and time.monotonic()<deadline: time.sleep(.01)
            context = self.agenda_context()
            key = 'reagenda:event:'+context['event']['id']
            result = subprocess.run([PROBE, '--notebook-once', context['event']['title'], key, json.dumps(context)],
                env=dict(os.environ, PAPER_BRIDGE_SOCKET=endpoint, QTFB_KEY='3141'), capture_output=True, text=True, timeout=6)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)['importedSignals'], 1)
            self.assertEqual(self.calls[-1], ('POST', '/v1/notebooks',
                {'displayName':context['event']['title'], 'idempotencyKey':key, 'agenda':context, 'callerQtfbKey':3141}))
        finally:
            service.terminate(); service.communicate(timeout=3)
        self.assert_store_untouched()

    def test_agenda_dst_named_timezone_and_all_day_dates(self):
        contexts = []
        dst = self.agenda_context()
        dst['date'] = '2026-10-25'
        dst['event'].update(start='2026-10-25T01:30:00+02:00', end='2026-10-25T03:30:00+01:00')
        contexts.append(dst)
        overseas = self.agenda_context()
        overseas['timeZone'] = overseas['event']['timeZone'] = 'America/New_York'
        overseas['event'].update(start='2026-09-07T09:00:00-04:00', end='2026-09-07T10:30:00-04:00')
        contexts.append(overseas)
        all_day = copy.deepcopy(dst)
        all_day['event'].update(start='2026-10-25T00:00:00+02:00', end='2026-10-26T00:00:00+01:00', allDay=True)
        contexts.append(all_day)
        contexts.append({'schemaVersion':1, 'kind':'day', 'date':'2026-10-25', 'timeZone':'Europe/Paris'})
        for context in contexts:
            with self.subTest(context=context):
                body = {'displayName':'Agenda', 'idempotencyKey':'reagenda:day:2026-10-25', 'agenda':context}
                self.assertEqual(self.native('POST', '/v1/notebooks', body)['status'], 'succeeded')
                self.assertEqual(self.calls[-1][2]['agenda'], context)

    def test_malformed_agenda_context_is_rejected_before_native_call(self):
        mutations = [
            lambda c: c.update(schemaVersion=2), lambda c: c.update(schemaVersion=1.5),
            lambda c: c.update(kind='unknown'), lambda c: c.update(date='2026-02-30'),
            lambda c: c.update(timeZone='Not/AZone'), lambda c: c.update(kind='day'),
            lambda c: c.update(event=[]), lambda c: c.update(extra='x'*8192),
            lambda c: c['event'].update(id=''), lambda c: c['event'].update(title='bad\0name'),
            lambda c: c['event'].update(subject='x'*513), lambda c: c['event'].update(allDay='false'),
            lambda c: c['event'].update(timeZone='America/New_York'),
            lambda c: c['event'].update(start='2026-09-07T09:00:00'),
            lambda c: c['event'].update(end='2026-09-07T08:00:00+02:00'),
            lambda c: c['event'].update(start='2026-09-07T09:00:00+01:00')]
        contexts = [None, 'event']
        for mutate in mutations:
            context = self.agenda_context(); mutate(context); contexts.append(context)
        for context in contexts:
            with self.subTest(context=context):
                result = self.native('POST', '/v1/notebooks',
                    {'displayName':'Agenda', 'idempotencyKey':'reagenda:event:mycpe:course-17', 'agenda':context})
                self.assertEqual(result['error']['code'], 'INVALID_AGENDA_CONTEXT')
        self.assertEqual(self.calls, [])
        self.assert_store_untouched()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--probe', required=True)
    args, remaining = parser.parse_known_args()
    BIN = str(Path(args.binary).resolve()); PROBE = str(Path(args.probe).resolve())
    unittest.main(argv=[__file__, *remaining])
