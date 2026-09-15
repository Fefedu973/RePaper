package rmchat

// These exact machine codes have unambiguous meanings. Unknown codes and all
// free-form messages remain generic errors; they are never exposed over IPC.
func knownRemoteErrorCode(code any) string {
	text, _ := code.(string)
	switch text {
	case "model_not_found", "model_not_available", "model_unavailable", "model_not_allowed", "model_access_denied", "model_permission_denied":
		return "MODEL_UNAVAILABLE"
	case "rate_limit_exceeded", "model_cap_exceeded":
		return "RATE_LIMITED"
	}
	return ""
}

func explicitRemoteError(object map[string]any) string {
	if code := knownRemoteErrorCode(object["code"]); code != "" {
		return code
	}
	for _, key := range []string{"error", "detail"} {
		if nested, ok := object[key].(map[string]any); ok {
			if code := knownRemoteErrorCode(nested["code"]); code != "" {
				return code
			}
		}
	}
	if value, present := object["error"]; present && value != nil && value != false {
		return "UPSTREAM_ERROR"
	}
	return ""
}
