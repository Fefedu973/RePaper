package rmchat

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func testClient(t *testing.T, handler http.HandlerFunc, roots ...string) *Client {
	t.Helper()
	server := httptest.NewServer(handler)
	t.Cleanup(server.Close)
	client, err := NewClient(roots)
	if err != nil {
		t.Fatal(err)
	}
	client.base = server.URL
	client.http = server.Client()
	client.http.CheckRedirect = func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }
	return client
}
func credential(value string) Credential {
	return Credential{Version: 1, Provider: "chatgpt-web", Kind: "access_token", Value: value}
}
func setSession(c *Client, token string) {
	c.state.mu.Lock()
	defer c.state.mu.Unlock()
	c.state.generation++
	c.state.session.close()
	c.state.session = c.newSession(token)
	c.state.session.generation = c.state.generation
}
func requireCode(t *testing.T, e *Error, code string) {
	t.Helper()
	if e == nil || e.Code != code {
		t.Fatalf("error=%v, want %s", e, code)
	}
}
func jsonResponse(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(v)
}

func TestAuthImportValidatesBeforeReplacingSession(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if r.URL.Path != "/backend-api/models" {
			t.Errorf("unexpected endpoint %s", r.URL.Path)
		}
		if r.Header.Get("Authorization") == "Bearer rejected-secret" {
			w.WriteHeader(401)
			fmt.Fprint(w, "rejected-secret")
			return
		}
		jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": "available-model"}}})
	})
	status, e := c.Import(context.Background(), credential("accepted-secret"), false)
	if e != nil || !status.Authenticated || status.Account != nil {
		t.Fatalf("import status=%+v error=%v", status, e)
	}
	_, e = c.Import(context.Background(), credential("rejected-secret"), false)
	requireCode(t, e, "AUTH_INVALID")
	if c.state.current().token != "accepted-secret" || calls.Load() != 2 {
		t.Fatal("failed candidate changed active session")
	}
	if strings.Contains(string(jsonBytes(status)), "secret") || strings.Contains(string(jsonBytes(e)), "secret") {
		t.Fatal("credential leaked")
	}
}
func TestSessionImportUsesCookieAndCamelCaseToken(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/auth/session":
			cookie, err := r.Cookie("__Secure-next-auth.session-token")
			if err != nil || cookie.Value != "supplied-session" || r.Header.Get("Authorization") != "" {
				t.Error("wrong session authentication")
			}
			jsonResponse(w, map[string]any{"accessToken": "derived-access", "user": map[string]any{"id": "user-1", "name": "Élodie"}})
		case "/backend-api/models":
			if r.Header.Get("Authorization") != "Bearer derived-access" || r.Header.Get("Cookie") != "" {
				t.Error("wrong models authentication")
			}
			jsonResponse(w, map[string]any{"models": []any{}})
		default:
			t.Errorf("unexpected %s", r.URL.Path)
		}
	})
	value := credential("supplied-session")
	value.Kind = "session_token"
	status, e := c.Import(context.Background(), value, true)
	if e != nil || status.Account == nil || status.Account.ID != "user-1" || *status.Account.DisplayName != "Élodie" {
		t.Fatalf("%+v %v", status, e)
	}
	if strings.Contains(string(jsonBytes(status)), "derived-access") {
		t.Fatal("access token leaked")
	}
}

func TestSessionRotationReturnsPrivateCredentialAndReplacesSession(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		http.SetCookie(w, &http.Cookie{Name: "__Secure-next-auth.session-token", Value: "rotated-private-cookie"})
		jsonResponse(w, map[string]any{"accessToken": "derived-private-access"})
	})
	setSession(c, "previous")
	candidate := credential("original-private-cookie")
	candidate.Kind = "session_token"
	result, e := c.Import(context.Background(), candidate, true)
	if e != nil || result.CredentialUpdate == nil || result.CredentialUpdate.Value != "rotated-private-cookie" || result.CredentialUpdate.Kind != "session_token" {
		t.Fatal("rotation did not return the private credential update")
	}
	if c.state.current().token != "derived-private-access" || calls.Load() != 1 || strings.Contains(string(jsonBytes(c.Status())), "private") {
		t.Fatal("rotation used extra requests or leaked a secret in status")
	}
}
func TestAuthRejectsMalformedAndUnsupportedWithoutNetwork(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { calls.Add(1) })
	for _, candidate := range []Credential{{Version: 1, Provider: "chatgpt-web", Kind: "refresh_token", Value: "x"}, credential("bad\r\nheader"), credential(strings.Repeat("x", 32768)), credential("")} {
		if _, e := c.Import(context.Background(), candidate, false); e == nil {
			t.Fatal("invalid credential accepted")
		}
	}
	if calls.Load() != 0 {
		t.Fatal("invalid credential caused network access")
	}
}
func TestAuthRejectsHTMLChallengeAndInvalidModels(t *testing.T) {
	for _, tc := range []struct {
		name                    string
		status                  int
		contentType, body, code string
	}{{"html", 200, "text/html", "<html>secret</html>", "UNEXPECTED_HTML"}, {"forbidden", 403, "application/json", `{"detail":"secret"}`, "UPSTREAM_FORBIDDEN"}, {"jsonchallenge", 200, "application/json", `{"challenge":{"required":true}}`, "WEB_AUTH_REQUIRED"}, {"missingmodels", 200, "application/json", `{}`, "UPSTREAM_ERROR"}, {"ratelimit", 429, "application/json", `{}`, "RATE_LIMITED"}} {
		t.Run(tc.name, func(t *testing.T) {
			c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", tc.contentType)
				w.WriteHeader(tc.status)
				fmt.Fprint(w, tc.body)
			})
			_, e := c.Import(context.Background(), credential("secret"), false)
			requireCode(t, e, tc.code)
			if c.Status().Authenticated || strings.Contains(string(jsonBytes(e)), "secret") {
				t.Fatal("bad auth state or raw payload leaked")
			}
		})
	}
}
func TestAuthCancellationPreservesPreviousSession(t *testing.T) {
	started := make(chan struct{})
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { close(started); <-r.Context().Done() })
	setSession(c, "old")
	previous := c.state.current()
	u, _ := url.Parse(c.base)
	previous.cookies.SetCookies(u, []*http.Cookie{{Name: "continuity", Value: "old-fixture", Path: "/"}})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan *Error, 1)
	go func() { _, e := c.Import(ctx, credential("new"), false); done <- e }()
	<-started
	cancel()
	select {
	case e := <-done:
		requireCode(t, e, "CANCELLED")
	case <-time.After(2 * time.Second):
		t.Fatal("cancel did not interrupt request")
	}
	remaining := c.state.current()
	cookies := remaining.cookies.Cookies(u)
	if remaining.token != "old" || remaining.cookies != previous.cookies || len(cookies) != 1 || cookies[0].Value != "old-fixture" {
		t.Fatal("cancel changed session")
	}
}
func TestModelsAreActualRemoteModels(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": "only-remote", "title": "Modèle disponible"}}})
	})
	setSession(c, "secret")
	result, e := c.Models(context.Background())
	if e != nil {
		t.Fatal(e)
	}
	data := string(jsonBytes(result))
	if !strings.Contains(data, `"items":[{"id":"only-remote","name":"Modèle disponible"}]`) || !strings.Contains(data, `"defaultModelId":null`) {
		t.Fatalf("unexpected models %s", data)
	}
}
func TestRedirectDoesNotForwardCredential(t *testing.T) {
	var leaked atomic.Int32
	destination := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { leaked.Add(1) }))
	defer destination.Close()
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, destination.URL, 302) })
	_, e := c.Import(context.Background(), credential("secret"), false)
	requireCode(t, e, "UPSTREAM_ERROR")
	if leaked.Load() != 0 {
		t.Fatal("redirect followed")
	}
}
func TestListConversationsUsesOpaquePagination(t *testing.T) {
	var offset string
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		offset = r.URL.Query().Get("offset")
		jsonResponse(w, map[string]any{"items": []any{map[string]any{"id": "conversation-1", "title": "Cours", "update_time": "2026-09-06T10:00:00Z"}}, "total": 2})
	})
	setSession(c, "secret")
	first, e := c.ListConversations(context.Background(), "", 1)
	if e != nil {
		t.Fatal(e)
	}
	page := first.(map[string]any)
	next := page["nextCursor"].(*string)
	if next == nil || *next == "1" || offset != "0" {
		t.Fatal("invalid cursor")
	}
	_, e = c.ListConversations(context.Background(), *next, 1)
	if e != nil || offset != "1" {
		t.Fatalf("next page failed %v %s", e, offset)
	}
	_, e = c.ListConversations(context.Background(), "../outside", 1)
	requireCode(t, e, "INVALID_PARAMS")
}
func conversationFixture() map[string]any {
	message := func(id, role, text string) map[string]any {
		return map[string]any{"id": id, "author": map[string]any{"role": role}, "content": map[string]any{"parts": []any{text}}}
	}
	return map[string]any{"conversation_id": "conv-1", "title": "Une branche", "current_node": "answer-2", "mapping": map[string]any{
		"root": map[string]any{"id": "root", "parent": nil, "message": nil}, "user-1": map[string]any{"id": "user-1", "parent": "root", "message": message("user-1", "user", "Question")},
		"answer-1": map[string]any{"id": "answer-1", "parent": "user-1", "message": message("answer-1", "assistant", "Branche abandonnée")}, "answer-2": map[string]any{"id": "answer-2", "parent": "user-1", "message": message("answer-2", "assistant", "Réponse active")}}}
}
func TestConversationPreservesActiveBranchAndContinuation(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/backend-api/conversation/conv-1" {
			t.Errorf("path %s", r.URL.Path)
		}
		jsonResponse(w, conversationFixture())
	})
	setSession(c, "secret")
	result, e := c.GetConversation(context.Background(), "conv-1", "", 1)
	if e != nil {
		t.Fatal(e)
	}
	page := result.(Conversation)
	if len(page.Messages) != 1 || page.Messages[0].ID != "user-1" || page.NextCursor == nil || *page.ContinuationParentID != "answer-2" {
		t.Fatalf("bad active page %+v", page)
	}
	result, e = c.GetConversation(context.Background(), "conv-1", *page.NextCursor, 1)
	if e != nil {
		t.Fatal(e)
	}
	page = result.(Conversation)
	if page.Messages[0].Text != "Réponse active" || page.NextCursor != nil {
		t.Fatalf("wrong branch %+v", page)
	}
}
func TestConversationRejectsCycleAndMissingParent(t *testing.T) {
	for _, parent := range []string{"answer-2", "missing"} {
		t.Run(parent, func(t *testing.T) {
			fixture := conversationFixture()
			fixture["mapping"].(map[string]any)["answer-2"].(map[string]any)["parent"] = parent
			var raw remoteConversation
			json.Unmarshal(jsonBytes(fixture), &raw)
			_, _, e := normalizeBranch(raw)
			requireCode(t, e, "UPSTREAM_ERROR")
		})
	}
}
func TestConversationCursorRejectsChangedBranch(t *testing.T) {
	var changed atomic.Bool
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		fixture := conversationFixture()
		if changed.Load() {
			fixture["current_node"] = "answer-1"
		}
		jsonResponse(w, fixture)
	})
	setSession(c, "secret")
	first, e := c.GetConversation(context.Background(), "conv-1", "", 1)
	if e != nil {
		t.Fatal(e)
	}
	next := first.(Conversation).NextCursor
	changed.Store(true)
	_, e = c.GetConversation(context.Background(), "conv-1", *next, 1)
	requireCode(t, e, "UPSTREAM_ERROR")
}

func TestConversationExcludesNonPublicAssistantChannels(t *testing.T) {
	fixture := conversationFixture()
	nodes := fixture["mapping"].(map[string]any)
	nodes["analysis"] = map[string]any{"id": "analysis", "parent": "user-1", "message": map[string]any{"id": "analysis", "author": map[string]any{"role": "assistant"}, "channel": "analysis", "content": map[string]any{"parts": []any{"not the public answer"}}}}
	nodes["answer-2"].(map[string]any)["parent"] = "analysis"
	var raw remoteConversation
	json.Unmarshal(jsonBytes(fixture), &raw)
	messages, continuation, e := normalizeBranch(raw)
	if e != nil || len(messages) != 2 || continuation != "answer-2" || strings.Contains(string(jsonBytes(messages)), "not the public answer") {
		t.Fatalf("unexpected public branch %v %s %v", messages, continuation, e)
	}
}
