package rtsp

import (
	"bytes"
	"fmt"
	"net"
	"strings"
	"sync"

	"github.com/bluenviron/gortsplib/v5"
	"github.com/bluenviron/gortsplib/v5/pkg/base"
	"github.com/bluenviron/gortsplib/v5/pkg/description"
	"github.com/bluenviron/gortsplib/v5/pkg/format"
	"github.com/bluenviron/gortsplib/v5/pkg/format/rtpsimpleaudio"
	"github.com/bluenviron/gortsplib/v5/pkg/liberrors"
	"github.com/pion/rtp"

	"github.com/onekvm/onekvm-core/pkg/av"
)

type Config struct {
	ListenAddress  string
	Port           int
	Path           string
	Codec          string
	Username       string
	Password       string
	OnDemandChange func(bool)
}

type rtpEncoder interface {
	Encode([][]byte) ([]*rtp.Packet, error)
}

// Sink exposes the already encoded pipeline output through RTSP.
type Sink struct {
	mu           sync.Mutex
	cfg          Config
	server       *gortsplib.Server
	stream       *gortsplib.ServerStream
	media        *description.Media
	audioMedia   *description.Media
	encoder      rtpEncoder
	audioEncoder *rtpsimpleaudio.Encoder
	started      bool
	timestamp    uint32
	audioTS      uint32
	readers      map[*gortsplib.ServerSession]struct{}
}

func NewSink(cfg Config) *Sink {
	return &Sink{cfg: cfg, readers: make(map[*gortsplib.ServerSession]struct{})}
}

func (s *Sink) Start() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.started {
		return nil
	}
	if err := ValidateConfig(s.cfg); err != nil {
		return err
	}
	s.cfg.Path = normalizePath(s.cfg.Path)

	var videoFormat format.Format
	var encoder rtpEncoder
	var err error
	switch strings.ToLower(s.cfg.Codec) {
	case "h264":
		f := &format.H264{PayloadTyp: 96, PacketizationMode: 1}
		videoFormat = f
		encoder, err = f.CreateEncoder()
	case "h265":
		f := &format.H265{PayloadTyp: 96}
		videoFormat = f
		encoder, err = f.CreateEncoder()
	}
	if err != nil {
		return fmt.Errorf("initialize RTP encoder: %w", err)
	}

	s.media = &description.Media{
		Type:    description.MediaTypeVideo,
		Formats: []format.Format{videoFormat},
	}
	opusFormat := &format.Opus{PayloadTyp: 111, ChannelCount: 2}
	audioEncoder, err := opusFormat.CreateEncoder()
	if err != nil {
		return fmt.Errorf("initialize Opus RTP encoder: %w", err)
	}
	s.audioMedia = &description.Media{
		Type:    description.MediaTypeAudio,
		Formats: []format.Format{opusFormat},
	}
	s.server = &gortsplib.Server{
		Handler:     s,
		RTSPAddress: net.JoinHostPort(s.cfg.ListenAddress, fmt.Sprint(s.cfg.Port)),
	}
	if err := s.server.Start(); err != nil {
		s.server = nil
		return fmt.Errorf("listen RTSP: %w", err)
	}
	s.stream = &gortsplib.ServerStream{
		Server: s.server,
		Desc:   &description.Session{Medias: []*description.Media{s.media, s.audioMedia}},
	}
	if err := s.stream.Initialize(); err != nil {
		s.server.Close()
		s.server = nil
		s.stream = nil
		return fmt.Errorf("initialize RTSP stream: %w", err)
	}
	s.encoder = encoder
	s.audioEncoder = audioEncoder
	s.started = true
	return nil
}

func ValidateConfig(cfg Config) error {
	if cfg.Port < 1 || cfg.Port > 65535 {
		return fmt.Errorf("RTSP port must be between 1 and 65535")
	}
	if (cfg.Username == "") != (cfg.Password == "") {
		return fmt.Errorf("RTSP username and password must be configured together")
	}
	switch strings.ToLower(cfg.Codec) {
	case "h264":
	case "h265":
	default:
		return fmt.Errorf("RTSP requires an active H.264 or H.265 encoder, got %q", cfg.Codec)
	}
	return nil
}

func (s *Sink) Stop() error {
	s.mu.Lock()
	if !s.started && s.server == nil {
		s.mu.Unlock()
		return nil
	}
	stream := s.stream
	server := s.server
	hadReaders := len(s.readers) != 0
	demandChange := s.cfg.OnDemandChange
	s.started = false
	s.stream = nil
	s.server = nil
	s.encoder = nil
	s.audioEncoder = nil
	s.media = nil
	s.audioMedia = nil
	s.readers = make(map[*gortsplib.ServerSession]struct{})
	s.mu.Unlock()
	if hadReaders && demandChange != nil {
		demandChange(false)
	}
	if stream != nil {
		stream.Close()
	}
	if server != nil {
		server.Close()
	}
	return nil
}

func (s *Sink) WriteVideo(payload []byte, duration float64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.started || s.stream == nil || len(s.readers) == 0 {
		return nil
	}
	nalus := splitAnnexB(payload)
	if len(nalus) == 0 {
		return nil
	}
	packets, err := s.encoder.Encode(nalus)
	if err != nil {
		return fmt.Errorf("packetize RTSP frame: %w", err)
	}
	if duration <= 0 || duration > 1 {
		duration = 1.0 / 60.0
	}
	s.timestamp += uint32(duration * 90000)
	for _, packet := range packets {
		packet.Timestamp = s.timestamp
		if err := s.stream.WritePacketRTP(s.media, packet); err != nil {
			return err
		}
	}
	return nil
}

func (s *Sink) WriteAudio(payload []byte, duration float64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.started || s.stream == nil || s.audioEncoder == nil || s.audioMedia == nil || len(s.readers) == 0 {
		return nil
	}
	packet, err := s.audioEncoder.Encode(payload)
	if err != nil {
		return fmt.Errorf("packetize RTSP audio: %w", err)
	}
	if duration <= 0 || duration > 1 {
		duration = 0.02
	}
	s.audioTS += uint32(duration * 48000)
	packet.Timestamp = s.audioTS
	return s.stream.WritePacketRTP(s.audioMedia, packet)
}

func (*Sink) OnControlMessage(func(av.ControlMsg)) {}

func (s *Sink) HasConsumers() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.started && len(s.readers) > 0
}

func (s *Sink) OnDescribe(ctx *gortsplib.ServerHandlerOnDescribeCtx) (*base.Response, *gortsplib.ServerStream, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.authorized(ctx.Conn, ctx.Request) {
		return &base.Response{StatusCode: base.StatusUnauthorized}, nil, liberrors.ErrServerAuth{}
	}
	if normalizePath(ctx.Path) != s.cfg.Path {
		return &base.Response{StatusCode: base.StatusNotFound}, nil, nil
	}
	return &base.Response{StatusCode: base.StatusOK}, s.stream, nil
}

func (s *Sink) OnSetup(ctx *gortsplib.ServerHandlerOnSetupCtx) (*base.Response, *gortsplib.ServerStream, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.authorized(ctx.Conn, ctx.Request) {
		return &base.Response{StatusCode: base.StatusUnauthorized}, nil, liberrors.ErrServerAuth{}
	}
	if normalizePath(ctx.Path) != s.cfg.Path {
		return &base.Response{StatusCode: base.StatusNotFound}, nil, nil
	}
	return &base.Response{StatusCode: base.StatusOK}, s.stream, nil
}

func (s *Sink) authorized(conn *gortsplib.ServerConn, request *base.Request) bool {
	return s.cfg.Username == "" || conn.VerifyCredentials(request, s.cfg.Username, s.cfg.Password)
}

func (s *Sink) OnPlay(ctx *gortsplib.ServerHandlerOnPlayCtx) (*base.Response, error) {
	s.mu.Lock()
	if normalizePath(ctx.Path) != s.cfg.Path {
		s.mu.Unlock()
		return &base.Response{StatusCode: base.StatusNotFound}, nil
	}
	firstReader := len(s.readers) == 0
	s.readers[ctx.Session] = struct{}{}
	demandChange := s.cfg.OnDemandChange
	s.mu.Unlock()
	if firstReader && demandChange != nil {
		demandChange(true)
	}
	return &base.Response{StatusCode: base.StatusOK}, nil
}

func (s *Sink) OnSessionClose(ctx *gortsplib.ServerHandlerOnSessionCloseCtx) {
	s.mu.Lock()
	_, existed := s.readers[ctx.Session]
	delete(s.readers, ctx.Session)
	lastReader := existed && len(s.readers) == 0
	demandChange := s.cfg.OnDemandChange
	s.mu.Unlock()
	if lastReader && demandChange != nil {
		demandChange(false)
	}
}

func normalizePath(value string) string {
	value = strings.TrimSpace(value)
	value = "/" + strings.Trim(value, "/")
	if value == "/" {
		return "/onekvm"
	}
	return value
}

func splitAnnexB(payload []byte) [][]byte {
	startCode := func(data []byte) (int, int) {
		for i := 0; i+3 <= len(data); i++ {
			if data[i] == 0 && data[i+1] == 0 {
				if data[i+2] == 1 {
					return i, 3
				}
				if i+4 <= len(data) && data[i+2] == 0 && data[i+3] == 1 {
					return i, 4
				}
			}
		}
		return -1, 0
	}

	first, prefix := startCode(payload)
	if first < 0 {
		if len(payload) == 0 {
			return nil
		}
		return [][]byte{payload}
	}
	payload = payload[first+prefix:]
	var nalus [][]byte
	for {
		next, nextPrefix := startCode(payload)
		if next < 0 {
			if nalu := bytes.TrimRight(payload, "\x00"); len(nalu) > 0 {
				nalus = append(nalus, nalu)
			}
			return nalus
		}
		if next > 0 {
			nalus = append(nalus, payload[:next])
		}
		payload = payload[next+nextPrefix:]
	}
}
