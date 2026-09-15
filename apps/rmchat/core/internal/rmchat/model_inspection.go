package rmchat

import (
	"context"
	"math"
	"net/http"
	"sort"
	"strings"
)

// EnableModelInspection is a startup-only opt-in for local QA. Normal clients
// neither advertise nor accept models.inspect; no diagnostic file is written.
func (c *Client) EnableModelInspection() { c.inspectModels = true }

func (c *Client) InspectModels(ctx context.Context) (any, *Error) {
	if !c.inspectModels {
		return nil, failure("METHOD_NOT_FOUND")
	}
	s, problem := c.authenticated()
	if problem != nil {
		return nil, problem
	}
	resp, problem := c.request(ctx, http.MethodGet, "/backend-api/models", s, nil)
	if problem != nil {
		return nil, problem
	}
	var raw map[string]any
	if problem = decodeResponse(resp, &raw, 4<<20); problem != nil {
		return nil, problem
	}
	if _, ok := raw["models"].([]any); !ok {
		return nil, httpFailure(resp, "UPSTREAM_ERROR", false)
	}
	projection := projectModelSchema(raw, 0)
	projection["topLevelKeys"] = projection["keys"]
	delete(projection, "keys")
	for _, key := range []string{"models", "categories"} {
		items, _ := raw[key].([]any)
		projected := []any{}
		if len(items) > 1024 {
			return nil, failure("RESPONSE_TOO_LARGE")
		}
		for _, item := range items {
			if object, ok := item.(map[string]any); ok {
				projected = append(projected, projectModelSchema(object, 0))
			}
		}
		projection[key] = projected
	}
	if len(jsonBytes(projection)) > 128<<10 {
		return nil, failure("RESPONSE_TOO_LARGE")
	}
	if ctx.Err() != nil || c.state.current().generation != s.generation {
		return nil, failure("CANCELLED")
	}
	return projection, nil
}

func schemaKeys(object map[string]any) []string {
	keys := []string{}
	for key := range object {
		if len(key) > 64 || key == "" {
			continue
		}
		valid := true
		for _, r := range key {
			if !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '_' || r == '-') {
				valid = false
			}
		}
		if valid {
			keys = append(keys, key)
		}
	}
	sort.Strings(keys)
	if len(keys) > 128 {
		keys = keys[:128]
	}
	return keys
}

func projectModelSchema(object map[string]any, depth int) map[string]any {
	out := map[string]any{"keys": schemaKeys(object)}
	// Boolean schema values cannot contain credentials. Preserve new flag names
	// so the diagnostic can identify a changed catalogue without guessing them.
	for _, key := range schemaKeys(object) {
		if value, ok := object[key].(bool); ok {
			out[key] = value
		}
	}
	for _, key := range []string{"slug", "category", "default_model", "default_model_slug", "code_interpreter_model", "browsing_model", "plugins_model", "dalle_model", "subscription_level"} {
		if text, ok := object[key].(string); ok && validModelID(text) {
			out[key] = text
		}
	}
	for _, key := range []string{"title", "human_category_name", "human_category_short_name"} {
		if text, ok := object[key].(string); ok && len(text) <= 256 && !strings.ContainsAny(text, "\x00\r\n") {
			out[key] = text
		}
	}
	for _, key := range []string{"model_ids", "model_slugs", "available_models"} {
		if values, ok := object[key].([]any); ok && len(values) <= 256 {
			ids := []string{}
			for _, value := range values {
				if id, ok := value.(string); ok && validModelID(id) {
					ids = append(ids, id)
				}
			}
			out[key] = ids
		}
	}
	for _, key := range []string{"can_use", "is_available", "is_enabled", "enabled", "is_user_selectable", "is_visible", "hidden", "disabled", "available", "is_default", "is_selected", "remaining", "remaining_messages", "limit", "max_requests", "reset_after", "requires_upgrade", "is_entitled", "has_access"} {
		switch value := object[key].(type) {
		case bool:
			out[key] = value
		case float64:
			if !math.IsInf(value, 0) && !math.IsNaN(value) && value >= 0 && value <= 1e9 {
				out[key] = value
			}
		}
	}
	for _, key := range []string{"availability", "status"} {
		if value, ok := object[key].(string); ok {
			switch value {
			case "available", "unavailable", "enabled", "disabled", "hidden", "limited", "blocked", "allowed", "denied", "restricted":
				out[key] = value
			}
		}
	}
	if depth < 3 {
		for _, key := range []string{"capabilities", "product_features", "entitlements", "availability", "access"} {
			if nested, ok := object[key].(map[string]any); ok {
				out[key] = projectModelSchema(nested, depth+1)
			}
		}
	}
	return out
}
