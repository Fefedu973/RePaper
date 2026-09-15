package rmchat

import (
	"context"
	"fmt"
	"net/http"
	"net/url"
	"sync/atomic"
	"testing"
)

func requireCookie(t *testing.T, r *http.Request, name, value string) {
	t.Helper()
	cookie, err := r.Cookie(name)
	if err != nil || cookie.Value != value {
		t.Errorf("missing or wrong fixture cookie %s", name)
	}
}

func TestSessionCookiesContinueAcrossAuthenticatedOperations(t *testing.T) {
	var step atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		n := step.Add(1)
		if n == 1 {
			if r.Header.Get("Cookie") != "" {
				t.Error("new import inherited cookies")
			}
		} else {
			requireCookie(t, r, "continuity", fmt.Sprint(n-1))
		}
		http.SetCookie(w, &http.Cookie{Name: "continuity", Value: fmt.Sprint(n), Path: "/"})
		switch r.URL.Path {
		case "/backend-api/models":
			jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": "remote-model"}}})
		case "/backend-api/conversations":
			jsonResponse(w, map[string]any{"items": []any{}, "total": 0})
		case "/backend-api/conversation/conv-1":
			jsonResponse(w, map[string]any{"current_node": "root", "mapping": map[string]any{"root": map[string]any{"id": "root"}}})
		case "/backend-api/f/conversation/prepare":
			jsonResponse(w, map[string]any{"conduit_token": "fixture-conduit"})
		case "/backend-api/f/conversation":
			w.Header().Set("Content-Type", "text/event-stream")
			fmt.Fprint(w, sse(snapshotEvent("answer-1", "final", "Réponse", true), "[DONE]"))
		default:
			t.Error("unexpected endpoint")
		}
	})
	ctx := context.Background()
	if _, e := c.Import(ctx, credential("fixture-access"), false); e != nil {
		t.Fatal(e)
	}
	if _, e := c.Models(ctx); e != nil {
		t.Fatal(e)
	}
	if _, e := c.ListConversations(ctx, "", 5); e != nil {
		t.Fatal(e)
	}
	if _, e := c.GetConversation(ctx, "conv-1", "", 5); e != nil {
		t.Fatal(e)
	}
	if _, e := c.Send(ctx, validSend(), nil); e != nil {
		t.Fatal(e)
	}
	if step.Load() != 6 {
		t.Fatal("incomplete sequential flow")
	}
}

func TestSessionExchangeResponseCookiesReachValidation(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/auth/session":
			requireCookie(t, r, "__Secure-next-auth.session-token", "fixture-session")
			http.SetCookie(w, &http.Cookie{Name: "continuity", Value: "exchange", Path: "/"})
			jsonResponse(w, map[string]any{"accessToken": "fixture-derived"})
		case "/backend-api/models":
			requireCookie(t, r, "continuity", "exchange")
			jsonResponse(w, map[string]any{"models": []any{}})
		}
	})
	value := credential("fixture-session")
	value.Kind = "session_token"
	if _, e := c.Import(context.Background(), value, true); e != nil {
		t.Fatal(e)
	}
	if _, e := c.Models(context.Background()); e != nil {
		t.Fatal(e)
	}
}

func TestFailedImportPreservesActiveCookies(t *testing.T) {
	for _, tc := range []struct {
		name, code string
		status     int
		body       string
	}{
		{"rejected", "AUTH_INVALID", 401, `{}`},
		{"malformed", "UPSTREAM_ERROR", 200, `{}`},
	} {
		t.Run(tc.name, func(t *testing.T) {
			c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
				if r.Header.Get("Authorization") == "Bearer candidate" {
					if r.Header.Get("Cookie") != "" {
						t.Error("candidate inherited active account cookies")
					}
					http.SetCookie(w, &http.Cookie{Name: "continuity", Value: "candidate", Path: "/"})
					w.Header().Set("Content-Type", "application/json")
					w.WriteHeader(tc.status)
					fmt.Fprint(w, tc.body)
					return
				}
				if r.Header.Get("Cookie") != "" {
					requireCookie(t, r, "continuity", "active")
				}
				http.SetCookie(w, &http.Cookie{Name: "continuity", Value: "active", Path: "/"})
				jsonResponse(w, map[string]any{"models": []any{}})
			})
			if _, e := c.Import(context.Background(), credential("active"), false); e != nil {
				t.Fatal(e)
			}
			before := c.state.current()
			_, e := c.Import(context.Background(), credential("candidate"), false)
			requireCode(t, e, tc.code)
			after := c.state.current()
			if after.http != before.http || after.cookies != before.cookies || after.token != before.token {
				t.Fatal("failed import replaced active transport")
			}
			if _, e := c.Models(context.Background()); e != nil {
				t.Fatal(e)
			}
		})
	}
}

func TestAccountReplacementAndLogoutClearCookies(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Cookie") != "" {
			t.Error("new import retained old account cookies")
		}
		http.SetCookie(w, &http.Cookie{Name: "continuity", Value: "private-fixture", Path: "/"})
		jsonResponse(w, map[string]any{"models": []any{}})
	})
	u, _ := url.Parse(c.base + "/backend-api/models")
	if _, e := c.Import(context.Background(), credential("old"), false); e != nil {
		t.Fatal(e)
	}
	old := c.state.current()
	if _, e := c.Import(context.Background(), credential("new"), false); e != nil {
		t.Fatal(e)
	}
	if len(old.cookies.Cookies(u)) != 0 {
		t.Fatal("replaced account retained cookies")
	}
	current := c.state.current()
	if len(current.cookies.Cookies(u)) != 1 {
		t.Fatal("active account lost response cookie")
	}
	c.Logout()
	if len(current.cookies.Cookies(u)) != 0 || c.Status().Authenticated {
		t.Fatal("logout retained session")
	}
	if _, e := c.Import(context.Background(), credential("after-logout"), false); e != nil {
		t.Fatal(e)
	}
}

func TestLateResponseCannotRestoreCookiesAfterLogout(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if calls.Add(1) > 1 {
			close(started)
			<-release
		}
		http.SetCookie(w, &http.Cookie{Name: "continuity", Value: "late-fixture", Path: "/"})
		jsonResponse(w, map[string]any{"models": []any{}})
	})
	if _, e := c.Import(context.Background(), credential("active"), false); e != nil {
		t.Fatal(e)
	}
	previous := c.state.current()
	done := make(chan struct{})
	go func() { defer close(done); c.Models(context.Background()) }()
	<-started
	c.Logout()
	close(release)
	<-done
	u, _ := url.Parse(c.base)
	if len(previous.cookies.Cookies(u)) != 0 || c.Status().Authenticated {
		t.Fatal("late response restored closed session cookies")
	}
}

func TestCookieOriginScopeAndDeletion(t *testing.T) {
	c, e := NewClient(nil)
	if e != nil {
		t.Fatal(e)
	}
	s := c.newSession("fixture")
	origin, _ := url.Parse(c.base + "/backend-api/models")
	s.cookies.SetCookies(origin, []*http.Cookie{{Name: "continuity", Value: "private-fixture", Domain: "chatgpt.com", Path: "/", Secure: true}})
	if len(s.cookies.Cookies(origin)) != 1 {
		t.Fatal("origin cookie missing")
	}
	for _, address := range []string{"https://files.chatgpt.com/backend-api/models", "https://chatgpt.com:444/backend-api/models", "http://chatgpt.com/backend-api/models", "https://files.oaiusercontent.com/upload"} {
		other, _ := url.Parse(address)
		if len(s.cookies.Cookies(other)) != 0 {
			t.Error("cookie crossed origin")
		}
		s.cookies.SetCookies(other, []*http.Cookie{{Name: "foreign", Value: "fixture", Domain: "chatgpt.com", Path: "/"}})
	}
	if len(s.cookies.Cookies(origin)) != 1 {
		t.Fatal("foreign response contaminated jar")
	}
	s.cookies.SetCookies(origin, []*http.Cookie{{Name: "continuity", Value: "", Domain: "chatgpt.com", Path: "/", MaxAge: -1}})
	if len(s.cookies.Cookies(origin)) != 0 {
		t.Fatal("server cookie deletion ignored")
	}
}

func TestLogoutDuringImportCannotActivateCandidateCookies(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		close(started)
		<-release
		http.SetCookie(w, &http.Cookie{Name: "continuity", Value: "candidate-fixture", Path: "/"})
		jsonResponse(w, map[string]any{"models": []any{}})
	})
	setSession(c, "previous-fixture")
	done := make(chan *Error, 1)
	go func() { _, e := c.Import(context.Background(), credential("candidate"), false); done <- e }()
	<-started
	c.Logout()
	close(release)
	requireCode(t, <-done, "CANCELLED")
	if c.Status().Authenticated || c.state.current().cookies != nil {
		t.Fatal("candidate reactivated session after logout")
	}
}
