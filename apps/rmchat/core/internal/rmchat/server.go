package rmchat

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net"
	"os"
	"path/filepath"
	"regexp"
	"sync"
	"time"
	"unicode/utf8"
)

type rpcRequest struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      string          `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
}
type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
	Data    any    `json:"data,omitempty"`
}
type rpcResponse struct {
	JSONRPC string    `json:"jsonrpc"`
	ID      any       `json:"id"`
	Result  any       `json:"result,omitempty"`
	Error   *rpcError `json:"error,omitempty"`
}

var requestIDPattern = regexp.MustCompile(`^ui:[0-9]{1,18}$`)

func strictDecode(data []byte, target any) error {
	if !uniqueJSON(data) {
		return io.ErrUnexpectedEOF
	}
	d := json.NewDecoder(bytes.NewReader(data))
	d.DisallowUnknownFields()
	if err := d.Decode(target); err != nil {
		return err
	}
	var extra any
	if err := d.Decode(&extra); err != io.EOF {
		return io.ErrUnexpectedEOF
	}
	return nil
}

func uniqueJSON(data []byte) bool {
	d := json.NewDecoder(bytes.NewReader(data))
	var walk func(int) bool
	walk = func(depth int) bool {
		if depth > 32 {
			return false
		}
		token, err := d.Token()
		if err != nil {
			return false
		}
		delim, ok := token.(json.Delim)
		if !ok {
			return true
		}
		switch delim {
		case '{':
			seen := map[string]bool{}
			for d.More() {
				key, err := d.Token()
				if err != nil {
					return false
				}
				name, ok := key.(string)
				if !ok || seen[name] {
					return false
				}
				seen[name] = true
				if !walk(depth + 1) {
					return false
				}
			}
			end, err := d.Token()
			return err == nil && end == json.Delim('}')
		case '[':
			for d.More() {
				if !walk(depth + 1) {
					return false
				}
			}
			end, err := d.Token()
			return err == nil && end == json.Delim(']')
		}
		return false
	}
	if !walk(0) {
		return false
	}
	_, err := d.Token()
	return err == io.EOF
}
func params(raw json.RawMessage, target any) *Error {
	raw = bytes.TrimSpace(raw)
	if len(raw) == 0 {
		raw = []byte("{}")
	}
	if len(raw) == 0 || raw[0] != '{' || strictDecode(raw, target) != nil {
		return failure("INVALID_PARAMS")
	}
	return nil
}

// Serve creates only a new socket inside the parent's private directory. Existing
// endpoints, including stale sockets, are never removed or replaced.
func Serve(ctx context.Context, path string, client *Client) error {
	if !filepath.IsAbs(path) {
		return failure("INVALID_PARAMS")
	}
	dir := filepath.Dir(path)
	info, err := os.Lstat(dir)
	if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 || info.Mode().Perm()&0077 != 0 {
		return failure("INVALID_PARAMS")
	}
	resolved, err := filepath.EvalSymlinks(dir)
	if err != nil || filepath.Clean(resolved) != filepath.Clean(dir) {
		return failure("INVALID_PARAMS")
	}
	if _, err = os.Lstat(path); !os.IsNotExist(err) {
		return failure("INVALID_PARAMS")
	}
	listener, err := net.ListenUnix("unix", &net.UnixAddr{Name: path, Net: "unix"})
	if err != nil {
		return failure("NETWORK_ERROR")
	}
	defer listener.Close()
	if err = os.Chmod(path, 0600); err != nil {
		return failure("NETWORK_ERROR")
	}
	listener.SetDeadline(time.Now().Add(10 * time.Second))
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		select {
		case <-ctx.Done():
			listener.Close()
		case <-stop:
		}
	}()
	conn, err := listener.AcceptUnix()
	if err != nil {
		return failure("NETWORK_ERROR")
	}
	listener.Close()
	defer conn.Close()
	ServeConnection(ctx, conn, client)
	return nil
}

// ServeConnection processes one authenticated local connection and returns at
// EOF. A second request can cancel the current network operation immediately.
func ServeConnection(parent context.Context, conn net.Conn, client *Client) {
	ctx, cancelAll := context.WithCancel(parent)
	defer cancelAll()
	defer client.Logout()
	var writeMu sync.Mutex
	write := func(value any) bool {
		data, err := json.Marshal(value)
		if err != nil {
			return false
		}
		if len(data) > MaxFrame {
			return false
		}
		writeMu.Lock()
		defer writeMu.Unlock()
		conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
		_, err = conn.Write(append(data, '\n'))
		return err == nil
	}
	respond := func(id any, result any, problem *Error) {
		response := rpcResponse{JSONRPC: "2.0", ID: id, Result: result}
		if problem != nil {
			code := -32000
			if problem.Code == "INVALID_PARAMS" {
				code = -32602
			}
			if problem.Code == "METHOD_NOT_FOUND" {
				code = -32601
			}
			response.Result = nil
			data := map[string]any{"kind": problem.Code, "retryable": problem.Retryable}
			if problem.Uncertain {
				data["outcomeUnknown"] = true
			}
			switch problem.Stage {
			case "validation", "prepare", "submit", "stream":
				data["stage"] = problem.Stage
			}
			if problem.HTTP != nil {
				data["httpStatus"] = problem.HTTP.Status
				data["contentType"] = problem.HTTP.ContentType
				data["challenge"] = problem.HTTP.Challenge
			}
			response.Error = &rpcError{Code: code, Message: problem.Message, Data: data}
		}
		if len(jsonBytes(response)) > MaxFrame {
			response.Result = nil
			response.Error = &rpcError{Code: -32000, Message: failure("RESPONSE_TOO_LARGE").Message, Data: map[string]any{"kind": "RESPONSE_TOO_LARGE", "retryable": false}}
		}
		if !write(response) {
			cancelAll()
			conn.Close()
		}
	}
	var mu sync.Mutex
	activeID := ""
	var activeCancel context.CancelFunc
	var workers sync.WaitGroup
	seen := map[string]bool{}
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		select {
		case <-ctx.Done():
			conn.Close()
		case <-stop:
		}
	}()
	scanner := bufio.NewScanner(conn)
	scanner.Buffer(make([]byte, 4096), MaxFrame+1)
	for scanner.Scan() {
		line := scanner.Bytes()
		var req rpcRequest
		if !json.Valid(line) {
			write(rpcResponse{JSONRPC: "2.0", ID: nil, Error: &rpcError{Code: -32700, Message: "JSON invalide."}})
			continue
		}
		if len(line) > MaxFrame || !utf8.Valid(line) || strictDecode(line, &req) != nil || req.JSONRPC != "2.0" || !requestIDPattern.MatchString(req.ID) || req.Method == "" {
			write(rpcResponse{JSONRPC: "2.0", ID: nil, Error: &rpcError{Code: -32600, Message: "Requête JSON-RPC invalide."}})
			continue
		}
		if seen[req.ID] {
			respond(req.ID, nil, failure("DUPLICATE_REQUEST"))
			continue
		}
		seen[req.ID] = true
		if len(seen) > 10000 {
			respond(req.ID, nil, failure("BUSY"))
			break
		}
		if req.Method == "request.cancel" {
			var p struct {
				RequestID string `json:"requestId"`
			}
			if problem := params(req.Params, &p); problem != nil || !requestIDPattern.MatchString(p.RequestID) {
				respond(req.ID, nil, failure("INVALID_PARAMS"))
				continue
			}
			mu.Lock()
			accepted := activeID == p.RequestID && activeCancel != nil
			if accepted {
				activeCancel()
			}
			mu.Unlock()
			respond(req.ID, map[string]any{"requestId": p.RequestID, "accepted": accepted}, nil)
			continue
		}
		if req.Method == "auth.status" {
			if problem := params(req.Params, &struct{}{}); problem != nil {
				respond(req.ID, nil, problem)
			} else {
				respond(req.ID, client.Status(), nil)
			}
			continue
		}
		if req.Method == "auth.logout" {
			if problem := params(req.Params, &struct{}{}); problem != nil {
				respond(req.ID, nil, problem)
				continue
			}
			mu.Lock()
			if activeCancel != nil {
				activeCancel()
			}
			client.Logout()
			mu.Unlock()
			respond(req.ID, map[string]any{"authenticated": false}, nil)
			continue
		}
		mu.Lock()
		if activeID != "" {
			mu.Unlock()
			respond(req.ID, nil, failure("BUSY"))
			continue
		}
		operationCtx, operationCancel := context.WithCancel(ctx)
		activeID = req.ID
		activeCancel = operationCancel
		mu.Unlock()
		// Scanner memory belongs to the reader; a concurrent worker owns its copy.
		req.Params = append(json.RawMessage(nil), req.Params...)
		workers.Add(1)
		go func(req rpcRequest) {
			defer workers.Done()
			defer operationCancel()
			emit := func(p Progress) {
				if operationCtx.Err() != nil {
					return
				}
				p.RequestID = req.ID
				if !write(map[string]any{"jsonrpc": "2.0", "method": "chat.progress", "params": p}) {
					cancelAll()
					conn.Close()
				}
			}
			result, problem := client.dispatch(operationCtx, req.Method, req.Params, emit)
			if operationCtx.Err() != nil && (problem == nil || problem.Code != "CANCELLED") {
				uncertain := req.Method == "chat.send"
				problem = failure("CANCELLED")
				problem.Uncertain = uncertain
				result = nil
			}
			mu.Lock()
			if activeID == req.ID {
				activeID = ""
				activeCancel = nil
			}
			mu.Unlock()
			if ctx.Err() == nil {
				respond(req.ID, result, problem)
			}
		}(req)
	}
	cancelAll()
	conn.Close()
	workers.Wait()
}
func (c *Client) dispatch(ctx context.Context, method string, raw json.RawMessage, emit func(Progress)) (any, *Error) {
	switch method {
	case "auth.import":
		var p struct {
			Credential     Credential `json:"credential"`
			PersistSession bool       `json:"persistSession"`
		}
		if e := params(raw, &p); e != nil {
			return nil, e
		}
		return c.Import(ctx, p.Credential, p.PersistSession)
	case "models.list":
		if e := params(raw, &struct{}{}); e != nil {
			return nil, e
		}
		return c.Models(ctx)
	case "models.inspect":
		if !c.inspectModels {
			return nil, failure("METHOD_NOT_FOUND")
		}
		if e := params(raw, &struct{}{}); e != nil {
			return nil, e
		}
		return c.InspectModels(ctx)
	case "conversations.list":
		var p struct {
			Cursor string `json:"cursor"`
			Limit  int    `json:"limit"`
		}
		if e := params(raw, &p); e != nil {
			return nil, e
		}
		return c.ListConversations(ctx, p.Cursor, p.Limit)
	case "conversations.get":
		var p struct {
			ConversationID string `json:"conversationId"`
			Cursor         string `json:"cursor"`
			Limit          int    `json:"limit"`
		}
		if e := params(raw, &p); e != nil {
			return nil, e
		}
		return c.GetConversation(ctx, p.ConversationID, p.Cursor, p.Limit)
	case "chat.send":
		var p SendParams
		if e := params(raw, &p); e != nil {
			return nil, e
		}
		return c.Send(ctx, p, emit)
	case "files.upload":
		var p UploadParams
		if e := params(raw, &p); e != nil {
			return nil, e
		}
		return c.Upload(ctx, p)
	default:
		return nil, failure("METHOD_NOT_FOUND")
	}
}
