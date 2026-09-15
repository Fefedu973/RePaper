package rmchat

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"reflect"
	"strings"
	"sync/atomic"
	"testing"
)

func TestModelsDeduplicateExactSlugsAndPreserveDistinctIDs(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, map[string]any{"models": []any{
			map[string]any{"slug": " model.v2 ", "title": "  Modèle  "},
			map[string]any{"slug": "model.v2", "title": "Duplicate title"},
			map[string]any{"slug": "vendor/model:variant", "title": "Modèle"},
			map[string]any{"slug": "other", "title": " "},
			map[string]any{"slug": "invalid id"},
			map[string]any{"slug": ""},
		}})
	})
	setSession(c, "fixture")
	result, problem := c.Models(context.Background())
	if problem != nil {
		t.Fatal(problem)
	}
	items := result.(map[string]any)["items"].([]Model)
	expected := []Model{{ID: "model.v2", Name: "Modèle"}, {ID: "vendor/model:variant", Name: "Modèle"}, {ID: "other", Name: "other"}}
	if !reflect.DeepEqual(items, expected) {
		t.Fatalf("model IDs changed or remained duplicated: %+v", items)
	}
}

func TestPublishedPunctuatedModelCanBeSubmittedUnchanged(t *testing.T) {
	var submissions atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/backend-api/models" {
			jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": "vendor/model.v2:variant"}}})
			return
		}
		var body map[string]any
		if json.NewDecoder(r.Body).Decode(&body) != nil || body["model"] != "vendor/model.v2:variant" {
			t.Error("model ID was changed between publication and submission")
		}
		if strings.HasSuffix(r.URL.Path, "/prepare") {
			jsonResponse(w, map[string]any{"conduit_token": "fixture"})
			return
		}
		submissions.Add(1)
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, sse(snapshotEvent("answer-1", "final", "Réponse", true), "[DONE]"))
	})
	setSession(c, "fixture")
	if _, problem := c.Models(context.Background()); problem != nil {
		t.Fatal(problem)
	}
	params := validSend()
	params.Model = "vendor/model.v2:variant"
	if _, problem := c.Send(context.Background(), params, nil); problem != nil || submissions.Load() != 1 {
		t.Fatalf("published model not usable: %v", problem)
	}
}

func TestSendRejectsModelMissingFromLoadedCatalogueBeforeNetwork(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": "allowed"}}})
	})
	setSession(c, "fixture")
	if _, problem := c.Models(context.Background()); problem != nil {
		t.Fatal(problem)
	}
	_, problem := c.Send(context.Background(), validSend(), nil)
	requireCode(t, problem, "MODEL_UNAVAILABLE")
	if problem.Stage != "validation" || calls.Load() != 1 || len(c.state.sent) != 0 {
		t.Fatal("unlisted model was prepared, submitted, or marked as submitted")
	}
}

func TestFailedModelsPreserveCatalogueAndLogoutClearsIt(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if calls.Add(1) == 1 {
			jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": "first"}}})
			return
		}
		w.WriteHeader(http.StatusForbidden)
	})
	setSession(c, "fixture")
	if _, problem := c.Models(context.Background()); problem != nil {
		t.Fatal(problem)
	}
	_, problem := c.Models(context.Background())
	requireCode(t, problem, "UPSTREAM_FORBIDDEN")
	if !c.state.models["first"] {
		t.Fatal("failed catalogue erased the established model list")
	}
	c.Logout()
	if c.state.models != nil {
		t.Fatal("catalogue survived logout")
	}
}

func TestLateModelsResponseCannotRestoreCatalogueAfterLogout(t *testing.T) {
	started, release := make(chan struct{}), make(chan struct{})
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		close(started)
		<-release
		jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": "previous-account-model"}}})
	})
	setSession(c, "fixture")
	done := make(chan *Error, 1)
	go func() { _, problem := c.Models(context.Background()); done <- problem }()
	<-started
	c.Logout()
	close(release)
	requireCode(t, <-done, "CANCELLED")
	if c.state.models != nil {
		t.Fatal("late response restored the old account's models")
	}
}

func TestSendLoadsMissingCatalogueAndPreservesRequestedModel(t *testing.T) {
	for _, test := range []struct {
		name, offered, code string
	}{
		{"offered", "remote-model", ""},
		{"absent", "different-model", "MODEL_UNAVAILABLE"},
		{"catalogueRejected", "", "UPSTREAM_FORBIDDEN"},
	} {
		t.Run(test.name, func(t *testing.T) {
			var sequence []string
			c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
				sequence = append(sequence, r.URL.Path)
				if r.URL.Path == "/backend-api/models" {
					if r.Method != http.MethodGet {
						t.Error("model validation must only read the catalogue")
					}
					if test.offered == "" {
						w.WriteHeader(403)
						return
					}
					jsonResponse(w, map[string]any{"models": []any{map[string]any{"slug": test.offered}}})
					return
				}
				var body map[string]any
				if json.NewDecoder(r.Body).Decode(&body) != nil || body["model"] != "remote-model" {
					t.Error("requested model was replaced by a catalogue default")
				}
				if strings.HasSuffix(r.URL.Path, "/prepare") {
					jsonResponse(w, map[string]any{"conduit_token": "fixture"})
					return
				}
				w.Header().Set("Content-Type", "text/event-stream")
				fmt.Fprint(w, sse(snapshotEvent("answer-1", "final", "Réponse", true), "[DONE]"))
			})
			setSession(c, "fixture") // No catalogue is installed by this fixture.
			_, problem := c.Send(context.Background(), validSend(), nil)
			if test.code != "" {
				requireCode(t, problem, test.code)
				if problem.Stage != "validation" || !reflect.DeepEqual(sequence, []string{"/backend-api/models"}) || len(c.state.sent) != 0 {
					t.Fatal("failed catalogue caused preparation or submission")
				}
				return
			}
			if problem != nil || !reflect.DeepEqual(sequence, []string{"/backend-api/models", "/backend-api/f/conversation/prepare", "/backend-api/f/conversation"}) {
				t.Fatalf("wrong initial lifecycle: %v %+v", problem, sequence)
			}
			second := validSend()
			second.MessageID = "b6e076be-ee10-441e-baa5-9b7c932c489e"
			if _, problem = c.Send(context.Background(), second, nil); problem != nil || len(sequence) != 5 || sequence[3] != "/backend-api/f/conversation/prepare" {
				t.Fatal("an established catalogue was needlessly fetched again")
			}
		})
	}
}

func TestModelsHonorExplicitDenialsWithoutInferringSubscriptionAccess(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		if r.URL.Path != "/backend-api/models" {
			t.Error("a denied model reached preparation or submission")
		}
		jsonResponse(w, map[string]any{
			"models": []any{
				map[string]any{"slug": "unknown", "title": "Unknown"},
				map[string]any{"slug": "allowed", "available": true},
				map[string]any{"slug": "unavailable", "available": false},
				map[string]any{"slug": "disabled", "enabled": false},
				map[string]any{"slug": "hidden", "hidden": true},
				map[string]any{"slug": "not-selectable", "is_user_selectable": false},
				map[string]any{"slug": "upgrade", "requires_upgrade": true},
				map[string]any{"slug": "account-denied", "can_use": false},
				map[string]any{"slug": "wrong-flag-type", "available": "false"},
				map[string]any{"slug": "duplicate", "available": true},
				map[string]any{"slug": "duplicate", "available": false},
				map[string]any{"slug": "duplicate-denied-first", "can_use": false},
				map[string]any{"slug": "duplicate-denied-first", "can_use": true},
			},
			"categories": []any{
				map[string]any{"category": "a", "subscription_level": "paid", "default_model": "unknown"},
				map[string]any{"category": "b", "subscription_level": "free", "default_model": "unavailable"},
				map[string]any{"category": "c", "subscription_level": "free", "default_model": "absent"},
			},
		})
	})
	setSession(c, "fixture")
	result, problem := c.Models(context.Background())
	if problem != nil {
		t.Fatal(problem)
	}
	items := result.(map[string]any)["items"].([]Model)
	expected := []Model{{ID: "unknown", Name: "Unknown"}, {ID: "allowed", Name: "allowed"}, {ID: "wrong-flag-type", Name: "wrong-flag-type"}}
	if !reflect.DeepEqual(items, expected) || result.(map[string]any)["defaultModelId"] != nil {
		t.Fatalf("catalogue inferred rights or ambiguous category defaults: %+v", result)
	}
	params := validSend()
	params.Model = "unavailable"
	_, problem = c.Send(context.Background(), params, nil)
	requireCode(t, problem, "MODEL_UNAVAILABLE")
	if problem.Stage != "validation" || calls.Load() != 1 || len(c.state.sent) != 0 {
		t.Fatal("explicitly denied model was retried or submitted")
	}
}
