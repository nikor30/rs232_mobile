package server

import (
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"
)

// limiter is a token bucket per key (a client address): burst requests at
// once, then one more every refill.
type limiter struct {
	mu      sync.Mutex
	burst   float64
	refill  time.Duration
	buckets map[string]*bucket
	sweep   time.Time
	denied  int64 // requests turned away since the start
}

type bucket struct {
	tokens float64
	last   time.Time
}

const maxBuckets = 20000

func newLimiter(burst int, refill time.Duration) *limiter {
	return &limiter{burst: float64(burst), refill: refill, buckets: map[string]*bucket{}}
}

// take spends one token of key. If there is none it says how long to wait.
func (l *limiter) take(key string, now time.Time) (ok bool, wait time.Duration) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if now.After(l.sweep) { // forget everybody whose bucket is full again
		for k, b := range l.buckets {
			if now.Sub(b.last) > time.Duration(l.burst)*l.refill {
				delete(l.buckets, k)
			}
		}
		l.sweep = now.Add(time.Minute)
	}
	b := l.buckets[key]
	if b == nil {
		if len(l.buckets) >= maxBuckets { // more addresses than this server will track: all of them wait
			return false, l.refill
		}
		b = &bucket{tokens: l.burst, last: now}
		l.buckets[key] = b
	}
	b.tokens += float64(now.Sub(b.last)) / float64(l.refill)
	if b.tokens > l.burst {
		b.tokens = l.burst
	}
	b.last = now
	if b.tokens < 1 {
		return false, time.Duration((1 - b.tokens) * float64(l.refill))
	}
	b.tokens--
	return true, 0
}

// clientIP is the address a request came from. Behind a reverse proxy that is
// the proxy's own address, unless the server was told to trust the proxy's
// X-Forwarded-For: then it is the last entry, the one the proxy itself saw.
func (sv *Server) clientIP(r *http.Request) string {
	if sv.cfg.TrustProxy {
		if xff := r.Header.Values("X-Forwarded-For"); len(xff) > 0 {
			parts := strings.Split(xff[len(xff)-1], ",")
			if ip := net.ParseIP(strings.TrimSpace(parts[len(parts)-1])); ip != nil {
				return ip.String()
			}
		}
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

// limited answers 429 and returns true if the client has used up its share of l.
func (sv *Server) limited(l *limiter, w http.ResponseWriter, r *http.Request) bool {
	ok, wait := l.take(sv.clientIP(r), sv.now())
	if ok {
		return false
	}
	l.mu.Lock()
	l.denied++
	l.mu.Unlock()
	w.Header().Set("Retry-After", strconv.Itoa(int(wait/time.Second)+1))
	http.Error(w, "too many requests", http.StatusTooManyRequests)
	return true
}
