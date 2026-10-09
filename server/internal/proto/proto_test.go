package proto

import (
	"bytes"
	"crypto/rand"
	"testing"
)

func mustStatic(t *testing.T) *StaticKey {
	t.Helper()
	k, err := GenerateStatic()
	if err != nil {
		t.Fatal(err)
	}
	return k
}

func randBytes(n int) []byte {
	b := make([]byte, n)
	rand.Read(b)
	return b
}

// handshake runs both halves; the server looks the device key up the way the
// HTTP layer does.
func handshake(t *testing.T, kind byte, srv, dev *StaticKey, clientSrvPub, clientPSK, serverPSK []byte) (c, s *Session, err error) {
	t.Helper()
	var tok [TokenIDSize]byte
	hs, req, err := NewClientHandshake(kind, dev, clientSrvPub, tok, clientPSK)
	if err != nil {
		t.Fatal(err)
	}
	h, err := ParseHello(req)
	if err != nil {
		t.Fatal(err)
	}
	devPub := dev.Public()
	if kind == KindEnroll {
		devPub = h.DevicePub
	}
	resp, s, err := ServerHandshake(srv, req, devPub, serverPSK)
	if err != nil {
		t.Fatal(err)
	}
	c, err = hs.Finish(resp)
	return c, s, err
}

func TestSessionRoundTrip(t *testing.T) {
	srv, dev := mustStatic(t), mustStatic(t)
	c, s, err := handshake(t, KindSession, srv, dev, srv.Public(), nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 3; i++ {
		rec, _ := c.Seal([]byte("hello"))
		pt, err := s.Open(rec)
		if err != nil || string(pt) != "hello" {
			t.Fatalf("c2s %d: %q %v", i, pt, err)
		}
		rec, _ = s.Seal([]byte("world"))
		pt, err = c.Open(rec)
		if err != nil || string(pt) != "world" {
			t.Fatalf("s2c %d: %q %v", i, pt, err)
		}
	}
}

func TestEnrollSASMatches(t *testing.T) {
	srv, dev := mustStatic(t), mustStatic(t)
	psk := randBytes(PSKSize)
	c, s, err := handshake(t, KindEnroll, srv, dev, srv.Public(), psk, psk)
	if err != nil {
		t.Fatal(err)
	}
	if c.SAS() != s.SAS() || len(c.SAS()) != 6 {
		t.Fatalf("SAS %q vs %q", c.SAS(), s.SAS())
	}
	rec, _ := c.Seal([]byte("x"))
	if _, err := s.Open(rec); err != nil {
		t.Fatal(err)
	}
}

func TestWrongServerIsRejected(t *testing.T) {
	// The device pinned srv, but someone else answers (man in the middle).
	srv, mitm, dev := mustStatic(t), mustStatic(t), mustStatic(t)
	if _, _, err := handshake(t, KindSession, mitm, dev, srv.Public(), nil, nil); err != ErrConfirm {
		t.Fatalf("got %v, want ErrConfirm", err)
	}
}

func TestWrongTokenSecretIsRejected(t *testing.T) {
	srv, dev := mustStatic(t), mustStatic(t)
	if _, _, err := handshake(t, KindEnroll, srv, dev, srv.Public(), randBytes(PSKSize), randBytes(PSKSize)); err != ErrConfirm {
		t.Fatalf("got %v, want ErrConfirm", err)
	}
}

func TestImpostorDeviceCannotTalk(t *testing.T) {
	// Someone knows a device's ID and public key but not its private key.
	srv, dev, impostor := mustStatic(t), mustStatic(t), mustStatic(t)
	var tok [TokenIDSize]byte
	_, req, err := NewClientHandshake(KindSession, impostor, srv.Public(), tok, nil)
	if err != nil {
		t.Fatal(err)
	}
	id := DeviceID(dev.Public())
	copy(req[2:], id[:]) // claim to be dev
	resp, s, err := ServerHandshake(srv, req, dev.Public(), nil)
	if err != nil {
		t.Fatal(err)
	}
	// The impostor cannot derive the keys. Whatever it sends does not open.
	_ = resp
	for _, rec := range [][]byte{randBytes(40), append(make([]byte, 8), randBytes(32)...)} {
		if _, err := s.Open(rec); err != ErrAuth {
			t.Fatalf("got %v, want ErrAuth", err)
		}
	}
}

func TestReplayAndTamper(t *testing.T) {
	srv, dev := mustStatic(t), mustStatic(t)
	c, s, err := handshake(t, KindSession, srv, dev, srv.Public(), nil, nil)
	if err != nil {
		t.Fatal(err)
	}
	r1, _ := c.Seal([]byte("one"))
	r2, _ := c.Seal([]byte("two"))
	if _, err := s.Open(r1); err != nil {
		t.Fatal(err)
	}
	if _, err := s.Open(r1); err != ErrAuth {
		t.Fatalf("replay: got %v", err)
	}
	bad := bytes.Clone(r2)
	bad[len(bad)-1] ^= 1
	if _, err := s.Open(bad); err != ErrAuth {
		t.Fatalf("tamper: got %v", err)
	}
	if _, err := s.Open(r2); err != nil {
		t.Fatalf("a rejected record must not block the real one: %v", err)
	}
	// a record of one direction is not valid in the other
	r3, _ := c.Seal([]byte("three"))
	if _, err := c.Open(r3); err != ErrAuth {
		t.Fatalf("reflection: got %v", err)
	}
}

func TestTamperedHandshakeFails(t *testing.T) {
	srv, dev := mustStatic(t), mustStatic(t)
	var tok [TokenIDSize]byte
	hs, req, _ := NewClientHandshake(KindSession, dev, srv.Public(), tok, nil)
	resp, _, err := ServerHandshake(srv, req, dev.Public(), nil)
	if err != nil {
		t.Fatal(err)
	}
	for _, i := range []int{20, 100, 1200, RespSize - 1} {
		bad := bytes.Clone(resp)
		bad[i] ^= 1
		if _, err := hs.Finish(bad); err == nil {
			t.Fatalf("response with byte %d flipped was accepted", i)
		}
	}
	if _, err := hs.Finish(resp); err != nil {
		t.Fatal(err)
	}
}

func TestMalformedInput(t *testing.T) {
	srv, dev := mustStatic(t), mustStatic(t)
	var tok [TokenIDSize]byte
	_, req, _ := NewClientHandshake(KindEnroll, dev, srv.Public(), tok, randBytes(PSKSize))
	for _, bad := range [][]byte{nil, req[:10], req[:ReqSession], append(bytes.Clone(req), 0)} {
		if _, err := ParseHello(bad); err == nil {
			t.Fatalf("accepted %d bytes", len(bad))
		}
	}
	other := mustStatic(t)
	swapped := bytes.Clone(req)
	copy(swapped[reqBase:], other.Public()) // key does not match the claimed ID
	if _, err := ParseHello(swapped); err == nil {
		t.Fatal("accepted a device key that does not match the device ID")
	}
	if _, _, err := ServerHandshake(srv, req, dev.Public(), nil); err == nil {
		t.Fatal("enrollment without token secret was answered")
	}
	if CheckPublic(randBytes(10)) == nil {
		t.Fatal("CheckPublic accepted garbage")
	}
}

func TestStaticKeyMarshal(t *testing.T) {
	k := mustStatic(t)
	k2, err := ParseStatic(k.Marshal())
	if err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(k.Public(), k2.Public()) || len(k.Public()) != PubSize {
		t.Fatal("key did not survive Marshal/ParseStatic")
	}
	if len(k.Marshal()) != 96 {
		t.Fatalf("private key is %d bytes", len(k.Marshal()))
	}
}
