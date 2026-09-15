//go:build linux

package rmchat

import (
	"os"
	"syscall"
)

// A non-regular file must not block before Stat can reject it (notably FIFOs).
// OpenRoot continues to enforce containment for every path component.
func openUploadFile(root *os.Root, path string) (*os.File, error) {
	return root.OpenFile(path, os.O_RDONLY|syscall.O_NONBLOCK, 0)
}
