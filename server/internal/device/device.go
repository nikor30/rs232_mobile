// Package device is the device's side of the protocol over HTTP. The simulator
// and the server's tests use it; it is also the reference for what the firmware
// has to do, step by step.
package device

import (
	"bytes"
	"encoding/hex"
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
	// A new key the server announced on a session the pinned key
	// authenticated. It takes the place of ServerPub with the first handshake
	// that only it can finish.
	NextPub []byte `json:"next_pub,omitempty"`
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
	Rekey []byte    `json:"rekey,omitempty"`
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
	var r Reply
	return &r, c.exchangeInto(msg, &r)
}

func (c *Client) exchangeInto(msg, reply any) error {
	pt, err := c.exchangeRaw(msg)
	if err != nil {
		return err
	}
	return json.Unmarshal(pt, reply)
}

func (c *Client) exchangeRaw(msg any) ([]byte, error) {
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
	return c.sess.Open(resp)
}

// ConMsg is one step of a remote console, device to server: output of the
// serial port from offset Off on, and how much input the device has taken.
type ConMsg struct {
	T     string `json:"t"`
	ID    uint64 `json:"id"`
	Off   int64  `json:"off"`
	D     []byte `json:"d,omitempty"`
	IAck  int64  `json:"iack"`
	LeftS int    `json:"left_s"`
	End   string `json:"end,omitempty"`
}

// ConReply brings input for the serial port (from offset IOff), the number of
// BREAKs asked for so far, and End if the session is over.
type ConReply struct {
	IOff int64  `json:"ioff"`
	D    []byte `json:"d"`
	Brk  int    `json:"brk"`
	End  string `json:"end"`
}

// Console sends one console message on the current session.
func (c *Client) Console(m ConMsg) (*ConReply, error) {
	if c.sess == nil {
		if err := c.sessionHandshake(); err != nil {
			return nil, err
		}
	}
	m.T = "con"
	var r ConReply
	err := c.exchangeInto(m, &r)
	if err != nil {
		c.sess = nil
	}
	return &r, err
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
	c.State.URL, c.State.ServerPub, c.State.NextPub = t.URL, pub, nil
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
	for attempt := 0; ; attempt++ {
		if c.sess == nil {
			if err := c.sessionHandshake(); err != nil {
				return nil, err
			}
		}
		// "sk" tells the server which of its keys this device holds: the
		// announced one, once it is stored.
		msg := map[string]any{"t": "poll", "status": status, "results": results, "sk": c.KeyTag()}
		r, err := c.exchange(msg)
		if errors.Is(err, ErrRehandshake) && attempt == 0 {
			c.sess = nil
			continue
		}
		if err != nil {
			c.sess = nil
			return r, err
		}
		if len(r.Rekey) > 0 && !bytes.Equal(r.Rekey, c.State.ServerPub) && proto.CheckPublic(r.Rekey) == nil {
			c.State.NextPub = r.Rekey
		}
		return r, nil
	}
}

// KeyTag names the newest server key the device holds (store.KeyTag at the server).
func (c *Client) KeyTag() string {
	pub := c.State.ServerPub
	if c.State.NextPub != nil {
		pub = c.State.NextPub
	}
	fp := proto.Fingerprint(pub)
	return hex.EncodeToString(fp[:8])
}

// sessionHandshake shakes hands with the pinned server key. If the server
// cannot prove that one and it has announced another, that one is tried - and
// pinned if it works: the key change is done.
func (c *Client) sessionHandshake() error {
	err := c.handshake(proto.KindSession, [proto.TokenIDSize]byte{}, nil)
	if !errors.Is(err, proto.ErrConfirm) || c.State.NextPub == nil {
		return err
	}
	pinned := c.State.ServerPub
	c.State.ServerPub = c.State.NextPub
	if err2 := c.handshake(proto.KindSession, [proto.TokenIDSize]byte{}, nil); err2 != nil {
		c.State.ServerPub = pinned
		return err
	}
	c.State.NextPub = nil
	return nil
}
