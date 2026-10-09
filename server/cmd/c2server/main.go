// c2server is the command-and-control server for RS232 Web Console devices.
// Configuration comes from the environment (see README.md).
package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/hex"
	"encoding/pem"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"rs232c2/internal/proto"
	"rs232c2/internal/server"
	"rs232c2/internal/store"
)

func env(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}

// selfSigned returns a certificate for local use, kept in dir so it stays the
// same across restarts.
func selfSigned(dir string, hosts []string) (tls.Certificate, error) {
	crtFile, keyFile := filepath.Join(dir, "tls.crt"), filepath.Join(dir, "tls.key")
	if c, err := tls.LoadX509KeyPair(crtFile, keyFile); err == nil {
		return c, nil
	}
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return tls.Certificate{}, err
	}
	serial, _ := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 120))
	tpl := &x509.Certificate{
		SerialNumber: serial,
		Subject:      pkix.Name{CommonName: "rs232-c2 (self-signed)"},
		NotBefore:    time.Now().Add(-time.Hour),
		NotAfter:     time.Now().AddDate(2, 0, 0),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
	}
	for _, h := range hosts {
		if ip := net.ParseIP(h); ip != nil {
			tpl.IPAddresses = append(tpl.IPAddresses, ip)
		} else {
			tpl.DNSNames = append(tpl.DNSNames, h)
		}
	}
	der, err := x509.CreateCertificate(rand.Reader, tpl, tpl, &key.PublicKey, key)
	if err != nil {
		return tls.Certificate{}, err
	}
	kb, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return tls.Certificate{}, err
	}
	if err := os.WriteFile(crtFile, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}), 0o644); err != nil {
		return tls.Certificate{}, err
	}
	if err := os.WriteFile(keyFile, pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: kb}), 0o600); err != nil {
		return tls.Certificate{}, err
	}
	return tls.LoadX509KeyPair(crtFile, keyFile)
}

func main() {
	log.SetFlags(log.LstdFlags | log.LUTC)
	dataDir := env("C2_DATA_DIR", "/data")
	listen := env("C2_LISTEN", ":8443")
	tlsMode := env("C2_TLS", "selfsigned") // selfsigned | files | off
	publicURL := env("C2_PUBLIC_URL", "https://localhost:8443")
	admin := os.Getenv("C2_ADMIN_TOKEN")
	if len(admin) < 16 {
		log.Fatal("C2_ADMIN_TOKEN must be set to at least 16 characters")
	}
	pollS, _ := strconv.Atoi(env("C2_POLL_S", "30"))

	if err := os.MkdirAll(dataDir, 0o700); err != nil {
		log.Fatal(err)
	}
	st, err := store.Open(dataDir)
	if err != nil {
		log.Fatalf("state: %v", err)
	}
	fp := proto.Fingerprint(st.ServerKey().Public())
	log.Printf("server key fingerprint %s", hex.EncodeToString(fp[:]))

	sv := server.New(server.Config{AdminToken: admin, PublicURL: publicURL, PollS: pollS}, st)
	hs := &http.Server{
		Addr:              listen,
		Handler:           sv.Handler(),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       30 * time.Second,
		WriteTimeout:      30 * time.Second,
		IdleTimeout:       2 * time.Minute,
		MaxHeaderBytes:    16 * 1024,
	}
	switch tlsMode {
	case "off": // behind a reverse proxy or load balancer that terminates TLS
		log.Printf("listening on %s (plain HTTP), public URL %s", listen, publicURL)
		log.Fatal(hs.ListenAndServe())
	case "files":
		log.Printf("listening on %s (TLS), public URL %s", listen, publicURL)
		log.Fatal(hs.ListenAndServeTLS(env("C2_TLS_CERT", "/data/tls.crt"), env("C2_TLS_KEY", "/data/tls.key")))
	case "selfsigned":
		hosts := strings.Split(env("C2_TLS_HOSTS", "localhost,127.0.0.1,c2server"), ",")
		cert, err := selfSigned(dataDir, hosts)
		if err != nil {
			log.Fatalf("certificate: %v", err)
		}
		hs.TLSConfig = &tls.Config{Certificates: []tls.Certificate{cert}, MinVersion: tls.VersionTLS12}
		log.Printf("listening on %s (TLS, self-signed), public URL %s", listen, publicURL)
		log.Fatal(hs.ListenAndServeTLS("", ""))
	default:
		log.Fatalf("C2_TLS=%q: use selfsigned, files or off", tlsMode)
	}
}
