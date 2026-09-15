package official

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestNewChatCompletionWithMetadata(t *testing.T) {
	response := NewChatCompletionWithMetadata(
		"hello",
		1,
		2,
		"gpt-4o",
		"conv-xxx",
		[]map[string]interface{}{
			{
				"event":      "artifact",
				"kind":       "generated_image",
				"slot_index": 1,
				"url":        "http://example.test/image.png",
			},
			{
				"event":      "artifact_slot_final",
				"kind":       "generated_image",
				"slot_index": 1,
			},
		},
	)

	data, err := json.Marshal(response)
	if err != nil {
		t.Fatalf("marshal response: %v", err)
	}
	var raw map[string]interface{}
	if err := json.Unmarshal(data, &raw); err != nil {
		t.Fatalf("unmarshal response: %v", err)
	}

	if raw["conversation_id"] != "conv-xxx" {
		t.Fatalf("conversation_id = %#v, want conv-xxx", raw["conversation_id"])
	}
	choices := raw["choices"].([]interface{})
	message := choices[0].(map[string]interface{})["message"].(map[string]interface{})
	if message["content"] != "hello" {
		t.Fatalf("message content = %#v, want hello", message["content"])
	}
	sentinel := raw["sentinel"].([]interface{})
	if len(sentinel) != 2 {
		t.Fatalf("sentinel count = %d, want 2", len(sentinel))
	}
	if sentinel[0].(map[string]interface{})["event"] != "artifact" {
		t.Fatalf("first sentinel = %#v, want artifact", sentinel[0])
	}
}

func TestNewResponsesResponseWithReasoning(t *testing.T) {
	resp := NewResponsesResponse("hello", "thinking...", 100, 50, 30, 80, 20, "auto")
	if resp.Object != "response" {
		t.Fatalf("object = %q, want response", resp.Object)
	}
	if resp.OutputText != "hello" {
		t.Fatalf("output_text = %q, want hello", resp.OutputText)
	}
	b, _ := json.Marshal(resp)
	s := string(b)
	for _, want := range []string{
		`"input_tokens":100`,
		`"cached_tokens":80`,
		`"cache_write_tokens":20`,
		`"reasoning_tokens":30`,
		`"type":"reasoning"`,
		`"reasoning_text"`,
		`"reasoning_content":"thinking..."`,
	} {
		if !strings.Contains(s, want) {
			t.Fatalf("missing %q in %s", want, s)
		}
	}
}

func TestNewResponsesResponseWithToolCalls(t *testing.T) {
	calls := []ToolCall{{
		ID:   "call_123",
		Type: "function",
		Function: ToolCallFunc{
			Name:      "get_weather",
			Arguments: `{"city":"Paris"}`,
		},
	}}

	resp := NewResponsesResponseWithToolCalls("ignored", "", calls, 10, 5, 0, 0, 0, "gpt-5")
	if resp.OutputText != "" {
		t.Fatalf("output_text = %q, want empty", resp.OutputText)
	}
	if len(resp.Output) != 1 {
		t.Fatalf("output len = %d, want 1", len(resp.Output))
	}
	item := resp.Output[0]
	if item.Type != "function_call" || item.CallID != "call_123" || item.Name != "get_weather" || item.Arguments != `{"city":"Paris"}` {
		t.Fatalf("function call item = %#v", item)
	}
}

func TestResponsesFunctionCallStreamEvents(t *testing.T) {
	call := ToolCall{
		ID:   "call_123",
		Type: "function",
		Function: ToolCallFunc{
			Name:      "get_weather",
			Arguments: `{"city":"Paris"}`,
		},
	}

	added := ResponsesFunctionCallAddedEvent(0, "fc_123", call)
	if added.Item.Type != "function_call" || added.Item.CallID != "call_123" || added.Item.Name != "get_weather" || added.Item.Arguments != "" {
		t.Fatalf("added item = %#v", added.Item)
	}
	delta := ResponsesFunctionCallArgumentsDeltaEvent(0, "fc_123", call.Function.Arguments)
	if delta.Type != "response.function_call_arguments.delta" || delta.Delta != call.Function.Arguments {
		t.Fatalf("delta event = %#v", delta)
	}
	done := ResponsesFunctionCallArgumentsDoneEvent(0, "fc_123", call)
	if done.CallID != "call_123" || done.Name != "get_weather" || done.Arguments != call.Function.Arguments {
		t.Fatalf("done event = %#v", done)
	}
	itemDone := ResponsesFunctionCallDoneEvent(0, "fc_123", call)
	if itemDone.Item.Status != "completed" || itemDone.Item.Arguments != call.Function.Arguments {
		t.Fatalf("item done event = %#v", itemDone)
	}
}

func TestNewResponsesResponseWithoutReasoning(t *testing.T) {
	resp := NewResponsesResponse("hi", "", 10, 5, 0, 0, 0, "auto")
	if len(resp.Output) != 1 {
		t.Fatalf("output len = %d, want 1", len(resp.Output))
	}
	if resp.Output[0].Type != "message" {
		t.Fatalf("first output type = %q, want message", resp.Output[0].Type)
	}
}
