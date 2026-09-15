package rmchat

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
	"time"
)

type rpcPeer struct {
	conn   net.Conn
	reader *bufio.Reader
	done   chan struct{}
}

func testPeer(t *testing.T, c *Client) *rpcPeer {
	t.Helper()
	server, client := net.Pipe()
	peer := &rpcPeer{conn: client, reader: bufio.NewReader(client), done: make(chan struct{})}
	go func() { ServeConnection(context.Background(), server, c); close(peer.done) }()
	t.Cleanup(func() {
		client.Close()
		select {
		case <-peer.done:
		case <-time.After(3 * time.Second):
			t.Error("server did not exit on EOF")
		}
	})
	return peer
}
func (p *rpcPeer) send(t *testing.T, id, method string, params any) {
	t.Helper()
	p.raw(t, string(jsonBytes(map[string]any{"jsonrpc": "2.0", "id": id, "method": method, "params": params})))
}
func (p *rpcPeer) raw(t *testing.T, line string) {
	t.Helper()
	p.conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	if _, err := fmt.Fprintln(p.conn, line); err != nil {
		t.Fatal(err)
	}
}
func (p *rpcPeer) read(t *testing.T) map[string]any {
	t.Helper()
	p.conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	line, err := p.reader.ReadBytes('\n')
	if err != nil {
		t.Fatal(err)
	}
	var value map[string]any
	if json.Unmarshal(line, &value) != nil {
		t.Fatalf("bad response %s", line)
	}
	return value
}
func rpcKind(value map[string]any) string {
	e, ok := value["error"].(map[string]any)
	if !ok {
		return ""
	}
	data, ok := e["data"].(map[string]any)
	if !ok {
		return ""
	}
	kind, _ := data["kind"].(string)
	return kind
}
func TestRPCStatusAndStrictProtocol(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { t.Error("status/invalid requests must not use network") })
	peer := testPeer(t, c)
	peer.send(t, "ui:1", "auth.status", map[string]any{})
	value := peer.read(t)
	result := value["result"].(map[string]any)
	capabilities := result["capabilities"].(map[string]any)
	if result["authenticated"] != false || result["account"] != nil || capabilities["protocolVersion"] != float64(1) || capabilities["provider"] != "chatgpt-web" {
		t.Fatalf("wrong status %+v", value)
	}
	peer.send(t, "ui:1", "auth.status", map[string]any{})
	if rpcKind(peer.read(t)) != "DUPLICATE_REQUEST" {
		t.Fatal("duplicate ID accepted")
	}
	peer.raw(t, `{"jsonrpc":"2.0","id":"ui:2","id":"ui:3","method":"auth.status"}`)
	if peer.read(t)["error"].(map[string]any)["code"] != float64(-32600) {
		t.Fatal("duplicate JSON key accepted")
	}
	peer.raw(t, `{"jsonrpc":`)
	if peer.read(t)["error"].(map[string]any)["code"] != float64(-32700) {
		t.Fatal("wrong parse error")
	}
	peer.send(t, "ui:4", "unknown.method", map[string]any{})
	if peer.read(t)["error"].(map[string]any)["code"] != float64(-32601) {
		t.Fatal("wrong method error")
	}
	peer.send(t, "ui:5", "auth.import", map[string]any{"credential": map[string]any{"version": 1, "provider": "chatgpt-web", "kind": "access_token", "value": "secret", "extra": "refused"}})
	if rpcKind(peer.read(t)) != "INVALID_PARAMS" {
		t.Fatal("unknown credential field accepted")
	}
}
func TestRPCCancelDuringAuthPreservesSessionAndRemainsResponsive(t *testing.T) {
	started := make(chan struct{})
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { close(started); <-r.Context().Done() })
	setSession(c, "old")
	peer := testPeer(t, c)
	peer.send(t, "ui:1", "auth.import", map[string]any{"credential": credential("candidate")})
	<-started
	peer.send(t, "ui:2", "models.list", map[string]any{})
	if rpcKind(peer.read(t)) != "BUSY" {
		t.Fatal("concurrent incompatible operation accepted")
	}
	peer.send(t, "ui:3", "auth.status", map[string]any{})
	if peer.read(t)["result"].(map[string]any)["authenticated"] != true {
		t.Fatal("status blocked or previous session lost")
	}
	peer.send(t, "ui:4", "request.cancel", map[string]any{"requestId": "ui:1"})
	responses := []map[string]any{peer.read(t), peer.read(t)}
	var cancelled, accepted bool
	for _, value := range responses {
		if value["id"] == "ui:1" {
			cancelled = rpcKind(value) == "CANCELLED"
		}
		if value["id"] == "ui:4" {
			accepted = value["result"].(map[string]any)["accepted"] == true
		}
	}
	if !cancelled || !accepted || c.state.current().token != "old" {
		t.Fatalf("cancellation incorrect %+v", responses)
	}
}
func TestRPCEOFInterruptsNetworkAndClearsSecrets(t *testing.T) {
	started := make(chan struct{})
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { close(started); <-r.Context().Done() })
	setSession(c, "old")
	peer := testPeer(t, c)
	peer.send(t, "ui:1", "auth.import", map[string]any{"credential": credential("candidate")})
	<-started
	peer.conn.Close()
	select {
	case <-peer.done:
	case <-time.After(3 * time.Second):
		t.Fatal("EOF did not cancel network")
	}
	if c.Status().Authenticated {
		t.Fatal("credentials survived connection lifetime")
	}
}
func TestRPCOversizedResultIsExplicitError(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": "remote", "title": strings.Repeat("x", MaxFrame)}}})
	})
	setSession(c, "secret")
	peer := testPeer(t, c)
	peer.send(t, "ui:1", "models.list", map[string]any{})
	if rpcKind(peer.read(t)) != "RESPONSE_TOO_LARGE" {
		t.Fatal("oversized response not rejected")
	}
}
func TestRPCOversizedInputClosesBoundedConnection(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { t.Error("oversize reached network") })
	peer := testPeer(t, c)
	peer.conn.SetWriteDeadline(time.Now().Add(3 * time.Second))
	fmt.Fprint(peer.conn, strings.Repeat("x", MaxFrame+2))
	select {
	case <-peer.done:
	case <-time.After(3 * time.Second):
		t.Fatal("oversized input did not close")
	}
}
func TestUnixSocketPrivateSingleClientAndEOF(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Linux socket permission contract is checked on Linux ARM64")
	}
	dir, err := os.MkdirTemp("", "rmchat-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "core.sock")
	c, _ := NewClient(nil)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	done := make(chan error, 1)
	go func() { done <- Serve(ctx, path, c) }()
	deadline := time.Now().Add(3 * time.Second)
	for {
		if info, err := os.Stat(path); err == nil {
			if info.Mode().Perm() != 0600 {
				t.Fatalf("socket mode %v", info.Mode())
			}
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("socket missing")
		}
		time.Sleep(5 * time.Millisecond)
	}
	connection, err := net.Dial("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	connection.Close()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("did not exit after EOF")
	}
	if _, err = os.Lstat(path); !os.IsNotExist(err) {
		t.Fatal("socket remained after connection closed")
	}
}
func TestUnixSocketNeverReplacesExistingEndpoint(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("Linux-only directory modes")
	}
	dir, err := os.MkdirTemp("", "rmchat-")
	if err != nil {
		t.Fatal(err)
	}
	defer os.RemoveAll(dir)
	path := filepath.Join(dir, "core.sock")
	os.WriteFile(path, []byte("preserve"), 0600)
	c, _ := NewClient(nil)
	if Serve(context.Background(), path, c) == nil {
		t.Fatal("existing endpoint accepted")
	}
	data, _ := os.ReadFile(path)
	if string(data) != "preserve" {
		t.Fatal("existing endpoint changed")
	}
}
