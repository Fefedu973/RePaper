//go:build !linux

package rmchat

import "os"

func openUploadFile(root *os.Root, path string) (*os.File, error) { return root.Open(path) }
