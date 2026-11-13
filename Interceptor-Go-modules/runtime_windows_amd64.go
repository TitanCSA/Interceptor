//go:build windows && amd64

package interceptor_sdk

import (
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"errors"
	"os"
	"path/filepath"
	"sync"
)

//go:embed native/InterceptorSDKBridge.dll
var nativeImage []byte
var runtimeOnce sync.Once
var runtimePath string
var runtimeError error

func bundledLibrary() (string, error) {
	runtimeOnce.Do(func() {
		cache, err := os.UserCacheDir()
		if err != nil {
			runtimeError = err
			return
		}
		sum := sha256.Sum256(nativeImage)
		dir := filepath.Join(cache, "TitanCSA", "interceptor", hex.EncodeToString(sum[:]))
		if err = os.MkdirAll(dir, 0700); err != nil {
			runtimeError = err
			return
		}
		path := filepath.Join(dir, "InterceptorSDKBridge.dll")
		if data, err := os.ReadFile(path); err != nil || sha256.Sum256(data) != sum {
			file, err := os.CreateTemp(dir, "native-*.tmp")
			if err != nil {
				runtimeError = err
				return
			}
			temp := file.Name()
			defer os.Remove(temp)
			_, writeErr := file.Write(nativeImage)
			closeErr := file.Close()
			if writeErr != nil {
				runtimeError = writeErr
				return
			}
			if closeErr != nil {
				runtimeError = closeErr
				return
			}
			if err = os.Rename(temp, path); err != nil {
				data, readErr := os.ReadFile(path)
				if readErr != nil || sha256.Sum256(data) != sum {
					runtimeError = err
					return
				}
			}
		}
		data, err := os.ReadFile(path)
		if err != nil {
			runtimeError = err
			return
		}
		if sha256.Sum256(data) != sum {
			runtimeError = errors.New("native SDK checksum mismatch")
			return
		}
		runtimePath = path
	})
	return runtimePath, runtimeError
}
