package rmchat

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync/atomic"
	"testing"
)

func pdfFixture(t *testing.T, dir string) UploadParams {
	t.Helper()
	data := []byte("%PDF-1.7\nA small offline fixture\n%%EOF\n")
	path := filepath.Join(dir, "fixture.pdf")
	if err := os.WriteFile(path, data, 0600); err != nil {
		t.Fatal(err)
	}
	hash := sha256.Sum256(data)
	return UploadParams{Path: path, Filename: "Notes.pdf", MIMEType: "application/pdf", SHA256: hex.EncodeToString(hash[:])}
}
func TestPDFUploadThreeStepsAndNoCredentialToStorage(t *testing.T) {
	root := t.TempDir()
	p := pdfFixture(t, root)
	var storageCalls atomic.Int32
	storage := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		storageCalls.Add(1)
		if r.Method != "PUT" || r.Header.Get("Authorization") != "" || r.Header.Get("Cookie") != "" || r.Header.Get("X-Ms-Blob-Type") != "BlockBlob" {
			t.Error("storage request was not isolated")
		}
		data, _ := io.ReadAll(r.Body)
		hash := sha256.Sum256(data)
		if hex.EncodeToString(hash[:]) != p.SHA256 {
			t.Error("wrong uploaded bytes")
		}
		http.SetCookie(w, &http.Cookie{Name: "storage-only", Value: "fixture", Path: "/"})
		w.WriteHeader(201)
	}))
	defer storage.Close()
	var reservation, confirmation atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer secret" {
			t.Error("missing account credential")
		}
		switch r.URL.Path {
		case "/backend-api/files":
			reservation.Add(1)
			http.SetCookie(w, &http.Cookie{Name: "continuity", Value: "upload-fixture", Path: "/"})
			var body map[string]any
			json.NewDecoder(r.Body).Decode(&body)
			if body["use_case"] != "my_files" || body["mime_type"] != "application/pdf" || body["store_in_library"] != true {
				t.Error("wrong PDF reservation")
			}
			jsonResponse(w, map[string]any{"file_id": "file-1", "upload_url": storage.URL + "/signed?private=value", "library_file_id": "library-1"})
		case "/backend-api/files/file-1/uploaded":
			confirmation.Add(1)
			requireCookie(t, r, "continuity", "upload-fixture")
			if len(r.Cookies()) != 1 {
				t.Error("storage cookie reached authenticated confirmation")
			}
			jsonResponse(w, map[string]any{})
		default:
			t.Errorf("unexpected %s", r.URL.Path)
		}
	}, root)
	c.allowStorage = func(u *url.URL) bool { return u.String() == storage.URL+"/signed?private=value" }
	setSession(c, "secret")
	result, e := c.Upload(context.Background(), p)
	if e != nil {
		t.Fatal(e)
	}
	if reservation.Load() != 1 || confirmation.Load() != 1 || storageCalls.Load() != 1 || result.(map[string]any)["attachmentRef"] != "file-1" {
		t.Fatalf("incomplete upload %+v", result)
	}
	c.Logout()
	if len(c.state.uploads) != 0 {
		t.Fatal("upload escaped account lifetime")
	}
}
func TestUploadRejectsOutsideRootHashMismatchAndNonPDF(t *testing.T) {
	root := t.TempDir()
	outside := t.TempDir()
	p := pdfFixture(t, root)
	var network atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { network.Add(1) }, root)
	setSession(c, "secret")
	bad := p
	bad.Path = pdfFixture(t, outside).Path
	_, e := c.Upload(context.Background(), bad)
	requireCode(t, e, "FILE_REJECTED")
	bad = p
	bad.SHA256 = strings.Repeat("0", 64)
	_, e = c.Upload(context.Background(), bad)
	requireCode(t, e, "FILE_CHANGED")
	bad = p
	bad.Filename = "../notes.pdf"
	_, e = c.Upload(context.Background(), bad)
	requireCode(t, e, "FILE_REJECTED")
	content := []byte("This is not a PDF")
	os.WriteFile(p.Path, content, 0600)
	hash := sha256.Sum256(content)
	p.SHA256 = hex.EncodeToString(hash[:])
	_, e = c.Upload(context.Background(), p)
	requireCode(t, e, "FILE_REJECTED")
	if network.Load() != 0 {
		t.Fatal("invalid file reached network")
	}
}
func TestUploadRejectsSymlinkEscape(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Windows symlink creation requires host policy; Linux ARM64 covers containment")
	}
	root := t.TempDir()
	outside := t.TempDir()
	p := pdfFixture(t, outside)
	link := filepath.Join(root, "escape.pdf")
	if err := os.Symlink(p.Path, link); err != nil {
		t.Fatal(err)
	}
	p.Path = link
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { t.Error("symlink reached network") }, root)
	setSession(c, "secret")
	_, e := c.Upload(context.Background(), p)
	requireCode(t, e, "FILE_REJECTED")
}
func TestUploadRejectsOversizedRegularFile(t *testing.T) {
	root := t.TempDir()
	p := pdfFixture(t, root)
	file, err := os.OpenFile(p.Path, os.O_WRONLY, 0600)
	if err != nil {
		t.Fatal(err)
	}
	if err = file.Truncate(MaxPDF + 1); err != nil {
		t.Fatal(err)
	}
	file.Close()
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { t.Error("oversize reached network") }, root)
	setSession(c, "secret")
	_, e := c.Upload(context.Background(), p)
	requireCode(t, e, "FILE_REJECTED")
}
func TestUploadSnapshotSurvivesOriginalMutation(t *testing.T) {
	root := t.TempDir()
	p := pdfFixture(t, root)
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {}, root)
	snapshot, cleanup, _, e := c.snapshot(context.Background(), p)
	if e != nil {
		t.Fatal(e)
	}
	defer cleanup()
	os.WriteFile(p.Path, []byte("changed"), 0600)
	data, _ := io.ReadAll(snapshot)
	hash := sha256.Sum256(data)
	if hex.EncodeToString(hash[:]) != p.SHA256 {
		t.Fatal("uploaded snapshot could change after validation")
	}
}
func TestUploadRejectsUntrustedStorageDestination(t *testing.T) {
	root := t.TempDir()
	p := pdfFixture(t, root)
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, map[string]any{"file_id": "file-1", "upload_url": "http://127.0.0.1/private"})
	}, root)
	setSession(c, "secret")
	_, e := c.Upload(context.Background(), p)
	requireCode(t, e, "UPSTREAM_ERROR")
}

func TestUploadConfirmationChallengeDoesNotRegisterAttachment(t *testing.T) {
	root := t.TempDir()
	p := pdfFixture(t, root)
	storage := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(201) }))
	defer storage.Close()
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/uploaded") {
			jsonResponse(w, map[string]any{"challenge": map[string]any{"required": true}})
		} else {
			jsonResponse(w, map[string]any{"file_id": "file-1", "upload_url": storage.URL})
		}
	}, root)
	c.allowStorage = func(u *url.URL) bool { return u.String() == storage.URL }
	setSession(c, "secret")
	_, e := c.Upload(context.Background(), p)
	requireCode(t, e, "WEB_AUTH_REQUIRED")
	if len(c.state.uploads) != 0 {
		t.Fatal("unconfirmed upload became a ready attachment")
	}
}
