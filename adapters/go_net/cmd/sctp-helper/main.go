package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
)

const sctpMsgNotification = 0x8000

type initOptions struct {
	numOStreams    int
	maxInStreams   int
	maxAttempts    int
	maxInitTimeout int
}

type messageSpec struct {
	payload string
	stream  uint16
	ppid    uint32
}

type helperOptions struct {
	mode          string
	bindAddrs     []string
	connectAddrs  []string
	subscribe     []string
	readMessages  int
	setNoDelay    bool
	emitLocal     bool
	emitPeer      bool
	expectFailure string
	messages      []messageSpec
	init          initOptions
}

func main() {
	if len(os.Args) < 2 {
		fatalf("missing mode")
	}
	opts, err := parseArgs(os.Args[1:])
	if err != nil {
		fatalf("%v", err)
	}
	switch opts.mode {
	case "server":
		if err := runServer(opts); err != nil {
			emit(map[string]any{"event": "error", "message": err.Error()})
			os.Exit(1)
		}
	case "client":
		if err := runClient(opts); err != nil {
			emit(map[string]any{"event": "error", "message": err.Error()})
			os.Exit(1)
		}
	default:
		fatalf("unsupported mode %q", opts.mode)
	}
}

func parseArgs(args []string) (helperOptions, error) {
	opts := helperOptions{mode: args[0]}
	fs := flag.NewFlagSet(opts.mode, flag.ContinueOnError)
	var bindAddrs string
	var connectAddrs string
	var subscribe string
	var messages string
	fs.StringVar(&bindAddrs, "bind-addrs", "", "")
	fs.StringVar(&connectAddrs, "connect-addrs", "", "")
	fs.StringVar(&subscribe, "subscribe", "", "")
	fs.StringVar(&messages, "messages", "", "")
	fs.IntVar(&opts.readMessages, "read-messages", 0, "")
	fs.BoolVar(&opts.setNoDelay, "set-nodelay", false, "")
	fs.BoolVar(&opts.emitLocal, "emit-local-addrs", false, "")
	fs.BoolVar(&opts.emitPeer, "emit-peer-addrs", false, "")
	fs.StringVar(&opts.expectFailure, "expect-failure", "", "")
	fs.IntVar(&opts.init.numOStreams, "init-ostreams", 0, "")
	fs.IntVar(&opts.init.maxInStreams, "init-instreams", 0, "")
	fs.IntVar(&opts.init.maxAttempts, "init-attempts", 0, "")
	fs.IntVar(&opts.init.maxInitTimeout, "init-timeout", 0, "")
	if err := fs.Parse(args[1:]); err != nil {
		return opts, err
	}
	opts.bindAddrs = splitCSV(bindAddrs)
	opts.connectAddrs = splitCSV(connectAddrs)
	opts.subscribe = splitCSV(subscribe)
	parsedMessages, err := parseMessages(messages)
	if err != nil {
		return opts, err
	}
	opts.messages = parsedMessages
	return opts, nil
}

func runServer(opts helperOptions) error {
	conn, err := listenConn(opts)
	if err != nil {
		return err
	}
	defer conn.Close()
	if err := subscribe(conn, opts.subscribe); err != nil {
		return err
	}
	localAddrs, _ := conn.LocalAddrs()
	if len(localAddrs) == 0 {
		if single, ok := conn.LocalAddr().(*net.SCTPAddr); ok && single != nil {
			localAddrs = []net.SCTPAddr{*single}
		}
	}
	emit(map[string]any{"event": "ready", "local_addrs": formatSCTPAddrs(localAddrs)})
	buf := make([]byte, 8192)
	var emittedPeer bool
	recvCount := 0
	for recvCount < opts.readMessages {
		n, _, flags, _, info, err := conn.ReadFromSCTP(buf)
		if err != nil {
			return err
		}
		if flags&sctpMsgNotification != 0 {
			emit(map[string]any{"event": "notify", "flags": flags})
			continue
		}
		stream := -1
		ppid := 0
		assocID := 0
		infoPresent := false
		if info != nil {
			stream = int(info.Stream)
			ppid = int(info.PPID)
			assocID = int(info.AssocID)
			infoPresent = true
		}
		emit(map[string]any{
			"event":        "recv",
			"payload":      string(buf[:n]),
			"stream":       stream,
			"ppid":         ppid,
			"assoc_id":     assocID,
			"info_present": infoPresent,
		})
		recvCount++
		if opts.emitPeer && !emittedPeer {
			if addrs, err := conn.PeerAddrs(); err == nil {
				emit(map[string]any{"event": "peer_addrs", "addrs": formatSCTPAddrs(addrs)})
				emittedPeer = true
			}
		}
	}
	emit(map[string]any{"event": "complete", "recv_count": recvCount})
	return nil
}

func runClient(opts helperOptions) error {
	conn, err := dialConn(opts)
	if err != nil {
		if opts.expectFailure == "connect" || opts.expectFailure == "connect_or_send" {
			emit(map[string]any{"event": "expected_failure", "stage": "connect", "message": err.Error()})
			return nil
		}
		return err
	}
	defer conn.Close()
	if opts.expectFailure == "connect" {
		return errors.New("connect succeeded unexpectedly")
	}
	if opts.setNoDelay {
		if err := conn.SetNoDelay(true); err != nil {
			return err
		}
	}
	if hasInit(opts.init) {
		if err := conn.SetInitOptions(net.SCTPInitOptions{
			NumOStreams:    uint16(opts.init.numOStreams),
			MaxInStreams:   uint16(opts.init.maxInStreams),
			MaxAttempts:    uint16(opts.init.maxAttempts),
			MaxInitTimeout: uint16(opts.init.maxInitTimeout),
		}); err != nil {
			return err
		}
	}
	if err := subscribe(conn, opts.subscribe); err != nil {
		return err
	}
	if opts.emitLocal {
		if addrs, err := conn.LocalAddrs(); err == nil {
			emit(map[string]any{"event": "local_addrs", "addrs": formatSCTPAddrs(addrs)})
		}
	}
	if opts.emitPeer {
		if addrs, err := conn.PeerAddrs(); err == nil {
			emit(map[string]any{"event": "peer_addrs", "addrs": formatSCTPAddrs(addrs)})
		}
	}
	for _, message := range opts.messages {
		_, err := conn.WriteToSCTP([]byte(message.payload), nil, &net.SCTPSndInfo{
			Stream: message.stream,
			PPID:   message.ppid,
		})
		if err != nil {
			if opts.expectFailure == "send" || opts.expectFailure == "connect_or_send" {
				emit(map[string]any{"event": "expected_failure", "stage": "send", "message": err.Error()})
				return nil
			}
			return err
		}
		emit(map[string]any{
			"event":   "sent",
			"payload": message.payload,
			"stream":  int(message.stream),
			"ppid":    int(message.ppid),
		})
	}
	if opts.expectFailure == "send" || opts.expectFailure == "connect_or_send" {
		return errors.New("send succeeded unexpectedly")
	}
	emit(map[string]any{"event": "complete", "sent_count": len(opts.messages)})
	return nil
}

func listenConn(opts helperOptions) (*net.SCTPConn, error) {
	if len(opts.bindAddrs) == 0 {
		return nil, errors.New("missing bind-addrs")
	}
	if len(opts.bindAddrs) == 1 {
		addr, err := net.ResolveSCTPAddr("sctp4", opts.bindAddrs[0])
		if err != nil {
			return nil, err
		}
		if hasInit(opts.init) {
			return net.ListenSCTPInit("sctp4", addr, net.SCTPInitOptions{
				NumOStreams:    uint16(opts.init.numOStreams),
				MaxInStreams:   uint16(opts.init.maxInStreams),
				MaxAttempts:    uint16(opts.init.maxAttempts),
				MaxInitTimeout: uint16(opts.init.maxInitTimeout),
			})
		}
		return net.ListenSCTP("sctp4", addr)
	}
	addrs, err := net.ResolveSCTPMultiAddr("sctp4", opts.bindAddrs)
	if err != nil {
		return nil, err
	}
	if hasInit(opts.init) {
		return net.ListenSCTPMultiInit("sctp4", addrs, net.SCTPInitOptions{
			NumOStreams:    uint16(opts.init.numOStreams),
			MaxInStreams:   uint16(opts.init.maxInStreams),
			MaxAttempts:    uint16(opts.init.maxAttempts),
			MaxInitTimeout: uint16(opts.init.maxInitTimeout),
		})
	}
	return net.ListenSCTPMulti("sctp4", addrs)
}

func dialConn(opts helperOptions) (*net.SCTPConn, error) {
	if len(opts.connectAddrs) == 0 {
		return nil, errors.New("missing connect-addrs")
	}
	if len(opts.connectAddrs) == 1 {
		addr, err := net.ResolveSCTPAddr("sctp4", opts.connectAddrs[0])
		if err != nil {
			return nil, err
		}
		return net.DialSCTP("sctp4", nil, addr)
	}
	addrs, err := net.ResolveSCTPMultiAddr("sctp4", opts.connectAddrs)
	if err != nil {
		return nil, err
	}
	return net.DialSCTPMulti("sctp4", nil, addrs)
}

func subscribe(conn *net.SCTPConn, names []string) error {
	if len(names) == 0 {
		return nil
	}
	mask := net.SCTPEventMask{}
	for _, name := range names {
		switch name {
		case "association":
			mask.Association = true
		case "shutdown":
			mask.Shutdown = true
		case "dataio":
			mask.DataIO = true
		default:
			return fmt.Errorf("unsupported subscription %q", name)
		}
	}
	return conn.SubscribeEvents(mask)
}

func hasInit(init initOptions) bool {
	return init.numOStreams != 0 || init.maxInStreams != 0 || init.maxAttempts != 0 || init.maxInitTimeout != 0
}

func splitCSV(raw string) []string {
	if raw == "" {
		return nil
	}
	parts := strings.Split(raw, ",")
	out := make([]string, 0, len(parts))
	for _, part := range parts {
		part = strings.TrimSpace(part)
		if part != "" {
			out = append(out, part)
		}
	}
	return out
}

func parseMessages(raw string) ([]messageSpec, error) {
	if raw == "" {
		return nil, nil
	}
	var out []messageSpec
	for _, item := range splitCSV(raw) {
		parts := strings.Split(item, ":")
		if len(parts) != 3 {
			return nil, fmt.Errorf("invalid message spec %q", item)
		}
		stream, err := strconv.Atoi(parts[1])
		if err != nil {
			return nil, err
		}
		ppid, err := strconv.Atoi(parts[2])
		if err != nil {
			return nil, err
		}
		out = append(out, messageSpec{
			payload: parts[0],
			stream:  uint16(stream),
			ppid:    uint32(ppid),
		})
	}
	return out, nil
}

func formatSCTPAddrs(addrs []net.SCTPAddr) []string {
	out := make([]string, 0, len(addrs))
	for _, addr := range addrs {
		out = append(out, addr.String())
	}
	return out
}

func emit(event map[string]any) {
	_ = json.NewEncoder(os.Stdout).Encode(event)
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", args...)
	os.Exit(2)
}
