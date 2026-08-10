module github.com/onekvm/onekvm-extension-rtsp

go 1.25.0

require (
	github.com/bluenviron/gortsplib/v5 v5.6.2
	github.com/onekvm/onekvm-core v0.0.0
	github.com/pion/rtp v1.10.5
	github.com/rs/zerolog v1.33.0
)

require (
	github.com/bluenviron/mediacommon/v2 v2.9.2 // indirect
	github.com/google/uuid v1.6.0 // indirect
	github.com/gorilla/websocket v1.5.3 // indirect
	github.com/mattn/go-colorable v0.1.13 // indirect
	github.com/mattn/go-isatty v0.0.20 // indirect
	github.com/pion/logging v0.2.4 // indirect
	github.com/pion/randutil v0.1.0 // indirect
	github.com/pion/rtcp v1.2.17 // indirect
	github.com/pion/sdp/v3 v3.0.19 // indirect
	github.com/pion/srtp/v3 v3.0.12 // indirect
	github.com/pion/transport/v4 v4.0.2 // indirect
	golang.org/x/net v0.57.0 // indirect
	golang.org/x/sys v0.47.0 // indirect
)

replace github.com/onekvm/onekvm-core => ../onekvm-core
