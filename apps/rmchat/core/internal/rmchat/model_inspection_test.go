package rmchat

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"
)

func TestModelInspectionRequiresExplicitStartupOption(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { t.Fatal("disabled inspection used the network") })
	setSession(c, "private-secret")
	_, problem := c.dispatch(context.Background(), "models.inspect", json.RawMessage(`{}`), nil)
	requireCode(t, problem, "METHOD_NOT_FOUND")
	if strings.Contains(string(jsonBytes(c.Status())), "models.inspect") {
		t.Fatal("diagnostic capability exposed by default")
	}
	c.EnableModelInspection()
	if !strings.Contains(string(jsonBytes(c.Status())), "models.inspect") {
		t.Fatal("opt-in capability missing")
	}
	c.Logout()
	_, problem = c.InspectModels(context.Background())
	requireCode(t, problem, "AUTH_REQUIRED")
}

func TestModelInspectionProjectsSchemaWithoutRawStringsOrCredentials(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if r.Method != http.MethodGet || r.URL.Path != "/backend-api/models" || r.Header.Get("Authorization") != "Bearer private-secret" {
			t.Error("inspection changed normal request")
		}
		jsonResponse(w, map[string]any{
			"accessToken": "private-secret", "default_model_slug": "model.v2",
			"models": []any{map[string]any{"slug": "model.v2", "title": "Modèle", "can_use": false,
				"description": "private-secret", "status": "private-secret", "remaining": 10,
				"capabilities": map[string]any{"is_available": true, "token": "private-secret"}}},
			"categories": []any{map[string]any{"category": "group", "subscription_level": "plus", "default_model": "model.v2", "model_slugs": []any{"model.v2"}}},
		})
	})
	c.EnableModelInspection()
	setSession(c, "private-secret")
	peer := testPeer(t, c)
	peer.send(t, "ui:1", "models.inspect", map[string]any{})
	response := peer.read(t)
	if response["error"] != nil {
		t.Fatalf("inspection failed: %v", response["error"])
	}
	data := string(jsonBytes(response["result"]))
	for _, required := range []string{`"topLevelKeys"`, `"slug":"model.v2"`, `"can_use":false`, `"remaining":10`, `"is_available":true`, `"subscription_level":"plus"`, `"default_model_slug":"model.v2"`} {
		if !strings.Contains(data, required) {
			t.Fatalf("missing projected field %s in %s", required, data)
		}
	}
	if strings.Contains(data, "private-secret") || calls.Load() != 1 || c.state.models != nil {
		t.Fatal("inspection leaked raw data, retried, or changed model eligibility")
	}
}

func TestModelInspectionBoundsProjection(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, map[string]any{"models": make([]any, 1025)})
	})
	c.EnableModelInspection()
	setSession(c, "fixture")
	_, problem := c.InspectModels(context.Background())
	requireCode(t, problem, "RESPONSE_TOO_LARGE")
}
