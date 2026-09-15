package rmchat

import (
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"strings"
	"sync"
)

// sessionCookies keeps ordinary response cookies only for this client's origin.
// Closing the jar also prevents an outstanding response from repopulating it.
type sessionCookies struct {
	mu     sync.Mutex
	origin *url.URL
	jar    http.CookieJar
}

func (c *Client) newSession(token string) session {
	origin, _ := url.Parse(c.base)
	jar, _ := cookiejar.New(nil)
	cookies := &sessionCookies{origin: origin, jar: jar}
	client := *c.http
	// Each import starts on a fresh connection pool. In particular, the first
	// GET /api/auth/session must not reuse an old connection whose failure could
	// trigger net/http's replay of a nominally idempotent GET.
	if transport, ok := c.http.Transport.(*http.Transport); ok {
		client.Transport = transport.Clone()
	}
	client.Jar = cookies
	return session{token: token, http: &client, cookies: cookies}
}

func (s session) close() {
	s.cookies.clear()
	if s.http != nil {
		s.http.CloseIdleConnections()
	}
}

func (j *sessionCookies) allows(u *url.URL) bool {
	return u != nil && j.origin != nil && u.Scheme == j.origin.Scheme && strings.EqualFold(u.Host, j.origin.Host)
}

func (j *sessionCookies) Cookies(u *url.URL) []*http.Cookie {
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.jar == nil || !j.allows(u) {
		return nil
	}
	return j.jar.Cookies(u)
}

func (j *sessionCookies) SetCookies(u *url.URL, cookies []*http.Cookie) {
	j.mu.Lock()
	defer j.mu.Unlock()
	if j.jar != nil && j.allows(u) {
		j.jar.SetCookies(u, cookies)
	}
}

func (j *sessionCookies) clear() {
	if j == nil {
		return
	}
	j.mu.Lock()
	defer j.mu.Unlock()
	j.jar = nil
}
