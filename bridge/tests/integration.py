#!/usr/bin/env python3
"""Integration tests of the real native Bridge process, always under a temp sandbox."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest
import struct
import zlib

BIN = None


def pdf_bytes():
    objects = [b"<< /Type /Catalog /Pages 2 0 R >>",
               b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>",
               b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] >>",
               b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 300] >>"]
    data = b"%PDF-1.4\n"
    offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(data))
        data += str(i).encode() + b" 0 obj\n" + obj + b"\nendobj\n"
    start = len(data)
    data += b"xref\n0 5\n0000000000 65535 f \n"
    data += b"".join(f"{offset:010} 00000 n \n".encode() for offset in offsets[1:])
    data += f"trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n{start}\n%%EOF\n".encode()
    return data


class BridgeIntegration(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="paper-bridge-test-")
        self.root = Path(self.temp.name)
        (self.root / "state/inputs").mkdir(parents=True)

    def tearDown(self):
        self.temp.cleanup()

    def call(self, route, body=None, fault=None, unvalidated=False):
        env = os.environ.copy()
        if fault:
            env["PAPER_BRIDGE_FAULT"] = fault
        request = {"method": "POST" if body is not None else "GET", "route": route, "body": body or {}}
        command = [BIN, "--sandbox", str(self.root), "--request", json.dumps(request)]
        if unvalidated:
            command.append("--sandbox-unvalidated")
        p = subprocess.run(command,
                           capture_output=True, timeout=20, env=env)
        self.assertIn(p.returncode, (0, 1), p.stderr.decode())
        return json.loads(p.stdout)

    def request(self, data, name="source.pdf", key="import-1"):
        path = self.root / "state/inputs" / name
        path.write_bytes(data)
        return {"path": str(path), "displayName": "Cours de test", "sha256": hashlib.sha256(data).hexdigest(), "idempotencyKey": key}

    def test_pdf_roundtrip_and_replay(self):
        request = self.request(pdf_bytes())
        first = self.call("/v1/imports", request)
        self.assertEqual(first["status"], "succeeded", first)
        second = self.call("/v1/imports", request)
        self.assertEqual(first, second)
        content = json.loads((self.root / "store" / (first["documentId"] + ".content")).read_text())
        self.assertEqual(content["pageCount"], 2)
        self.assertEqual(len(content["pages"]), 2)
        self.assertEqual(len(list((self.root / "store").glob("*.metadata"))), 1)
        self.assertEqual((self.root / "store" / (first["documentId"] + ".pdf")).read_bytes(), pdf_bytes())

    def test_idempotency_conflict_preserves_original(self):
        first = self.call("/v1/imports", self.request(pdf_bytes()))
        second = self.call("/v1/imports", self.request(pdf_bytes() + b"\n% changed"))
        self.assertEqual(second["error"]["code"], "IDEMPOTENCY_CONFLICT")
        self.assertTrue((self.root / "store" / (first["documentId"] + ".metadata")).exists())

    def test_invalid_input_and_path_never_create_document(self):
        result = self.call("/v1/imports", self.request(b"<html>Login required</html>"))
        self.assertIn("error", result)
        request = self.request(pdf_bytes(), key="outside")
        request["path"] = "/etc/passwd"
        self.assertEqual(self.call("/v1/imports", request)["error"]["code"], "INPUT_PATH_NOT_ALLOWED")
        self.assertEqual(len(list((self.root / "store").glob("*.metadata"))), 0)

    def test_fault_rolls_back_only_new_document(self):
        keep = self.call("/v1/notebooks", {"displayName": "À conserver", "idempotencyKey": "keep"})
        result = self.call("/v1/imports", self.request(pdf_bytes()), "after_first_file")
        self.assertEqual(result["error"]["code"], "INJECTED_FAILURE")
        files = list((self.root / "store").glob("*.metadata"))
        self.assertEqual([f.stem for f in files], [keep["documentId"]])
        with sqlite3.connect(self.root / "state/operations.sqlite") as db:
            self.assertEqual(db.execute("SELECT state FROM operations WHERE idempotency_key='import-1'").fetchone()[0], "rolled_back")

    def test_committed_recovery_preserves_phase_and_user_edits_after_revalidation(self):
        note = self.call("/v1/notebooks", {"displayName": "Document modifié", "idempotencyKey": "committed"})
        document = self.root / "store" / note["documentId"]
        page = next(document.glob("*.rm"))
        # An edit made after services restarted must survive recovery of the committing operation.
        page.write_bytes(page.read_bytes() + b"user-edited-after-commit")
        before = {str(path.relative_to(self.root / "store")): path.read_bytes()
                  for path in (self.root / "store").rglob("*") if path.is_file()}
        with sqlite3.connect(self.root / "state/operations.sqlite") as db:
            db.execute("UPDATE operations SET state='verifying',result_json=NULL WHERE idempotency_key='committed'")

        capabilities = self.call("/v1/capabilities", unvalidated=True)
        self.assertEqual(capabilities["document.createNotebook"], "unavailable")
        with sqlite3.connect(self.root / "state/operations.sqlite") as db:
            phase, result = db.execute("SELECT state,result_json FROM operations WHERE idempotency_key='committed'").fetchone()
            self.assertEqual(phase, "verifying")
            self.assertEqual(json.loads(result)["error"]["code"], "PAPER_FIRMWARE_UNSUPPORTED")
        self.call("/v1/health")  # Validation is restored; recover() must preserve the committed document.
        with sqlite3.connect(self.root / "state/operations.sqlite") as db:
            self.assertEqual(db.execute("SELECT state FROM operations WHERE idempotency_key='committed'").fetchone()[0], "succeeded")
        after = {str(path.relative_to(self.root / "store")): path.read_bytes()
                 for path in (self.root / "store").rglob("*") if path.is_file()}
        self.assertEqual(after, before)

    def test_legacy_unknown_recovery_phase_never_deletes_document(self):
        note = self.call("/v1/notebooks", {"displayName": "À vérifier", "idempotencyKey": "legacy"})
        with sqlite3.connect(self.root / "state/operations.sqlite") as db:
            db.execute("UPDATE operations SET state='recovery_requires_review' WHERE idempotency_key='legacy'")
        self.call("/v1/health", unvalidated=True)
        self.call("/v1/health")
        self.assertTrue((self.root / "store" / (note["documentId"] + ".metadata")).exists())
        self.assertTrue(next((self.root / "store" / note["documentId"]).glob("*.rm")).is_file())
        with sqlite3.connect(self.root / "state/operations.sqlite") as db:
            phase, result = db.execute("SELECT state,result_json FROM operations WHERE idempotency_key='legacy'").fetchone()
            self.assertEqual(phase, "recovery_requires_review")
            self.assertEqual(json.loads(result)["error"]["code"], "RECOVERY_REQUIRES_REVIEW")

    def test_unvalidated_simulation_requires_sandbox_and_cannot_enable_imports(self):
        result = self.call("/v1/notebooks", {"displayName": "Refusé", "idempotencyKey": "disabled"}, unvalidated=True)
        self.assertEqual(result["error"]["code"], "PAPER_FIRMWARE_UNSUPPORTED")
        self.assertEqual(len(list((self.root / "store").glob("*.metadata"))), 0)
        request = {"method": "GET", "route": "/v1/health", "body": {}}
        process = subprocess.run([BIN, "--sandbox-unvalidated", "--request", json.dumps(request)],
                                 capture_output=True, timeout=20)
        self.assertEqual(process.returncode, 1)
        self.assertEqual(json.loads(process.stdout)["error"]["code"], "SANDBOX_REQUIRED")

    def test_native_scene_and_notebook_identity(self):
        body = {"displayName": "Note du jour", "idempotencyKey": "day:2026-09-04"}
        note = self.call("/v1/notebooks", body)
        body["displayName"] = "Nouveau titre"
        self.assertEqual(self.call("/v1/notebooks", body)["documentId"], note["documentId"])
        scene = {"schemaVersion": 1, "page": {"width": 1404, "height": 1872}, "strokes": [
            {"width": 3, "color": "#000000", "points": [[100, 100], [300, 200], [500, 100]]}]}
        result = self.call("/v1/imports", self.request(json.dumps(scene).encode(), "stencil.paper-scene.json"))
        self.assertEqual(result["status"], "succeeded", result)
        pages = list((self.root / "store" / result["documentId"]).glob("*.rm"))
        self.assertEqual(len(pages), 1)
        self.assertTrue(pages[0].read_bytes().startswith(b"reMarkable .lines file, version=6"))
        if os.environ.get("PAPER_TEST_ARTIFACTS"):
            output = Path(os.environ["PAPER_TEST_ARTIFACTS"])
            output.mkdir(parents=True, exist_ok=True)
            (output / "roundtrip.rm").write_bytes(pages[0].read_bytes())

    def test_real_unix_http_transport(self):
        endpoint = str(self.root / "bridge.sock")
        process = subprocess.Popen([BIN, "--sandbox", str(self.root), "--socket", endpoint], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            for _ in range(100):
                if Path(endpoint).exists():
                    break
                if process.poll() is not None:
                    self.fail(process.stderr.read().decode())
                time.sleep(.02)
            with socket.socket(socket.AF_UNIX) as connection:
                connection.settimeout(5)
                connection.connect(endpoint)
                connection.sendall(b"GET /v1/health HTTP/1.1\r\nHost: localhost\r\nContent-Length: 2\r\n\r\n{}")
                response = b""
                while chunk := connection.recv(8192):
                    response += chunk
            self.assertIn(b"200 OK", response)
            self.assertEqual(json.loads(response.split(b"\r\n\r\n", 1)[1])["status"], "ok")
        finally:
            process.terminate()
            process.wait(5)
            process.stderr.close()

    def test_image_is_validated_and_converted(self):
        def chunk(name, data):
            return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
        png += chunk(b'IDAT', zlib.compress(b'\0\0\0\0')) + chunk(b'IEND', b'')
        result = self.call('/v1/imports', self.request(png, 'pixel.png'))
        self.assertEqual(result['status'], 'succeeded', result)
        content = json.loads((self.root / 'store' / (result['documentId'] + '.content')).read_text())
        self.assertEqual(content['fileType'], 'pdf')
        self.assertEqual(content['pageCount'], 1)
        huge = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 20000, 20000, 8, 2, 0, 0, 0))
        huge += chunk(b'IDAT', zlib.compress(b'\0\0\0\0')) + chunk(b'IEND', b'')
        rejected = self.call('/v1/imports', self.request(huge, 'bomb.png', 'bomb'))
        self.assertIn('error', rejected)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    args, rest = parser.parse_known_args()
    BIN = str(Path(args.binary).resolve())
    unittest.main(argv=[__file__] + rest)
