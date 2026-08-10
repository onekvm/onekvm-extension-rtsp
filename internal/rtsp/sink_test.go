package rtsp

import (
	"bufio"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/bluenviron/gortsplib/v5"
)

func TestSplitAnnexB(t *testing.T) {
	payload := []byte{0, 0, 0, 1, 0x67, 0x01, 0, 0, 1, 0x68, 0x02, 0, 0}
	nalus := splitAnnexB(payload)
	if len(nalus) != 2 || string(nalus[0]) != string([]byte{0x67, 0x01}) || string(nalus[1]) != string([]byte{0x68, 0x02}) {
		t.Fatalf("NAL units = %#v", nalus)
	}
}

func TestValidateConfigAllowsOptionalCredentials(t *testing.T) {
	base := Config{ListenAddress: "0.0.0.0", Port: 8554, Path: "/onekvm", Codec: "h264"}
	if err := ValidateConfig(base); err != nil {
		t.Fatal(err)
	}
	base.Username = "viewer"
	if err := ValidateConfig(base); err == nil || !strings.Contains(err.Error(), "configured together") {
		t.Fatalf("partial credentials error = %v", err)
	}
	base.Password = "secret"
	if err := ValidateConfig(base); err != nil {
		t.Fatal(err)
	}
	base.Codec = "mjpeg"
	if err := ValidateConfig(base); err == nil || !strings.Contains(err.Error(), "H.264 or H.265") {
		t.Fatalf("unsupported codec error = %v", err)
	}
}

func TestRTSPDescribeAndAuthentication(t *testing.T) {
	tests := []struct {
		name       string
		username   string
		password   string
		wantStatus string
	}{
		{name: "local without auth", wantStatus: "200"},
		{name: "credentials required", username: "viewer", password: "secret", wantStatus: "401"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			port := availablePort(t)
			sink := NewSink(Config{
				ListenAddress: "127.0.0.1", Port: port, Path: "/onekvm", Codec: "h264",
				Username: test.username, Password: test.password,
			})
			if err := sink.Start(); err != nil {
				t.Fatal(err)
			}
			defer sink.Stop()

			conn, err := net.DialTimeout("tcp", fmt.Sprintf("127.0.0.1:%d", port), time.Second)
			if err != nil {
				t.Fatal(err)
			}
			defer conn.Close()
			_ = conn.SetDeadline(time.Now().Add(2 * time.Second))
			_, err = fmt.Fprintf(conn, "DESCRIBE rtsp://127.0.0.1:%d/onekvm RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n", port)
			if err != nil {
				t.Fatal(err)
			}
			status, err := bufio.NewReader(conn).ReadString('\n')
			if err != nil {
				t.Fatal(err)
			}
			if !strings.Contains(status, "RTSP/1.0 "+test.wantStatus) {
				t.Fatalf("status = %q", status)
			}
		})
	}
}

func TestRTSPDemandTracksFirstAndLastReader(t *testing.T) {
	var changes []bool
	sink := NewSink(Config{
		Path:           "/onekvm",
		OnDemandChange: func(active bool) { changes = append(changes, active) },
	})
	first := new(gortsplib.ServerSession)
	second := new(gortsplib.ServerSession)
	for _, session := range []*gortsplib.ServerSession{first, second} {
		response, err := sink.OnPlay(&gortsplib.ServerHandlerOnPlayCtx{Session: session, Path: "/onekvm"})
		if err != nil || response.StatusCode != 200 {
			t.Fatalf("OnPlay response = %+v, err = %v", response, err)
		}
	}
	if len(changes) != 1 || !changes[0] {
		t.Fatalf("reader start demand changes = %v", changes)
	}
	sink.OnSessionClose(&gortsplib.ServerHandlerOnSessionCloseCtx{Session: first})
	if len(changes) != 1 {
		t.Fatalf("non-final reader close demand changes = %v", changes)
	}
	sink.OnSessionClose(&gortsplib.ServerHandlerOnSessionCloseCtx{Session: second})
	if len(changes) != 2 || changes[1] {
		t.Fatalf("final reader close demand changes = %v", changes)
	}
}

func availablePort(t *testing.T) int {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	port := listener.Addr().(*net.TCPAddr).Port
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	return port
}
