package main

import (
	"fmt"
	"testing"
)

func TestAddSerialLogKeepsNewest100Entries(t *testing.T) {
	hub := NewHub()

	for i := 0; i < 101; i++ {
		hub.AddSerialLog(fmt.Sprintf("log-%d", i))
	}

	if got := len(hub.serialLogs); got != 100 {
		t.Fatalf("got %d serial logs, want 100", got)
	}
	if got := hub.serialLogs[0].Message; got != "log-1" {
		t.Errorf("oldest retained log = %q, want %q", got, "log-1")
	}
	if got := hub.serialLogs[99].Message; got != "log-100" {
		t.Errorf("newest retained log = %q, want %q", got, "log-100")
	}
}
