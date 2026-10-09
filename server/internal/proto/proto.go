// Package proto is the device <-> server protocol: a mutually authenticated
// hybrid key exchange (X25519 + ML-KEM-768) and the record layer on top of it.
// It knows nothing about HTTP; server and device simulator both build on it, and
// the firmware has to implement exactly what is written here (KONZEPT.md §4).
//
// Authentication is done with KEMs only, no signatures: whoever can decapsulate
// what was encapsulated to a static key owns that key. That keeps the device
// side to one post-quantum primitive.
package proto

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/hkdf"
	"crypto/hmac"
	"crypto/mlkem"
	"crypto/rand"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
)

const (
	Version = 1

	KindSession = 1 // device already known to the server
	KindEnroll  = 2 // first contact, carries the device's static key and an enrollment token

	xSize   = 32
	ekSize  = mlkem.EncapsulationKeySize768 // 1184
	ctSize  = mlkem.CiphertextSize768       // 1088
	PubSize = xSize + ekSize                // static public key: X25519 || ML-KEM-768

	IDSize      = 16
	TokenIDSize = 8
	PSKSize     = 32

	reqBase    = 2 + IDSize + xSize + ekSize + ctSize // 2322
	ReqSession = reqBase
	ReqEnroll  = reqBase + PubSize + TokenIDSize // 3546
	respSigned = 1 + IDSize + xSize + ctSize + ctSize
	RespSize   = respSigned + sha256.Size // 2257

	seqSize = 8
	// Largest plaintext of one record. Keeps a device with little RAM safe from
	// a server (or anyone on the path) sending it megabytes.
	MaxPlaintext = 32 * 1024
)

var (
	ErrFormat  = errors.New("proto: malformed message")
	ErrConfirm = errors.New("proto: server did not prove its identity")
	ErrAuth    = errors.New("proto: record rejected")
)

// StaticKey is the long-term identity of a server or a device.
type StaticKey struct {
	x *ecdh.PrivateKey
	k *mlkem.DecapsulationKey768
}

func GenerateStatic() (*StaticKey, error) {
	x, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		return nil, err
	}
	k, err := mlkem.GenerateKey768()
	if err != nil {
		return nil, err
	}
	return &StaticKey{x, k}, nil
}

// Marshal returns the private key: X25519 scalar (32) || ML-KEM seed (64).
func (s *StaticKey) Marshal() []byte {
	return append(s.x.Bytes(), s.k.Bytes()...)
}

func ParseStatic(b []byte) (*StaticKey, error) {
	if len(b) != xSize+mlkem.SeedSize {
		return nil, ErrFormat
	}
	x, err := ecdh.X25519().NewPrivateKey(b[:xSize])
	if err != nil {
		return nil, err
	}
	k, err := mlkem.NewDecapsulationKey768(b[xSize:])
	if err != nil {
		return nil, err
	}
	return &StaticKey{x, k}, nil
}

// Public returns the public key, PubSize bytes.
func (s *StaticKey) Public() []byte {
	return append(s.x.PublicKey().Bytes(), s.k.EncapsulationKey().Bytes()...)
}

type publicKey struct {
	x *ecdh.PublicKey
	k *mlkem.EncapsulationKey768
}

func parsePublic(b []byte) (*publicKey, error) {
	if len(b) != PubSize {
		return nil, ErrFormat
	}
	x, err := ecdh.X25519().NewPublicKey(b[:xSize])
	if err != nil {
		return nil, ErrFormat
	}
	k, err := mlkem.NewEncapsulationKey768(b[xSize:])
	if err != nil {
		return nil, ErrFormat
	}
	return &publicKey{x, k}, nil
}

// CheckPublic reports whether b is a well-formed static public key.
func CheckPublic(b []byte) error {
	_, err := parsePublic(b)
	return err
}

// Fingerprint identifies a static public key.
func Fingerprint(pub []byte) [32]byte {
	h := sha256.New()
	h.Write([]byte("rs232-c2 key v1"))
	h.Write(pub)
	var out [32]byte
	h.Sum(out[:0])
	return out
}

// DeviceID is the first half of the fingerprint.
func DeviceID(pub []byte) (id [IDSize]byte) {
	fp := Fingerprint(pub)
	copy(id[:], fp[:])
	return id
}

func IDString(id [IDSize]byte) string { return hex.EncodeToString(id[:]) }

// ---------------------------------------------------------------- handshake

// Hello is what the server reads from a handshake request before it can decide
// whether to answer: who is asking, and with which token.
type Hello struct {
	Kind      byte
	DeviceID  [IDSize]byte
	DevicePub []byte            // enroll only: the device's static public key
	TokenID   [TokenIDSize]byte // enroll only
}

// ParseHello checks the layout of a request. It does no cryptography.
func ParseHello(req []byte) (*Hello, error) {
	if len(req) < reqBase || req[0] != Version {
		return nil, ErrFormat
	}
	h := &Hello{Kind: req[1]}
	copy(h.DeviceID[:], req[2:])
	switch h.Kind {
	case KindSession:
		if len(req) != ReqSession {
			return nil, ErrFormat
		}
	case KindEnroll:
		if len(req) != ReqEnroll {
			return nil, ErrFormat
		}
		h.DevicePub = req[reqBase : reqBase+PubSize]
		copy(h.TokenID[:], req[reqBase+PubSize:])
		if DeviceID(h.DevicePub) != h.DeviceID {
			return nil, ErrFormat
		}
	default:
		return nil, ErrFormat
	}
	return h, nil
}

// ClientHandshake is the device's half of a handshake in progress.
type ClientHandshake struct {
	dev       *StaticKey
	serverPub []byte
	psk       []byte
	ex        *ecdh.PrivateKey
	ek        *mlkem.DecapsulationKey768
	ssS, dhES []byte
	req       []byte
}

// NewClientHandshake builds the request. serverPub must already be trusted
// (pinned): the device proves nothing to anyone else. For KindEnroll, tokenID
// and psk come from the enrollment token; for KindSession both are ignored.
func NewClientHandshake(kind byte, dev *StaticKey, serverPub []byte, tokenID [TokenIDSize]byte, psk []byte) (*ClientHandshake, []byte, error) {
	srv, err := parsePublic(serverPub)
	if err != nil {
		return nil, nil, err
	}
	if kind != KindSession && kind != KindEnroll {
		return nil, nil, ErrFormat
	}
	if kind == KindEnroll && len(psk) != PSKSize {
		return nil, nil, ErrFormat
	}
	if kind == KindSession {
		psk = nil
	}
	c := &ClientHandshake{dev: dev, serverPub: serverPub, psk: psk}
	if c.ex, err = ecdh.X25519().GenerateKey(rand.Reader); err != nil {
		return nil, nil, err
	}
	if c.ek, err = mlkem.GenerateKey768(); err != nil {
		return nil, nil, err
	}
	var ctS []byte
	c.ssS, ctS = srv.k.Encapsulate()
	if c.dhES, err = c.ex.ECDH(srv.x); err != nil {
		return nil, nil, err
	}
	devPub := dev.Public()
	id := DeviceID(devPub)
	req := make([]byte, 0, ReqEnroll)
	req = append(req, Version, kind)
	req = append(req, id[:]...)
	req = append(req, c.ex.PublicKey().Bytes()...)
	req = append(req, c.ek.EncapsulationKey().Bytes()...)
	req = append(req, ctS...)
	if kind == KindEnroll {
		req = append(req, devPub...)
		req = append(req, tokenID[:]...)
	}
	c.req = req
	return c, req, nil
}

// Finish checks the server's answer. A session is returned only if the server
// proved that it owns the pinned static key (and, when enrolling, that it knows
// the token secret).
func (c *ClientHandshake) Finish(resp []byte) (*Session, error) {
	if len(resp) != RespSize || resp[0] != Version {
		return nil, ErrFormat
	}
	p := resp[1:]
	var sid [IDSize]byte
	copy(sid[:], p)
	p = p[IDSize:]
	esx, err := ecdh.X25519().NewPublicKey(p[:xSize])
	if err != nil {
		return nil, ErrFormat
	}
	p = p[xSize:]
	ssE, err := c.ek.Decapsulate(p[:ctSize])
	if err != nil {
		return nil, ErrFormat
	}
	ssD, err := c.dev.k.Decapsulate(p[ctSize : 2*ctSize])
	if err != nil {
		return nil, ErrFormat
	}
	dhEE, err := c.ex.ECDH(esx)
	if err != nil {
		return nil, ErrFormat
	}
	dhSE, err := c.dev.x.ECDH(esx)
	if err != nil {
		return nil, ErrFormat
	}
	ks := schedule(c.psk, c.serverPub, c.dev.Public(), c.req, resp[:respSigned], c.ssS, ssE, ssD, c.dhES, dhEE, dhSE)
	if !hmac.Equal(ks.confirm, resp[respSigned:]) {
		return nil, ErrConfirm
	}
	return newSession(sid, ks, true)
}

// ServerHandshake answers a request. devicePub is the device's static key: from
// the server's records for KindSession, from the request for KindEnroll. psk is
// the token secret when enrolling, nil otherwise.
//
// The returned session is not yet proof of anything: the device is
// authenticated by the first record that opens with it.
func ServerHandshake(srv *StaticKey, req, devicePub, psk []byte) (resp []byte, s *Session, err error) {
	h, err := ParseHello(req)
	if err != nil {
		return nil, nil, err
	}
	if h.Kind == KindEnroll && len(psk) != PSKSize {
		return nil, nil, ErrFormat
	}
	if h.Kind == KindSession {
		psk = nil
	}
	dev, err := parsePublic(devicePub)
	if err != nil || DeviceID(devicePub) != h.DeviceID {
		return nil, nil, ErrFormat
	}
	p := req[2+IDSize:]
	ex, err := ecdh.X25519().NewPublicKey(p[:xSize])
	if err != nil {
		return nil, nil, ErrFormat
	}
	p = p[xSize:]
	eek, err := mlkem.NewEncapsulationKey768(p[:ekSize])
	if err != nil {
		return nil, nil, ErrFormat
	}
	p = p[ekSize:]
	ssS, err := srv.k.Decapsulate(p[:ctSize])
	if err != nil {
		return nil, nil, ErrFormat
	}
	dhES, err := srv.x.ECDH(ex)
	if err != nil {
		return nil, nil, ErrFormat
	}
	es, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	ssE, ctE := eek.Encapsulate()
	ssD, ctD := dev.k.Encapsulate()
	dhEE, err := es.ECDH(ex)
	if err != nil {
		return nil, nil, ErrFormat
	}
	dhSE, err := es.ECDH(dev.x)
	if err != nil {
		return nil, nil, ErrFormat
	}
	var sid [IDSize]byte
	if _, err := rand.Read(sid[:]); err != nil {
		return nil, nil, err
	}
	resp = make([]byte, 0, RespSize)
	resp = append(resp, Version)
	resp = append(resp, sid[:]...)
	resp = append(resp, es.PublicKey().Bytes()...)
	resp = append(resp, ctE...)
	resp = append(resp, ctD...)
	ks := schedule(psk, srv.Public(), devicePub, req, resp, ssS, ssE, ssD, dhES, dhEE, dhSE)
	resp = append(resp, ks.confirm...)
	s, err = newSession(sid, ks, false)
	return resp, s, err
}

type keys struct {
	c2s, s2c, confirm []byte
	sas               string
}

// schedule derives everything from the six shared secrets. Three of them are
// post-quantum (ML-KEM), three classical (X25519); in each group one
// authenticates the server, one the device and one gives forward secrecy. The
// transcript hash binds both static keys and every byte exchanged.
func schedule(psk, serverPub, devicePub, req, respSigned []byte, secrets ...[]byte) keys {
	var ikm []byte
	for _, s := range secrets {
		ikm = append(ikm, s...)
	}
	salt := psk
	if len(salt) == 0 {
		salt = make([]byte, PSKSize)
	}
	th := sha256.New()
	th.Write([]byte("rs232-c2 hs v1"))
	th.Write(serverPub)
	th.Write(devicePub)
	th.Write(req)
	th.Write(respSigned)
	info := string(th.Sum(nil))
	prk, _ := hkdf.Extract(sha256.New, ikm, salt)
	expand := func(label string, n int) []byte {
		out, _ := hkdf.Expand(sha256.New, prk, label+info, n) // fails only for absurd n
		return out
	}
	m := hmac.New(sha256.New, expand("confirm", 32))
	m.Write([]byte("server"))
	m.Write([]byte(info))
	return keys{
		c2s:     expand("c2s", 32),
		s2c:     expand("s2c", 32),
		confirm: m.Sum(nil),
		sas:     fmt.Sprintf("%06d", binary.BigEndian.Uint32(expand("sas", 4))%1000000),
	}
}

// ---------------------------------------------------------------- records

// Session is one end of an established channel. Not safe for concurrent use.
type Session struct {
	ID       [IDSize]byte
	isClient bool
	send     cipher.AEAD
	recv     cipher.AEAD
	sendSeq  uint64 // next sequence number to send
	recvSeq  uint64 // lowest sequence number still accepted
	sas      string
}

func newSession(id [IDSize]byte, ks keys, isClient bool) (*Session, error) {
	mk := func(key []byte) (cipher.AEAD, error) {
		b, err := aes.NewCipher(key)
		if err != nil {
			return nil, err
		}
		return cipher.NewGCM(b)
	}
	s := &Session{ID: id, isClient: isClient, sas: ks.sas}
	sk, rk := ks.s2c, ks.c2s
	if isClient {
		sk, rk = ks.c2s, ks.s2c
	}
	var err error
	if s.send, err = mk(sk); err != nil {
		return nil, err
	}
	if s.recv, err = mk(rk); err != nil {
		return nil, err
	}
	return s, nil
}

// SAS is the six-digit code both ends derive from an enrollment handshake. A
// person compares it between the device's display and the server.
func (s *Session) SAS() string { return s.sas }

func (s *Session) nonceAAD(seq uint64, fromClient bool) (nonce, aad []byte) {
	nonce = make([]byte, 12)
	binary.BigEndian.PutUint64(nonce[4:], seq)
	aad = make([]byte, 0, 1+IDSize+seqSize)
	dir := byte('s')
	if fromClient {
		dir = 'c'
	}
	aad = append(aad, dir)
	aad = append(aad, s.ID[:]...)
	aad = binary.BigEndian.AppendUint64(aad, seq)
	return nonce, aad
}

// Seal encrypts one message: seq (8) || AES-256-GCM ciphertext.
func (s *Session) Seal(plaintext []byte) ([]byte, error) {
	if len(plaintext) > MaxPlaintext || s.sendSeq == ^uint64(0) {
		return nil, ErrFormat
	}
	seq := s.sendSeq
	s.sendSeq++
	nonce, aad := s.nonceAAD(seq, s.isClient)
	out := binary.BigEndian.AppendUint64(make([]byte, 0, seqSize+len(plaintext)+16), seq)
	return s.send.Seal(out, nonce, plaintext, aad), nil
}

// Open decrypts what the other end sealed. Sequence numbers must increase, so a
// recorded message cannot be played back.
func (s *Session) Open(record []byte) ([]byte, error) {
	if len(record) < seqSize+16 || len(record) > seqSize+MaxPlaintext+16 {
		return nil, ErrAuth
	}
	seq := binary.BigEndian.Uint64(record)
	if seq < s.recvSeq {
		return nil, ErrAuth
	}
	nonce, aad := s.nonceAAD(seq, !s.isClient)
	pt, err := s.recv.Open(nil, nonce, record[seqSize:], aad)
	if err != nil {
		return nil, ErrAuth
	}
	s.recvSeq = seq + 1
	return pt, nil
}
