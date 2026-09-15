package official

import (
	"encoding/json"
	"testing"
)

func TestResponsesAPIRequestToAPIRequestConvertsFunctionTools(t *testing.T) {
	var req ResponsesAPIRequest
	body := []byte(`{
		"model":"gpt-5",
		"input":"What is the weather?",
		"tools":[{
			"type":"function",
			"name":"get_weather",
			"description":"Get the current weather",
			"parameters":{"type":"object","properties":{"city":{"type":"string"}},"required":["city"]},
			"strict":true
		}],
		"tool_choice":{"type":"function","name":"get_weather"},
		"parallel_tool_calls":false
	}`)
	if err := json.Unmarshal(body, &req); err != nil {
		t.Fatalf("unmarshal Responses request: %v", err)
	}

	got, err := req.ToAPIRequest()
	if err != nil {
		t.Fatalf("ToAPIRequest: %v", err)
	}
	if len(got.Tools) != 1 {
		t.Fatalf("tools len = %d, want 1", len(got.Tools))
	}
	tool := got.Tools[0]
	if tool.Type != "function" || tool.Function.Name != "get_weather" {
		t.Fatalf("tool = %#v", tool)
	}
	if tool.Function.Strict == nil || !*tool.Function.Strict {
		t.Fatalf("tool strict = %#v, want true", tool.Function.Strict)
	}
	if got.ToolChoice.ForcedFunctionName() != "get_weather" {
		t.Fatalf("forced function = %q", got.ToolChoice.ForcedFunctionName())
	}
	if got.ParallelToolCalls == nil || *got.ParallelToolCalls {
		t.Fatalf("parallel_tool_calls = %#v, want false", got.ParallelToolCalls)
	}
}

func TestResponsesAPIRequestApplyFunctionCallNames(t *testing.T) {
	req := ResponsesAPIRequest{
		Input: json.RawMessage(`[{"type":"function_call_output","call_id":"call_1","output":"ok"}]`),
	}
	req.ApplyFunctionCallNames(map[string]string{"call_1": "get_weather"})

	got, err := req.ToAPIRequest()
	if err != nil {
		t.Fatalf("ToAPIRequest: %v", err)
	}
	if len(got.Messages) != 1 || got.Messages[0].Name != "get_weather" {
		t.Fatalf("messages = %#v", got.Messages)
	}
}

func TestResponsesAPIRequestToAPIRequestConvertsFunctionCallHistory(t *testing.T) {
	var req ResponsesAPIRequest
	body := []byte(`{
		"model":"gpt-5",
		"input":[
			{"type":"function_call","id":"fc_1","call_id":"call_1","name":"get_weather","arguments":"{\"city\":\"Paris\"}"},
			{"type":"function_call_output","call_id":"call_1","output":"{\"temperature\":21}"}
		],
		"tools":[{"type":"function","name":"get_weather","parameters":{"type":"object"}}]
	}`)
	if err := json.Unmarshal(body, &req); err != nil {
		t.Fatalf("unmarshal Responses request: %v", err)
	}

	got, err := req.ToAPIRequest()
	if err != nil {
		t.Fatalf("ToAPIRequest: %v", err)
	}
	if len(got.Messages) != 2 {
		t.Fatalf("messages len = %d, want 2: %#v", len(got.Messages), got.Messages)
	}
	if got.Messages[0].Role != "assistant" || len(got.Messages[0].ToolCalls) != 1 {
		t.Fatalf("function_call message = %#v", got.Messages[0])
	}
	call := got.Messages[0].ToolCalls[0]
	if call.ID != "call_1" || call.Function.Name != "get_weather" || call.Function.Arguments != `{"city":"Paris"}` {
		t.Fatalf("function call = %#v", call)
	}
	if !got.Messages[1].IsToolResult() || got.Messages[1].ToolCallID != "call_1" || got.Messages[1].Text() != `{"temperature":21}` {
		t.Fatalf("function_call_output message = %#v", got.Messages[1])
	}
}
