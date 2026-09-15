//go:build linux

package rmchat

import (
	"context"
	"net/http"
	"path/filepath"
	"strings"
	"syscall"
	"testing"
	"time"
)

func TestUploadRejectsFIFOWithoutBlocking(t *testing.T) {
	root := t.TempDir()
	path := filepath.Join(root, "fifo.pdf")
	if err := syscall.Mkfifo(path, 0600); err != nil {
		t.Fatal(err)
	}
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { t.Error("FIFO reached network") }, root)
	setSession(c, "secret")
	done := make(chan *Error, 1)
	go func() {
		_, e := c.Upload(context.Background(), UploadParams{Path: path, Filename: "fifo.pdf", MIMEType: "application/pdf", SHA256: strings.Repeat("0", 64)})
		done <- e
	}()
	select {
	case e := <-done:
		requireCode(t, e, "FILE_REJECTED")
	case <-time.After(time.Second):
		t.Fatal("opening an invalid FIFO blocked")
	}
}
