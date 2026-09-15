package rmchat

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
)

type UploadParams struct {
	Path     string `json:"path"`
	Filename string `json:"filename"`
	MIMEType string `json:"mimeType"`
	SHA256   string `json:"sha256"`
}
type contextReader struct {
	ctx context.Context
	r   io.Reader
}

func (r contextReader) Read(p []byte) (int, error) {
	if err := r.ctx.Err(); err != nil {
		return 0, err
	}
	return r.r.Read(p)
}
func (c *Client) snapshot(ctx context.Context, p UploadParams) (*os.File, func(), int64, *Error) {
	noop := func() {}
	if !filepath.IsAbs(p.Path) || p.Filename == "" || filepath.Base(p.Filename) != p.Filename || strings.ContainsAny(p.Filename, "/\\\r\n\x00") || len(p.Filename) > 255 || p.MIMEType != "application/pdf" || len(p.SHA256) != 64 {
		return nil, noop, 0, failure("FILE_REJECTED")
	}
	if _, err := hex.DecodeString(p.SHA256); err != nil {
		return nil, noop, 0, failure("FILE_REJECTED")
	}
	var input *os.File
	for _, rootPath := range c.roots {
		relative, err := filepath.Rel(rootPath, filepath.Clean(p.Path))
		if err != nil || relative == ".." || strings.HasPrefix(relative, ".."+string(filepath.Separator)) || filepath.IsAbs(relative) {
			continue
		}
		root, err := os.OpenRoot(rootPath)
		if err != nil {
			continue
		}
		input, err = openUploadFile(root, relative)
		root.Close()
		if err == nil {
			break
		}
	}
	if input == nil {
		return nil, noop, 0, failure("FILE_REJECTED")
	}
	defer input.Close()
	info, err := input.Stat()
	if err != nil || !info.Mode().IsRegular() || info.Size() < 5 || info.Size() > MaxPDF {
		return nil, noop, 0, failure("FILE_REJECTED")
	}
	dir, err := os.MkdirTemp("", "rmchat-pdf-")
	if err != nil {
		return nil, noop, 0, failure("FILE_REJECTED")
	}
	cleanup := func() { os.RemoveAll(dir) }
	snapshot, err := os.OpenFile(filepath.Join(dir, "upload.pdf"), os.O_CREATE|os.O_EXCL|os.O_RDWR, 0600)
	if err != nil {
		cleanup()
		return nil, noop, 0, failure("FILE_REJECTED")
	}
	cleanupAll := func() { snapshot.Close(); cleanup() }
	hash := sha256.New()
	size, err := io.Copy(io.MultiWriter(snapshot, hash), io.LimitReader(contextReader{ctx: ctx, r: input}, MaxPDF+1))
	if err != nil {
		cleanupAll()
		return nil, noop, 0, contextError(ctx)
	}
	if size != info.Size() || size > MaxPDF || hex.EncodeToString(hash.Sum(nil)) != strings.ToLower(p.SHA256) {
		cleanupAll()
		return nil, noop, 0, failure("FILE_CHANGED")
	}
	snapshot.Seek(0, io.SeekStart)
	magic := make([]byte, 5)
	_, err = io.ReadFull(snapshot, magic)
	if err != nil || string(magic) != "%PDF-" {
		cleanupAll()
		return nil, noop, 0, failure("FILE_REJECTED")
	}
	snapshot.Seek(0, io.SeekStart)
	return snapshot, cleanupAll, size, nil
}
func (c *Client) Upload(ctx context.Context, p UploadParams) (any, *Error) {
	s, problem := c.authenticated()
	if problem != nil {
		return nil, problem
	}
	file, cleanup, size, problem := c.snapshot(ctx, p)
	if problem != nil {
		return nil, problem
	}
	defer cleanup()
	payload := map[string]any{"file_name": p.Filename, "file_size": size, "use_case": "my_files", "mime_type": "application/pdf", "store_in_library": true, "library_persistence_mode": "opportunistic"}
	resp, problem := c.request(ctx, http.MethodPost, "/backend-api/files", s, postBody(payload))
	if problem != nil {
		return nil, problem
	}
	var meta struct {
		FileID    string `json:"file_id"`
		UploadURL string `json:"upload_url"`
		LibraryID string `json:"library_file_id"`
	}
	if problem = decodeResponse(resp, &meta, 128<<10); problem != nil {
		return nil, problem
	}
	storageURL, err := url.Parse(meta.UploadURL)
	if err != nil || !validID(meta.FileID) || !c.allowStorage(storageURL) {
		return nil, failure("UPSTREAM_ERROR")
	}
	// A separate request deliberately carries neither account bearer nor cookies.
	req, err := http.NewRequestWithContext(ctx, http.MethodPut, storageURL.String(), file)
	if err != nil {
		return nil, failure("UPSTREAM_ERROR")
	}
	req.ContentLength = size
	req.Header.Set("Content-Type", "application/pdf")
	req.Header.Set("X-Ms-Blob-Type", "BlockBlob")
	req.Header.Set("X-Ms-Version", "2020-04-08")
	resp, err = c.http.Do(req)
	if err != nil {
		return nil, contextError(ctx)
	}
	problem = responseError(resp)
	resp.Body.Close()
	if problem != nil {
		return nil, problem
	}
	resp, problem = c.request(ctx, http.MethodPost, "/backend-api/files/"+url.PathEscape(meta.FileID)+"/uploaded", s, postBody(map[string]any{}))
	if problem != nil {
		return nil, problem
	}
	var confirmation map[string]any
	if problem = decodeResponse(resp, &confirmation, 128<<10); problem != nil {
		return nil, problem
	}
	if confirmation == nil || confirmation["error"] != nil {
		return nil, failure("UPSTREAM_ERROR")
	}
	if ctx.Err() != nil {
		return nil, failure("CANCELLED")
	}
	c.state.mu.Lock()
	defer c.state.mu.Unlock()
	if c.state.session.generation != s.generation {
		return nil, failure("CANCELLED")
	}
	c.state.uploads[meta.FileID] = Attachment{ID: meta.FileID, Name: p.Filename, MIME: p.MIMEType, Size: size, SHA256: strings.ToLower(p.SHA256), LibraryID: meta.LibraryID}
	return map[string]any{"attachmentRef": meta.FileID, "filename": p.Filename, "mimeType": p.MIMEType, "size": size}, nil
}
