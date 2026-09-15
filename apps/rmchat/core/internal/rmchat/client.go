package rmchat

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"mime"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"time"
)

type Client struct {
	// http is the cookie-free template, also used for isolated signed storage PUTs.
	http          *http.Client
	base          string
	roots         []string
	state         state
	allowStorage  func(*url.URL) bool
	inspectModels bool
}

func NewClient(uploadRoots []string) (*Client, error) {
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.MaxIdleConns = 4
	transport.MaxIdleConnsPerHost = 2
	transport.ResponseHeaderTimeout = 30 * time.Second
	c := &Client{http: &http.Client{Transport: transport, Timeout: 10 * time.Minute, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}, base: "https://chatgpt.com"}
	for _, root := range uploadRoots {
		if !filepath.IsAbs(root) {
			return nil, failure("INVALID_PARAMS")
		}
		absolute, err := filepath.EvalSymlinks(root)
		if err != nil {
			return nil, failure("FILE_REJECTED")
		}
		info, err := os.Stat(absolute)
		if err != nil || !info.IsDir() {
			return nil, failure("FILE_REJECTED")
		}
		c.roots = append(c.roots, absolute)
	}
	c.allowStorage = func(u *url.URL) bool {
		return u.Scheme == "https" && u.User == nil && u.Port() == "" && (strings.HasSuffix(u.Hostname(), ".blob.core.windows.net") || u.Hostname() == "files.oaiusercontent.com" || strings.HasSuffix(u.Hostname(), ".oaiusercontent.com"))
	}
	c.state.clear()
	return c, nil
}
func (c *Client) capabilities() Capabilities {
	methods := []string{"auth.status", "auth.import", "auth.logout", "request.cancel", "models.list", "conversations.list", "conversations.get", "chat.send"}
	if c.inspectModels {
		methods = append(methods, "models.inspect")
	}
	if len(c.roots) > 0 {
		methods = append(methods, "files.upload")
	}
	return Capabilities{ProtocolVersion: 1, Provider: "chatgpt-web", CredentialKinds: []string{"access_token", "session_token"}, Methods: methods}
}
func (c *Client) Status() AuthStatus {
	s := c.state.current()
	out := AuthStatus{Authenticated: s.token != "", Capabilities: c.capabilities()}
	if out.Authenticated && (s.account.ID != "" || s.account.DisplayName != nil) {
		out.Account = &s.account
	}
	return out
}
func (c *Client) Logout() { c.state.clear() }
func (c *Client) request(ctx context.Context, method, path string, s session, body io.Reader) (*http.Response, *Error) {
	req, err := http.NewRequestWithContext(ctx, method, c.base+path, body)
	if err != nil {
		return nil, failure("INVALID_PARAMS")
	}
	req.Header.Set("Accept", "application/json")
	req.Header.Set("User-Agent", "reMoodle-rmchat/0.1")
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	if s.token != "" {
		req.Header.Set("Authorization", "Bearer "+s.token)
	}
	resp, err := s.http.Do(req)
	if err != nil {
		return nil, contextError(ctx)
	}
	if problem := responseError(resp); problem != nil {
		resp.Body.Close()
		return nil, problem
	}
	return resp, nil
}
func responseError(resp *http.Response) *Error {
	if strings.EqualFold(strings.TrimSpace(resp.Header.Get("Cf-Mitigated")), "challenge") {
		return httpFailure(resp, "WEB_AUTH_REQUIRED", true)
	}
	// Only exact machine codes may refine a rejected request. Never publish or
	// guess a cause from a free-form upstream message, header, or response body.
	if (resp.StatusCode == 400 || resp.StatusCode == 403 || resp.StatusCode == 404) && normalizedContentType(resp.Header.Get("Content-Type")) == "application/json" && resp.Body != nil {
		body, err := io.ReadAll(io.LimitReader(resp.Body, (128<<10)+1))
		if err == nil && len(body) <= 128<<10 {
			var object map[string]any
			if json.Unmarshal(body, &object) == nil {
				if code := explicitRemoteError(object); code == "MODEL_UNAVAILABLE" || code == "RATE_LIMITED" {
					return httpFailure(resp, code, false)
				}
			}
		}
	}
	switch resp.StatusCode {
	case 401:
		return httpFailure(resp, "AUTH_INVALID", false)
	case 403:
		return httpFailure(resp, "UPSTREAM_FORBIDDEN", false)
	case 404:
		return httpFailure(resp, "NOT_FOUND", false)
	case 429:
		return httpFailure(resp, "RATE_LIMITED", false)
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return httpFailure(resp, "UPSTREAM_ERROR", false)
	}
	contentType := normalizedContentType(resp.Header.Get("Content-Type"))
	if contentType == "text/html" || contentType == "application/xhtml+xml" {
		return httpFailure(resp, "UNEXPECTED_HTML", false)
	}
	return nil
}

func normalizedContentType(header string) string {
	if strings.TrimSpace(header) == "" {
		return "unknown"
	}
	mediaType, _, err := mime.ParseMediaType(header)
	if err != nil {
		return "other"
	}
	switch strings.ToLower(mediaType) {
	case "application/json", "text/html", "application/xhtml+xml", "text/event-stream":
		return strings.ToLower(mediaType)
	}
	return "other"
}

func httpFailure(resp *http.Response, code string, challenge bool) *Error {
	e := failure(code)
	e.HTTP = &HTTPDiagnostics{Status: resp.StatusCode, ContentType: normalizedContentType(resp.Header.Get("Content-Type")), Challenge: challenge}
	return e
}

func explicitChallenge(value any) bool {
	if required, ok := value.(bool); ok {
		return required
	}
	if object, ok := value.(map[string]any); ok {
		required, _ := object["required"].(bool)
		return required
	}
	return false
}
func decodeResponse(resp *http.Response, target any, limit int64) *Error {
	defer resp.Body.Close()
	data, err := io.ReadAll(io.LimitReader(resp.Body, limit+1))
	if err != nil {
		return httpFailure(resp, "NETWORK_ERROR", false)
	}
	if int64(len(data)) > limit {
		return httpFailure(resp, "RESPONSE_TOO_LARGE", false)
	}
	// Even a HTTP 200 response can be an explicit browser challenge. No challenge is executed.
	var marker struct {
		Challenge   any `json:"challenge"`
		Turnstile   any `json:"turnstile"`
		ProofOfWork any `json:"proofofwork"`
	}
	if json.Unmarshal(data, &marker) == nil && (explicitChallenge(marker.Challenge) || explicitChallenge(marker.Turnstile) || explicitChallenge(marker.ProofOfWork)) {
		return httpFailure(resp, "WEB_AUTH_REQUIRED", true)
	}
	var object map[string]any
	if json.Unmarshal(data, &object) == nil {
		if code := explicitRemoteError(object); code != "" {
			return httpFailure(resp, code, false)
		}
	}
	if json.Unmarshal(data, target) != nil {
		return httpFailure(resp, "UPSTREAM_ERROR", false)
	}
	return nil
}
func (c *Client) authenticated() (session, *Error) {
	s := c.state.current()
	if s.token == "" {
		return s, failure("AUTH_REQUIRED")
	}
	return s, nil
}
func (c *Client) Import(ctx context.Context, credential Credential, persistSession bool) (AuthImportResult, *Error) {
	if credential.Kind != "access_token" && credential.Kind != "session_token" {
		return AuthImportResult{}, failure("UNSUPPORTED_CREDENTIAL_KIND")
	}
	if credential.Version != 1 || credential.Provider != "chatgpt-web" || len(credential.Value) == 0 || len(jsonBytes(credential)) > 32768 || strings.ContainsAny(credential.Value, "\r\n\x00") {
		return AuthImportResult{}, failure("INVALID_PARAMS")
	}
	if credential.Kind == "session_token" {
		if !validSessionValue(credential.Value) {
			return AuthImportResult{}, failure("INVALID_PARAMS")
		}
		// A session exchange can rotate its credential even when the caller only
		// intended to inspect authentication. Require a persistence-capable parent.
		if !persistSession {
			return AuthImportResult{}, failure("SESSION_PERSISTENCE_REQUIRED")
		}
	}
	c.state.mu.Lock()
	if c.state.authBusy {
		c.state.mu.Unlock()
		return AuthImportResult{}, failure("BUSY")
	}
	c.state.authBusy = true
	generation := c.state.generation
	c.state.mu.Unlock()
	defer func() { c.state.mu.Lock(); c.state.authBusy = false; c.state.mu.Unlock() }()
	candidate := c.newSession(credential.Value)
	activated := false
	defer func() {
		if !activated {
			candidate.close()
		}
	}()
	token := credential.Value
	var account Account
	var update *Credential
	if credential.Kind == "session_token" {
		req, _ := http.NewRequestWithContext(ctx, http.MethodGet, c.base+"/api/auth/session", nil)
		req.Header.Set("Accept", "application/json")
		req.Header.Set("User-Agent", "reMoodle-rmchat/0.1")
		req.AddCookie(&http.Cookie{Name: sessionCookieName, Value: token})
		resp, err := candidate.http.Do(req)
		if err != nil {
			return AuthImportResult{}, contextError(ctx)
		}
		if problem := responseError(resp); problem != nil {
			resp.Body.Close()
			return AuthImportResult{}, problem
		}
		var result struct {
			AccessToken string `json:"accessToken"`
			User        struct {
				ID   string `json:"id"`
				Name string `json:"name"`
			} `json:"user"`
		}
		if problem := decodeResponse(resp, &result, 128<<10); problem != nil {
			return AuthImportResult{}, problem
		}
		if result.AccessToken == "" || len(result.AccessToken) > 32768 || strings.ContainsAny(result.AccessToken, "\r\n\x00") {
			return AuthImportResult{}, httpFailure(resp, "AUTH_INVALID", false)
		}
		rotated, problem := sessionCredentialFromResponse(resp, credential)
		if problem != nil {
			return AuthImportResult{}, problem
		}
		update = &rotated
		token = result.AccessToken
		account = Account{ID: result.User.ID, DisplayName: optional(result.User.Name)}
	} else {
		// An access token alone is validated against /models. A session exchange
		// already returned authenticated JSON: save its rotation before any later
		// request such as /models can fail independently.
		resp, problem := c.request(ctx, http.MethodGet, "/backend-api/models", candidate, nil)
		if problem != nil {
			return AuthImportResult{}, problem
		}
		var models struct {
			Models []json.RawMessage `json:"models"`
		}
		if problem = decodeResponse(resp, &models, 4<<20); problem != nil {
			return AuthImportResult{}, problem
		}
		if models.Models == nil {
			return AuthImportResult{}, httpFailure(resp, "UPSTREAM_ERROR", false)
		}
	}
	if ctx.Err() != nil {
		return AuthImportResult{}, failure("CANCELLED")
	}
	c.state.mu.Lock()
	if c.state.generation != generation {
		c.state.mu.Unlock()
		return AuthImportResult{}, failure("CANCELLED")
	}
	c.state.generation++
	c.state.session.close()
	candidate.token = token
	candidate.account = account
	candidate.generation = c.state.generation
	c.state.session = candidate
	activated = true
	c.state.uploads = map[string]Attachment{}
	c.state.sent = map[string]bool{}
	c.state.models = nil
	c.state.mu.Unlock()
	return AuthImportResult{AuthStatus: c.Status(), CredentialUpdate: update}, nil
}
func (c *Client) Models(ctx context.Context) (any, *Error) {
	s, problem := c.authenticated()
	if problem != nil {
		return nil, problem
	}
	resp, problem := c.request(ctx, http.MethodGet, "/backend-api/models", s, nil)
	if problem != nil {
		return nil, problem
	}
	var result struct {
		Models []map[string]any `json:"models"`
	}
	if problem = decodeResponse(resp, &result, 4<<20); problem != nil {
		return nil, problem
	}
	if result.Models == nil {
		return nil, httpFailure(resp, "UPSTREAM_ERROR", false)
	}
	candidates := []Model{}
	seen := map[string]bool{}
	denied := map[string]bool{}
	for _, model := range result.Models {
		slug, _ := model["slug"].(string)
		id := strings.TrimSpace(slug)
		if !validModelID(id) {
			continue
		}
		if modelExplicitlyDenied(model) {
			denied[id] = true
		}
		if !seen[id] {
			seen[id] = true
			title, _ := model["title"].(string)
			name := strings.TrimSpace(title)
			if name == "" {
				name = id
			}
			candidates = append(candidates, Model{ID: id, Name: name})
		}
	}
	out := []Model{}
	allowed := map[string]bool{}
	for _, model := range candidates {
		if !denied[model.ID] {
			out = append(out, model)
			allowed[model.ID] = true
		}
	}
	c.state.mu.Lock()
	defer c.state.mu.Unlock()
	if ctx.Err() != nil || c.state.session.generation != s.generation {
		return nil, failure("CANCELLED")
	}
	c.state.models = allowed
	return map[string]any{"items": out, "defaultModelId": nil}, nil
}
func postBody(v any) io.Reader { return bytes.NewReader(jsonBytes(v)) }
