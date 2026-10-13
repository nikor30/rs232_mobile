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
	"errors"
	"fmt"
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

// mb reads a size in megabytes from the environment and returns bytes.
func mb(key string, def int64) int64 {
	if n, err := strconv.ParseInt(os.Getenv(key), 10, 64); err == nil && n > 0 {
		return n << 20
	}
	return def << 20
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

// health asks the server in this container how it is (docker's health check:
// the image has no shell and no curl).
func health(listen, tlsMode string) int {
	_, port, err := net.SplitHostPort(listen)
	if err != nil {
		port = strings.TrimPrefix(listen, ":")
	}
	scheme := "https"
	if tlsMode == "off" {
		scheme = "http"
	}
	// its own certificate, asked for by address
	c := &http.Client{Timeout: 5 * time.Second, Transport: &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}}}
	resp, err := c.Get(scheme + "://127.0.0.1:" + port + "/healthz")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		fmt.Fprintln(os.Stderr, resp.Status)
		return 1
	}
	return 0
}

// watch checks the state twice a minute and writes into the log when that
// changes - also while nobody asks /healthz.
func watch(sv *server.Server) {
	var failing bool
	for range time.Tick(30 * time.Second) {
		err := sv.Check()
		if errors.Is(err, store.ErrTaken) {
			log.Fatalf("state: %v - stopping", err)
		}
		switch {
		case err != nil && !failing:
			log.Printf("state: CANNOT BE SAVED: %v", err)
		case err == nil && failing:
			log.Print("state: can be saved again")
		}
		failing = err != nil
	}
}

func main() {
	log.SetFlags(log.LstdFlags | log.LUTC)
	if len(os.Args) > 1 && os.Args[1] == "-health" {
		os.Exit(health(env("C2_LISTEN", ":8443"), env("C2_TLS", "selfsigned")))
	}
	dataDir := env("C2_DATA_DIR", "/data")
	listen := env("C2_LISTEN", ":8443")
	tlsMode := env("C2_TLS", "selfsigned") // selfsigned | files | off
	publicURL := env("C2_PUBLIC_URL", "https://localhost:8443")
	admin := os.Getenv("C2_ADMIN_TOKEN")
	if admin != "" && len(admin) < 16 {
		log.Fatal("C2_ADMIN_TOKEN needs at least 16 characters")
	}
	pollS, _ := strconv.Atoi(env("C2_POLL_S", "30"))

	err := os.MkdirAll(dataDir, 0o700)
	if err != nil {
		log.Fatal(err)
	}
	var st *store.Store
	if db := os.Getenv("C2_DATABASE_URL"); db != "" {
		if st, err = store.OpenPostgres(db, dataDir); err != nil {
			log.Fatalf("state: %v", err)
		}
		log.Print("state is kept in PostgreSQL")
		if f := st.Imported(); f != "" {
			log.Printf("took over the state of the file; it is now %s and still holds the server key - store it safely or delete it", f)
		} else if _, err := os.Stat(filepath.Join(dataDir, "state.json")); err == nil {
			log.Printf("%s is not used: the database has a state of its own", filepath.Join(dataDir, "state.json"))
		}
	} else if st, err = store.Open(dataDir); err != nil {
		log.Fatalf("state: %v", err)
	}
	fp := proto.Fingerprint(st.ServerKey().Public())
	log.Printf("server key fingerprint %s", hex.EncodeToString(fp[:]))

	// Without the admin token somebody has to be able to sign in already.
	if admin == "" && len(st.Users()) == 0 {
		log.Fatal("no account exists yet: start once with C2_ADMIN_TOKEN set and create one (README.md)")
	}
	cfg := server.Config{AdminToken: admin, PublicURL: publicURL, PollS: pollS, FirmwareDir: filepath.Join(dataDir, "firmware"),
		ImageDir: filepath.Join(dataDir, "images"), ImageMax: mb("C2_IMAGE_MAX_MB", 2048), ImageTotal: mb("C2_IMAGE_TOTAL_MB", 20480),
		SecureCookies: strings.HasPrefix(publicURL, "https://"), TrustProxy: os.Getenv("C2_TRUST_PROXY") == "1"}
	if tenant := os.Getenv("C2_ENTRA_TENANT"); tenant != "" {
		cfg.Entra = &server.EntraConfig{Tenant: strings.ToLower(tenant), ClientID: os.Getenv("C2_ENTRA_CLIENT_ID"), ClientSecret: os.Getenv("C2_ENTRA_CLIENT_SECRET")}
		if cfg.Entra.ClientID == "" || cfg.Entra.ClientSecret == "" {
			log.Fatal("C2_ENTRA_TENANT is set: C2_ENTRA_CLIENT_ID and C2_ENTRA_CLIENT_SECRET are needed too")
		}
		log.Printf("sign-in with Microsoft Entra ID is on (tenant %s); redirect URI %s/auth/entra/callback", cfg.Entra.Tenant, strings.TrimRight(publicURL, "/"))
	}
	if admin == "" {
		log.Print("no admin token: accounts only")
	}
	sv := server.New(cfg, st)
	go watch(sv)
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
