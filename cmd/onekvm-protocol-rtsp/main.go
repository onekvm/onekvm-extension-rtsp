package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"github.com/rs/zerolog/log"

	"github.com/onekvm/onekvm-core/pkg/extensionruntime"
	"github.com/onekvm/onekvm-core/pkg/protocolipc"
	"github.com/onekvm/onekvm-extension-rtsp/internal/rtsp"
)

const defaultConfigPath = "/run/onekvm/extensions/rtsp/config.json"

type config struct {
	ServiceEnabled *bool  `json:"service_enabled"`
	ListenAddress  string `json:"listen_address"`
	Port           int    `json:"port"`
	Path           string `json:"path"`
	Username       string `json:"username"`
	Password       string `json:"password"`
	Codec          string `json:"codec"`
	FPS            int    `json:"fps"`
	MediaSocket    string `json:"media_socket"`
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		log.Fatal().Err(err).Msg("RTSP protocol process failed")
	}
}

func run(args []string) error {
	flags := flag.NewFlagSet("onekvm-protocol-rtsp", flag.ContinueOnError)
	configPath := flags.String("config", extensionConfigPath(defaultConfigPath), "protocol configuration path")
	health := flags.Bool("health", false, "validate configuration and exit")
	lifecycle := flags.String("lifecycle", "", "handle an enable or disable lifecycle notification")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return fmt.Errorf("unexpected arguments: %v", flags.Args())
	}
	var cfg config
	if err := extensionruntime.LoadConfig(*configPath, &cfg); err != nil && !(*health && errors.Is(err, os.ErrNotExist)) {
		return err
	}
	applyRTSPDefaults(&cfg)
	if err := validateRTSPConfig(cfg); err != nil {
		return err
	}
	if *health {
		return nil
	}
	if *lifecycle != "" {
		if *lifecycle != "enable" && *lifecycle != "disable" {
			return fmt.Errorf("unsupported lifecycle action %q", *lifecycle)
		}
		return nil
	}
	if !serviceEnabled(cfg) {
		return nil
	}
	identity, err := extensionruntime.LoadIdentity()
	if err != nil {
		return err
	}
	infoCtx, cancelInfo := context.WithTimeout(context.Background(), 3*time.Second)
	info, err := protocolipc.QueryVideoInfo(infoCtx, cfg.MediaSocket, protocolipc.SubscriptionRequest{
		ExtensionID: identity.ExtensionID,
		Token:       identity.Token,
	})
	cancelInfo()
	if err != nil {
		return fmt.Errorf("query primary video stream: %w", err)
	}
	cfg.Codec = info.Codec
	if err := validateRTSPConfig(cfg); err != nil {
		return err
	}

	var subscriber *extensionruntime.MediaSubscriber
	fatal := make(chan error, 1)
	sink := rtsp.NewSink(rtsp.Config{
		ListenAddress: cfg.ListenAddress,
		Port:          cfg.Port,
		Path:          cfg.Path,
		Username:      cfg.Username,
		Password:      cfg.Password,
		Codec:         cfg.Codec,
		OnDemandChange: func(active bool) {
			subscriber.SetDemand(active)
		},
	})
	subscriber = &extensionruntime.MediaSubscriber{
		SocketPath:  cfg.MediaSocket,
		ExtensionID: identity.ExtensionID,
		Token:       identity.Token,
		Video:       "encoded",
		FPS:         cfg.FPS,
		Consume: func(frame protocolipc.Frame) error {
			if frame.Codec != cfg.Codec {
				return fmt.Errorf("%w from %s to %s", errCodecChanged, cfg.Codec, frame.Codec)
			}
			return sink.WriteVideo(frame.Payload, float64(frame.DurationUS)/1_000_000)
		},
		OnError: func(err error) {
			if errors.Is(err, errCodecChanged) {
				select {
				case fatal <- err:
				default:
				}
				return
			}
			log.Warn().Err(err).Msg("RTSP media subscription interrupted")
		},
	}
	if err := sink.Start(); err != nil {
		return err
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	var runErr error
	select {
	case <-ctx.Done():
	case runErr = <-fatal:
	}
	if err := sink.Stop(); err != nil {
		return err
	}
	subscriber.Close()
	return runErr
}

func serviceEnabled(cfg config) bool {
	return cfg.ServiceEnabled == nil || *cfg.ServiceEnabled
}

var errCodecChanged = errors.New("primary video codec changed")

func extensionConfigPath(fallback string) string {
	if root := os.Getenv("ONEKVM_EXTENSION_CONFIG"); root != "" {
		return filepath.Join(root, "config.json")
	}
	return fallback
}

func applyRTSPDefaults(cfg *config) {
	if cfg.ListenAddress == "" {
		cfg.ListenAddress = "127.0.0.1"
	}
	if cfg.Port == 0 {
		cfg.Port = 8554
	}
	if cfg.Path == "" {
		cfg.Path = "/onekvm"
	}
	if cfg.Codec == "" {
		cfg.Codec = "h264"
	}
	if cfg.FPS <= 0 {
		cfg.FPS = 60
	}
	if cfg.MediaSocket == "" {
		cfg.MediaSocket = protocolipc.DefaultMediaSocket
	}
}

func validateRTSPConfig(cfg config) error {
	if err := rtsp.ValidateConfig(rtsp.Config{
		ListenAddress: cfg.ListenAddress, Port: cfg.Port, Path: cfg.Path,
		Username: cfg.Username, Password: cfg.Password, Codec: cfg.Codec,
	}); err != nil {
		return err
	}
	if !filepath.IsAbs(cfg.MediaSocket) {
		return fmt.Errorf("RTSP media socket path must be absolute")
	}
	if cfg.FPS < 1 || cfg.FPS > 60 {
		return fmt.Errorf("RTSP FPS must be between 1 and 60")
	}
	return nil
}
