package rmchat

import "strings"

// Model slugs are opaque JSON values, not conversation path components.
// Use the same validation for publication and submission, including punctuation
// used by model identifiers; never reinterpret the slug as an endpoint or alias.
func validModelID(id string) bool {
	if id == "" || len(id) > 128 {
		return false
	}
	for _, r := range id {
		if !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || strings.ContainsRune("-_.:/", r)) {
			return false
		}
	}
	return true
}

// Apply only explicit model-level booleans. Subscription/category labels and
// absent or differently typed flags are not evidence of a user's entitlement.
// Unknown models remain as offered by the authenticated catalogue; membership
// is not a guarantee that a changing quota or later send will be accepted.
func modelExplicitlyDenied(model map[string]any) bool {
	for _, key := range []string{"available", "is_available", "enabled", "is_enabled", "can_use", "has_access", "is_entitled", "is_user_selectable", "user_selectable", "is_visible"} {
		if value, ok := model[key].(bool); ok && !value {
			return true
		}
	}
	for _, key := range []string{"disabled", "is_disabled", "hidden", "is_hidden", "requires_upgrade"} {
		if value, ok := model[key].(bool); ok && value {
			return true
		}
	}
	return false
}
