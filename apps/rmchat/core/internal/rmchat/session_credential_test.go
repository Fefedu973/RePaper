package rmchat

import (
	"context"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func sessionCredential(value string) Credential {
	result := credential(value)
	result.Kind = "session_token"
	return result
}

func TestSessionImportRequiresPersistenceBeforeNetwork(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { calls.Add(1) })
	setSession(c, "old-fixture-access")
	previous := c.state.current()
	_, e := c.Import(context.Background(), sessionCredential("fixture-session"), false)
	requireCode(t, e, "SESSION_PERSISTENCE_REQUIRED")
	if calls.Load() != 0 || c.state.current().http != previous.http || !c.Status().Authenticated {
		t.Fatal("non-persistent session import reached network or changed the active account")
	}
	peer := testPeer(t, c)
	for i, args := range []map[string]any{
		{"credential": sessionCredential("fixture-session")},
		{"credential": sessionCredential("fixture-session"), "persistSession": false},
	} {
		peer.send(t, fmt.Sprintf("ui:%d", i+1), "auth.import", args)
		if rpcKind(peer.read(t)) != "SESSION_PERSISTENCE_REQUIRED" {
			t.Fatal("IPC default did not protect a rotating session")
		}
	}
	peer.send(t, "ui:3", "auth.import", map[string]any{"credential": sessionCredential("fixture-session"), "persistSession": "true"})
	if rpcKind(peer.read(t)) != "INVALID_PARAMS" || calls.Load() != 0 {
		t.Fatal("non-boolean persistence flag reached network")
	}
}

func TestSessionImportReturnsUpdateBeforeIndependentModelsFailure(t *testing.T) {
	var exchanges, models atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/auth/session":
			exchanges.Add(1)
			requireCookie(t, r, sessionCookieName, "original-fixture")
			http.SetCookie(w, &http.Cookie{Name: sessionCookieName + ".1", Value: "fixture", Path: "/"})
			http.SetCookie(w, &http.Cookie{Name: sessionCookieName + ".0", Value: "rotated-", Path: "/"})
			jsonResponse(w, map[string]any{"accessToken": "derived-fixture-access", "user": map[string]any{"id": "account-1", "name": "Élodie"}})
		case "/backend-api/models":
			models.Add(1)
			if r.Header.Get("Authorization") != "Bearer derived-fixture-access" {
				t.Error("derived access token not used")
			}
			requireCookie(t, r, sessionCookieName+".0", "rotated-")
			requireCookie(t, r, sessionCookieName+".1", "fixture")
			w.WriteHeader(http.StatusForbidden)
		default:
			t.Error("unexpected request")
		}
	})
	result, e := c.Import(context.Background(), sessionCredential("original-fixture"), true)
	if e != nil || !result.Authenticated || result.Account == nil || result.Account.ID != "account-1" || result.CredentialUpdate == nil || *result.CredentialUpdate != sessionCredential("rotated-fixture") {
		t.Fatal("complete session exchange did not return a persistable credential")
	}
	if exchanges.Load() != 1 || models.Load() != 0 {
		t.Fatal("rotation was held behind an extra model request")
	}
	_, e = c.Models(context.Background())
	requireCode(t, e, "UPSTREAM_FORBIDDEN")
	if exchanges.Load() != 1 || models.Load() != 1 || !c.Status().Authenticated {
		t.Fatal("independent model rejection retried the exchange or discarded its session")
	}
}

func TestSessionCredentialUpdatesAcceptBaseAndContiguousChunks(t *testing.T) {
	deleted := "; Max-Age=0; Path=/"
	for _, tc := range []struct {
		name, want string
		cookies    []string
	}{
		{"unchanged-without-cookie", "original-fixture", nil},
		{"unrelated-cookie", "original-fixture", []string{"ordinary=value; Path=/"}},
		{"base", "rotated-fixture", []string{sessionCookieName + "=rotated-fixture; Path=/; Secure; HttpOnly"}},
		{"unchanged-base", "original-fixture", []string{sessionCookieName + "=original-fixture; Path=/"}},
		{"out-of-order-chunks", "rotated-fixture", []string{sessionCookieName + ".1=fixture; Path=/", sessionCookieName + ".0=rotated-; Path=/"}},
		{"single-chunk", "rotated-fixture", []string{sessionCookieName + ".0=rotated-fixture; Path=/"}},
		{"base-deleted-new-chunks", "rotated-fixture", []string{sessionCookieName + "=" + deleted, sessionCookieName + ".0=rotated-; Path=/", sessionCookieName + ".1=fixture; Path=/"}},
		{"chunks-deleted-new-base", "rotated-fixture", []string{sessionCookieName + ".0=" + deleted, sessionCookieName + ".1=" + deleted, sessionCookieName + "=rotated-fixture; Path=/"}},
		{"shorter-chunks", "rotated-fixture", []string{sessionCookieName + ".0=rotated-; Path=/", sessionCookieName + ".1=fixture; Path=/", sessionCookieName + ".2=" + deleted}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			response := &http.Response{StatusCode: 200, Header: http.Header{"Set-Cookie": tc.cookies}}
			got, e := sessionCredentialFromResponse(response, sessionCredential("original-fixture"))
			if e != nil || got != sessionCredential(tc.want) {
				t.Fatal("valid session cookie update was not preserved")
			}
		})
	}
}

func TestSessionCredentialUpdatesRejectAmbiguousOrIncompleteCookies(t *testing.T) {
	for _, tc := range []struct {
		name    string
		cookies []string
	}{
		{"missing-zero", []string{sessionCookieName + ".1=private-fixture"}},
		{"gap", []string{sessionCookieName + ".0=private-fixture", sessionCookieName + ".2=private-fixture"}},
		{"base-and-chunk", []string{sessionCookieName + "=private-fixture", sessionCookieName + ".0=private-fixture"}},
		{"duplicate-base", []string{sessionCookieName + "=private-fixture", sessionCookieName + "=private-fixture"}},
		{"duplicate-chunk", []string{sessionCookieName + ".0=private-fixture", sessionCookieName + ".0=private-fixture"}},
		{"non-numeric-suffix", []string{sessionCookieName + ".x=private-fixture"}},
		{"noncanonical-number", []string{sessionCookieName + ".00=private-fixture"}},
		{"negative-index", []string{sessionCookieName + ".-1=private-fixture"}},
		{"too-many-chunks", []string{sessionCookieName + ".16=private-fixture"}},
		{"empty-value", []string{sessionCookieName + "="}},
		{"deleted-only", []string{sessionCookieName + "=; Max-Age=0"}},
		{"deleted-chunks-only", []string{sessionCookieName + ".0=; Max-Age=0", sessionCookieName + ".1=; Max-Age=0"}},
		{"expired-only", []string{sessionCookieName + "=private-fixture; Expires=Thu, 01 Jan 1970 00:00:00 GMT"}},
		{"invalid-value", []string{sessionCookieName + "=private-fixture value"}},
		{"malformed-cookie", []string{sessionCookieName + "=\"private-fixture"}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			response := &http.Response{StatusCode: 200, Header: http.Header{"Set-Cookie": tc.cookies, "Content-Type": []string{"application/json"}}}
			got, e := sessionCredentialFromResponse(response, sessionCredential("original-fixture"))
			requireCode(t, e, "SESSION_REIMPORT_REQUIRED")
			if got.Value != "" || strings.Contains(string(jsonBytes(e)), "private-fixture") {
				t.Fatal("invalid session response leaked a cookie")
			}
		})
	}
}

func TestSessionCredentialUpdatesRespectCompleteEnvelopeLimit(t *testing.T) {
	overhead := len(jsonBytes(sessionCredential("")))
	for _, extra := range []int{0, 1} {
		value := strings.Repeat("a", maxCredentialBytes-overhead+extra)
		response := &http.Response{StatusCode: 200, Header: http.Header{"Set-Cookie": []string{sessionCookieName + "=" + value}}}
		got, e := sessionCredentialFromResponse(response, sessionCredential("original"))
		if extra == 0 {
			if e != nil || len(jsonBytes(got)) != maxCredentialBytes {
				t.Fatal("exact envelope boundary rejected")
			}
		} else {
			requireCode(t, e, "SESSION_REIMPORT_REQUIRED")
		}
	}
	response := &http.Response{StatusCode: 200, Header: make(http.Header)}
	for i := 0; i < maxSessionChunks; i++ {
		response.Header.Add("Set-Cookie", fmt.Sprintf("%s.%d=x", sessionCookieName, i))
	}
	got, e := sessionCredentialFromResponse(response, sessionCredential("original"))
	if e != nil || got.Value != strings.Repeat("x", maxSessionChunks) {
		t.Fatal("maximum contiguous chunk count rejected")
	}
}

func TestSessionFailurePreservesOldAccountAndDoesNotExposeUpdate(t *testing.T) {
	for _, tc := range []struct {
		name, body, code string
		status           int
		challenge        bool
	}{
		{"unauthorized", `{}`, "AUTH_INVALID", 401, false},
		{"challenge", `{}`, "WEB_AUTH_REQUIRED", 403, true},
		{"malformed-json", `not-json`, "UPSTREAM_ERROR", 200, false},
		{"missing-access", `{}`, "AUTH_INVALID", 200, false},
		{"explicit-json-challenge", `{"challenge":true}`, "WEB_AUTH_REQUIRED", 200, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var calls atomic.Int32
			c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				http.SetCookie(w, &http.Cookie{Name: sessionCookieName, Value: "rotated-private-fixture", Path: "/"})
				w.Header().Set("Content-Type", "application/json")
				if tc.challenge {
					w.Header().Set("Cf-Mitigated", "challenge")
				}
				w.WriteHeader(tc.status)
				fmt.Fprint(w, tc.body)
			})
			setSession(c, "old-private-fixture")
			old := c.state.current()
			result, e := c.Import(context.Background(), sessionCredential("candidate-private-fixture"), true)
			requireCode(t, e, tc.code)
			if calls.Load() != 1 || c.state.current().http != old.http || result.CredentialUpdate != nil || strings.Contains(string(jsonBytes(e)), "private-fixture") {
				t.Fatal("failed exchange retried, changed the account or exposed its credential")
			}
		})
	}
}

func TestSessionInvalidRotationPreservesPreviousAccount(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		http.SetCookie(w, &http.Cookie{Name: sessionCookieName + ".1", Value: "incomplete-private-fixture", Path: "/"})
		jsonResponse(w, map[string]any{"accessToken": "derived-private-fixture"})
	})
	setSession(c, "old-private-fixture")
	old := c.state.current()
	result, e := c.Import(context.Background(), sessionCredential("candidate-private-fixture"), true)
	requireCode(t, e, "SESSION_REIMPORT_REQUIRED")
	if calls.Load() != 1 || c.state.current().http != old.http || result.CredentialUpdate != nil {
		t.Fatal("incomplete rotation changed the account or returned a partial credential")
	}
}

func TestSessionCancellationPreservesPreviousAccount(t *testing.T) {
	started := make(chan struct{})
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { close(started); <-r.Context().Done() })
	setSession(c, "old-private-fixture")
	old := c.state.current()
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan *Error, 1)
	go func() {
		result, e := c.Import(ctx, sessionCredential("candidate-private-fixture"), true)
		if result.CredentialUpdate != nil {
			t.Error("cancelled exchange returned a credential")
		}
		done <- e
	}()
	<-started
	cancel()
	select {
	case e := <-done:
		requireCode(t, e, "CANCELLED")
	case <-time.After(3 * time.Second):
		t.Fatal("exchange cancellation did not finish")
	}
	if c.state.current().http != old.http || c.state.current().token != "old-private-fixture" {
		t.Fatal("cancelled exchange discarded the previous account")
	}
}

func TestSessionInputMustBeCookieSafeBeforeNetwork(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { calls.Add(1) })
	for _, value := range []string{"with space", "with,comma", "with;semicolon", `with\backslash`, `with"quote`, "non-ascii-é", strings.Repeat("a", maxCredentialBytes)} {
		_, e := c.Import(context.Background(), sessionCredential(value), true)
		requireCode(t, e, "INVALID_PARAMS")
	}
	if calls.Load() != 0 {
		t.Fatal("invalid cookie value reached HTTP serialization")
	}
}

func TestAccessImportDoesNotReturnCredentialUpdate(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if r.URL.Path != "/backend-api/models" || r.Header.Get("Authorization") != "Bearer supplied-private-fixture" {
			t.Error("access import authentication changed")
		}
		jsonResponse(w, map[string]any{"models": []any{}})
	})
	for _, persist := range []bool{false, true} {
		result, e := c.Import(context.Background(), credential("supplied-private-fixture"), persist)
		if e != nil || !result.Authenticated || result.CredentialUpdate != nil || strings.Contains(string(jsonBytes(result)), "private-fixture") {
			t.Fatal("access import returned a credential update or leaked its token")
		}
	}
	if calls.Load() != 2 {
		t.Fatal("access import no longer validates one models response")
	}
}

func TestSessionLogoutDuringExchangeDiscardsCandidate(t *testing.T) {
	started := make(chan struct{})
	release := make(chan struct{})
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		close(started)
		<-release
		http.SetCookie(w, &http.Cookie{Name: sessionCookieName, Value: "rotated-private-fixture", Path: "/"})
		jsonResponse(w, map[string]any{"accessToken": "derived-private-fixture"})
	})
	setSession(c, "old-private-fixture")
	old := c.state.current()
	done := make(chan *Error, 1)
	go func() {
		result, e := c.Import(context.Background(), sessionCredential("candidate-private-fixture"), true)
		if result.CredentialUpdate != nil {
			t.Error("cancelled candidate returned credential update")
		}
		done <- e
	}()
	<-started
	c.Logout()
	close(release)
	requireCode(t, <-done, "CANCELLED")
	u, _ := url.Parse(c.base)
	if c.Status().Authenticated || len(old.cookies.Cookies(u)) != 0 {
		t.Fatal("logout failed to close the session")
	}
}

func TestRPCSessionCredentialUpdateOnlyAppearsInImport(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/auth/session" {
			t.Error("unexpected request before persistence")
		}
		http.SetCookie(w, &http.Cookie{Name: sessionCookieName, Value: "rotated-private-fixture", Path: "/"})
		jsonResponse(w, map[string]any{"accessToken": "derived-private-fixture"})
	})
	peer := testPeer(t, c)
	peer.send(t, "ui:1", "auth.import", map[string]any{"credential": sessionCredential("candidate-private-fixture"), "persistSession": true})
	response := peer.read(t)
	result, ok := response["result"].(map[string]any)
	if !ok || result["authenticated"] != true {
		t.Fatal("session import failed")
	}
	update, ok := result["credentialUpdate"].(map[string]any)
	if !ok || len(update) != 4 || update["version"] != float64(1) || update["provider"] != "chatgpt-web" || update["kind"] != "session_token" || update["value"] != "rotated-private-fixture" {
		t.Fatal("private update contract mismatch")
	}
	delete(result, "credentialUpdate")
	if strings.Contains(string(jsonBytes(result)), "private-fixture") {
		t.Fatal("import status contains an unrelated secret")
	}
	peer.send(t, "ui:2", "auth.status", map[string]any{})
	status := peer.read(t)
	if strings.Contains(string(jsonBytes(status)), "private-fixture") || strings.Contains(string(jsonBytes(status)), "credentialUpdate") {
		t.Fatal("credential update leaked into auth.status")
	}
	active := c.state.current()
	peer.conn.Close()
	select {
	case <-peer.done:
	case <-time.After(3 * time.Second):
		t.Fatal("session connection did not close")
	}
	u, _ := url.Parse(c.base)
	if c.Status().Authenticated || len(active.cookies.Cookies(u)) != 0 {
		t.Fatal("session survived connection EOF")
	}
}

func TestPersistedChunkedSessionCanBeImportedOnFreshConnection(t *testing.T) {
	addresses := make(chan string, 2)
	var exchanges atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/auth/session" || r.Header.Get("Authorization") != "" {
			t.Error("unexpected exchange")
		}
		addresses <- r.RemoteAddr
		switch exchanges.Add(1) {
		case 1:
			requireCookie(t, r, sessionCookieName, "initial-fixture")
			http.SetCookie(w, &http.Cookie{Name: sessionCookieName + ".0", Value: "rotated-", Path: "/"})
			http.SetCookie(w, &http.Cookie{Name: sessionCookieName + ".1", Value: "fixture", Path: "/"})
		case 2:
			requireCookie(t, r, sessionCookieName, "rotated-fixture")
			if len(r.Cookies()) != 1 {
				t.Error("new import inherited old chunk cookies")
			}
		default:
			t.Error("session exchange replayed")
		}
		jsonResponse(w, map[string]any{"accessToken": "derived-fixture-access"})
	})
	first, e := c.Import(context.Background(), sessionCredential("initial-fixture"), true)
	if e != nil || first.CredentialUpdate == nil {
		t.Fatal("first import failed")
	}
	second, e := c.Import(context.Background(), *first.CredentialUpdate, true)
	if e != nil || second.CredentialUpdate == nil || second.CredentialUpdate.Value != "rotated-fixture" {
		t.Fatal("persisted session could not be imported again")
	}
	if <-addresses == <-addresses || exchanges.Load() != 2 {
		t.Fatal("session imports shared a connection or replayed an exchange")
	}
}
