// Package device is the device's side of the protocol over HTTP. The simulator
// and the server's tests use it; it is also the reference for what the firmware
// has to do, step by step.
package device

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"

	"rs232c2/internal/proto"
)

var (
	// ErrServerKey: the server does not have the key the enrollment token names.
	ErrServerKey = errors.New("server key does not match the enrollment token")
	// ErrRehandshake: the server does not know the session (any more).
	ErrRehandshake = errors.New("session expired")
	// ErrRefused: the server does not accept this device or token.
	ErrRefused = errors.New("refused by the server")
)

// State is everything a device has to remember between restarts.
type State struct {
	Key       []byte `json:"key"`        // own static private key (proto.StaticKey.Marshal)
	URL       string `json:"url"`        // server, from the enrollment token
	ServerPub []byte `json:"server_pub"` // pinned when enrolling
}

type Command struct {
	ID   uint64          `json:"id"`
	Type string          `json:"type"`
	Args json.RawMessage `json:"args,omitempty"`
}

type Result struct {
	ID  uint64 `json:"id"`
	OK  bool   `json:"ok"`
	Out any    `json:"out,omitempty"`
}

type Reply struct {
	State string    `json:"state"`
	Cmds  []Command `json:"cmds"`
	PollS int       `json:"poll_s"`
}

type Client struct {
	HTTP  *http.Client
	State State
	key   *proto.StaticKey
	sess  *proto.Session
}

// New returns a client for st, creating the device key if there is none yet.
func New(hc *http.Client, st State) (*Client, error) {
	c := &Client{HTTP: hc, State: st}
	var err error
	if st.Key == nil {
		if c.key, err = proto.GenerateStatic(); err != nil {
			return nil, err
		}
		c.State.Key = c.key.Marshal()
	} else if c.key, err = proto.ParseStatic(st.Key); err != nil {
		return nil, err
	}
	return c, nil
}

func (c *Client) ID() string { return proto.IDString(proto.DeviceID(c.key.Public())) }

func (c *Client) post(path string, body []byte, limit int64) ([]byte, error) {
	resp, err := c.HTTP.Post(strings.TrimRight(c.State.URL, "/")+path, "application/octet-stream", bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()
	switch resp.StatusCode {
	case http.StatusOK:
	case http.StatusUnauthorized:
		return nil, ErrRehandshake
	case http.StatusForbidden:
		return nil, ErrRefused
	default:
		return nil, fmt.Errorf("server answered %s", resp.Status)
	}
	return io.ReadAll(io.LimitReader(resp.Body, limit))
}

func (c *Client) handshake(kind byte, tokenID [proto.TokenIDSize]byte, psk []byte) error {
	hs, req, err := proto.NewClientHandshake(kind, c.key, c.State.ServerPub, tokenID, psk)
	if err != nil {
		return err
	}
	resp, err := c.post("/v1/handshake", req, proto.RespSize+1)
	if err != nil {
		return err
	}
	c.sess, err = hs.Finish(resp)
	return err
}

func (c *Client) exchange(msg any) (*Reply, error) {
	b, err := json.Marshal(msg)
	if err != nil {
		return nil, err
	}
	rec, err := c.sess.Seal(b)
	if err != nil {
		return nil, err
	}
	resp, err := c.post("/v1/msg", append(c.sess.ID[:], rec...), proto.MaxPlaintext+64)
	if err != nil {
		return nil, err
	}
	pt, err := c.sess.Open(resp)
	if err != nil {
		return nil, err
	}
	var r Reply
	return &r, json.Unmarshal(pt, &r)
}

// Enroll registers the device with the server named in the token and returns
// the six-digit code that an administrator has to confirm. The server's key is
// fetched, checked against the token's fingerprint and pinned from then on.
func (c *Client) Enroll(token, name string, info any) (sas string, err error) {
	t, err := proto.ParseEnrollToken(token)
	if err != nil {
		return "", err
	}
	resp, err := c.HTTP.Get(strings.TrimRight(t.URL, "/") + "/v1/server-key")
	if err != nil {
		return "", err
	}
	pub, err := io.ReadAll(io.LimitReader(resp.Body, proto.PubSize+1))
	resp.Body.Close()
	if err != nil {
		return "", err
	}
	if !t.PinsServer(pub) {
		return "", ErrServerKey
	}
	c.State.URL, c.State.ServerPub = t.URL, pub
	var tid [proto.TokenIDSize]byte
	copy(tid[:], t.ID)
	if err := c.handshake(proto.KindEnroll, tid, t.Secret); err != nil {
		return "", err
	}
	sas = c.sess.SAS()
	_, err = c.exchange(map[string]any{"t": "enroll", "name": name, "info": info})
	return sas, err
}

// Poll reports status and results and fetches new commands. It shakes hands
// first if there is no session, and once more if the server has forgotten it.
func (c *Client) Poll(status any, results []Result) (*Reply, error) {
	if c.State.ServerPub == nil {
		return nil, errors.New("device is not enrolled")
	}
	msg := map[string]any{"t": "poll", "status": status, "results": results}
	for attempt := 0; ; attempt++ {
		if c.sess == nil {
			if err := c.handshake(proto.KindSession, [proto.TokenIDSize]byte{}, nil); err != nil {
				return nil, err
			}
		}
		r, err := c.exchange(msg)
		if errors.Is(err, ErrRehandshake) && attempt == 0 {
			c.sess = nil
			continue
		}
		if err != nil {
			c.sess = nil
		}
		return r, err
	}
}
