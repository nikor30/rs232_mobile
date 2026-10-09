package proto

import (
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"strings"
)

// EnrollToken is what an administrator hands to a new device, as one line of
// text (later a QR code): where the server is, which key it must prove, and a
// one-time secret that proves to the server that this device was invited.
type EnrollToken struct {
	URL         string `json:"u"`
	Fingerprint []byte `json:"fp"` // Fingerprint() of the server's static public key
	ID          []byte `json:"id"` // TokenIDSize bytes, sent in the clear
	Secret      []byte `json:"s"`  // PSKSize bytes, never sent
}

const tokenPrefix = "c2e1:"

func (t EnrollToken) String() string {
	b, _ := json.Marshal(t)
	return tokenPrefix + base64.RawURLEncoding.EncodeToString(b)
}

func ParseEnrollToken(s string) (*EnrollToken, error) {
	s = strings.TrimSpace(s)
	if !strings.HasPrefix(s, tokenPrefix) {
		return nil, errors.New("not an enrollment token")
	}
	b, err := base64.RawURLEncoding.DecodeString(s[len(tokenPrefix):])
	if err != nil {
		return nil, errors.New("enrollment token is damaged")
	}
	var t EnrollToken
	if err := json.Unmarshal(b, &t); err != nil {
		return nil, errors.New("enrollment token is damaged")
	}
	if t.URL == "" || len(t.Fingerprint) != 32 || len(t.ID) != TokenIDSize || len(t.Secret) != PSKSize {
		return nil, errors.New("enrollment token is incomplete")
	}
	return &t, nil
}

// PinsServer reports whether serverPub is the key the token was issued for.
func (t *EnrollToken) PinsServer(serverPub []byte) bool {
	fp := Fingerprint(serverPub)
	return subtle.ConstantTimeCompare(fp[:], t.Fingerprint) == 1
}
