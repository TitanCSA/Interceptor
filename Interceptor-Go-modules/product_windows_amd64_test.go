//go:build windows && amd64

package interceptor_sdk

import (
	"errors"
	"os"
	"testing"
	"time"
)

func TestProductNativeContract(t *testing.T) {
	fixture := os.Getenv("SDK_TEST_FIXTURE")
	if fixture == "" {
		t.Skip("synthetic fixture path required")
	}
	client, err := New(fixture)
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	mode, err := client.dll.FindProc("interceptor_test_mode")
	if err != nil {
		t.Fatal("refuse driver operations without a synthetic backend")
	}
	allowed := map[string]bool{"keyboard": true, "mouse": true, "hid": true, "xinput": true, "xinputhid": true, "gip": true}
	if _, err = client.Initialize(); err != nil {
		t.Fatal(err)
	}
	info, err := client.Info()
	if err != nil || info.InputMask != 3 || info.ControllerMask != 15 {
		t.Fatal("product mask mismatch", err)
	}
	devices, err := client.Devices()
	if err != nil || len(devices) != len(allowed) {
		t.Fatal("device mismatch", err)
	}
	batch, err := client.Receive(0)
	if err != nil || len(batch.Inputs) != len(allowed) {
		t.Fatal("batch mismatch", err)
	}
	for _, event := range batch.Inputs {
		if !allowed[event.Kind] || event.Sequence <= 1<<63 {
			t.Fatal("family or uint64 lost")
		}
		if event.Kind == "keyboard" {
			event.State["scan_code"] = 0x30
		}
		if event.Kind == "mouse" {
			event.State["y"] = -7
		}
	}
	if n, e := client.Send(batch); e != nil || int(n) != len(allowed) {
		t.Fatal("send", n, e)
	}
	if n, e := client.Send(batch); e != nil || n != 0 {
		t.Fatal("duplicate send", n, e)
	}
	current, err := client.Receive(0)
	if err != nil {
		t.Fatal(err)
	}
	if _, err = client.Send(batch); err == nil {
		t.Fatal("stale batch accepted")
	}
	current.Inputs[0].Blocked = true
	if n, e := client.Send(current); e != nil || int(n) != len(allowed)-1 {
		t.Fatal("block", n, e)
	}
	retry, err := client.Receive(0)
	if err != nil {
		t.Fatal(err)
	}
	mode.Call(2)
	_, err = client.Send(retry)
	var nativeErr *Error
	if !errors.As(err, &nativeErr) || nativeErr.Code != 170 || nativeErr.Accepted != 1 {
		t.Fatal("partial send", err)
	}
	if n, e := client.Send(retry); e != nil || int(n) != len(allowed)-1 {
		t.Fatal("retry", n, e)
	}
	mode.Call(0)
	discard, err := client.Receive(0)
	if err != nil {
		t.Fatal(err)
	}
	if err = client.Discard(discard); err != nil {
		t.Fatal(err)
	}
	for _, kind := range []string{"keyboard", "mouse", "hid", "xinput", "xinputhid", "gip"} {
		if !allowed[kind] {
			_, err = client.Inject(kind, map[string]int64{})
			if !errors.As(err, &nativeErr) || nativeErr.Code != 50 {
				t.Fatal("unsupported", err)
			}
		}
	}
	mode.Call(1)
	done := make(chan error, 1)
	go func() {
		b, e := client.Receive(2000)
		if e == nil && len(b.Inputs) != 0 {
			e = errors.New("expected cancelled empty batch")
		}
		done <- e
	}()
	time.Sleep(30 * time.Millisecond)
	if err = client.Cancel(); err != nil {
		t.Fatal(err)
	}
	select {
	case err = <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("cancel did not interrupt wait")
	}
	mode.Call(0)
	client.Close()
	production, err := New()
	if err != nil {
		t.Fatal(err)
	}
	production.Close()
}
