package rmchat

import (
	"net/http"
	"strconv"
	"strings"
	"time"
)

const sessionCookieName = "__Secure-next-auth.session-token"
const maxSessionChunks = 16
const maxCredentialBytes = 32 << 10

// Cookie values must be representable without net/http silently sanitizing
// them. The same complete value is persisted for either response cookie form.
func validSessionValue(value string) bool {
	if value == "" || len(value) > maxCredentialBytes {
		return false
	}
	for i := range value {
		b := value[i]
		if b < 0x21 || b > 0x7e || b == '"' || b == ',' || b == ';' || b == '\\' {
			return false
		}
	}
	return true
}

func isSessionCookie(name string) bool {
	return name == sessionCookieName || strings.HasPrefix(name, sessionCookieName+".")
}

// sessionCredentialFromResponse accepts a base cookie or a complete sequence
// .0, .1, ... .15. It never persists an ambiguous or partial replacement.
// Expired/deleted old chunks may accompany a new, shorter sequence or base.
func sessionCredentialFromResponse(resp *http.Response, original Credential) (Credential, *Error) {
	invalid := func() (Credential, *Error) {
		return Credential{}, httpFailure(resp, "SESSION_REIMPORT_REQUIRED", false)
	}
	seen := map[string]bool{}
	chunks := map[int]string{}
	base := ""
	total := 0
	now := time.Now()
	for _, raw := range resp.Header.Values("Set-Cookie") {
		cookie, err := http.ParseSetCookie(raw)
		if err != nil {
			name, _, _ := strings.Cut(raw, "=")
			if isSessionCookie(strings.TrimSpace(name)) {
				return invalid()
			}
			continue
		}
		if !isSessionCookie(cookie.Name) {
			continue
		}
		if seen[cookie.Name] {
			return invalid()
		}
		seen[cookie.Name] = true
		index := -1
		if cookie.Name != sessionCookieName {
			suffix := strings.TrimPrefix(cookie.Name, sessionCookieName+".")
			index, err = strconv.Atoi(suffix)
			if err != nil || index < 0 || index >= maxSessionChunks || strconv.Itoa(index) != suffix {
				return invalid()
			}
		}
		if cookie.MaxAge < 0 || (!cookie.Expires.IsZero() && !cookie.Expires.After(now)) {
			continue
		}
		if !validSessionValue(cookie.Value) {
			return invalid()
		}
		total += len(cookie.Value)
		if total > maxCredentialBytes {
			return invalid()
		}
		if index < 0 {
			base = cookie.Value
		} else {
			chunks[index] = cookie.Value
		}
	}
	if len(chunks) > 0 {
		if base != "" {
			return invalid()
		}
		var joined strings.Builder
		for i := 0; i < len(chunks); i++ {
			value, exists := chunks[i]
			if !exists {
				return invalid()
			}
			joined.WriteString(value)
		}
		original.Value = joined.String()
	} else if base != "" {
		original.Value = base
	} else if len(seen) > 0 {
		return invalid()
	}
	if !validSessionValue(original.Value) || len(jsonBytes(original)) > maxCredentialBytes {
		return invalid()
	}
	return original, nil
}
