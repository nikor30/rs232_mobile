#!/usr/bin/env bash
# Off-site copy of a control center that keeps its state in a data directory
# (state.json, firmware/, images/), pulled over SSH by another machine - the
# server itself is not changed. Each copy is encrypted to a certificate
# (CMS, AES-256-GCM): the machine that pulls needs only the certificate, the
# private key is needed only to restore and belongs somewhere else as well.
#
#   offsite-backup.sh keygen              key pair, once (prints where it went)
#   offsite-backup.sh run                 one copy now (the timer calls this)
#   offsite-backup.sh list                what is there
#   offsite-backup.sh restore FILE DIR    decrypt into DIR, a data directory for c2server
#
# Settings: /etc/rs232-c2-offsite.env or the environment.
#   C2_SSH            user@host of the server (required for run)
#   C2_SSH_KEY        SSH key for it (default: the user's own)
#   C2_REMOTE_DIR     data directory on the server (default /var/lib/rs232-c2; read with sudo)
#   C2_OFFSITE_DIR    where the copies go (default /var/backups/rs232-c2)
#   C2_OFFSITE_KEEP   how many copies stay (default 14)
#   C2_OFFSITE_KEY    private key (default ~/.config/rs232-console/offsite-backup-key.pem)
#   C2_NTFY           optional ntfy topic URL: a message when a copy fails
#
# The images (images/*.bin, up to 2 GB each, not secret) are not in the copies:
# they are mirrored unencrypted to $C2_OFFSITE_DIR/images and never deleted there.
set -euo pipefail
[ -r /etc/rs232-c2-offsite.env ] && . /etc/rs232-c2-offsite.env
REMOTE_DIR=${C2_REMOTE_DIR:-/var/lib/rs232-c2}
DIR=${C2_OFFSITE_DIR:-/var/backups/rs232-c2}
KEEP=${C2_OFFSITE_KEEP:-14}
HOME=${HOME:-$(getent passwd "$(id -u)" | cut -d: -f6)}   # systemd does not set it
KEY=${C2_OFFSITE_KEY:-$HOME/.config/rs232-console/offsite-backup-key.pem}
CERT=$DIR/offsite-backup-cert.pem
umask 077

ssh_() { ssh ${C2_SSH_KEY:+-i "$C2_SSH_KEY" -o IdentitiesOnly=yes} -o BatchMode=yes -o ConnectTimeout=20 "$C2_SSH" "$@"; }
notify() { [ -n "${C2_NTFY:-}" ] && curl -s -m 20 -H "Title: RS232 control center backup" -H "Priority: high" -d "$1" "$C2_NTFY" >/dev/null || true; }
die() { echo "offsite-backup: $*" >&2; [ "${CMD:-}" = run ] && notify "Off-site copy FAILED on $(hostname): $*"; exit 1; }

CMD=${1:-}
[ "$CMD" = run ] && trap 'notify "Off-site copy FAILED on $(hostname) (line $LINENO)"' ERR
case "$CMD" in
keygen)
  [ -e "$KEY" ] && die "$KEY exists already"
  mkdir -p "$(dirname "$KEY")" "$DIR"
  openssl req -x509 -newkey rsa:4096 -nodes -keyout "$KEY" -out "$CERT" -days 36500 \
    -subj "/CN=rs232-c2 offsite backup" 2>/dev/null
  chmod 600 "$KEY"
  echo "private key: $KEY  (needed to restore - keep a copy away from this machine)"
  echo "certificate: $CERT (enough to make copies)"
  ;;
run)
  [ -n "${C2_SSH:-}" ] || die "C2_SSH is not set"
  [ -r "$CERT" ] || die "no certificate at $CERT - run keygen first"
  mkdir -p "$DIR/images"
  WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
  # state.json is replaced by a rename, never written in place: a copy is always whole
  ssh_ "sudo tar -C '$REMOTE_DIR' -cf - --exclude='./images/*.bin' ." > "$WORK/data.tar" || die "could not read $REMOTE_DIR on $C2_SSH"
  tar -xOf "$WORK/data.tar" ./state.json | python3 -c 'import json,sys; s=json.load(sys.stdin); assert s' \
    || die "state.json missing or unreadable in the copy"
  NAME=c2-$(date -u +%Y%m%dT%H%M%SZ).tar.zst.cms
  zstd -q -19 -c "$WORK/data.tar" > "$WORK/data.tar.zst"
  openssl cms -encrypt -binary -aes-256-gcm -outform DER -in "$WORK/data.tar.zst" -out "$WORK/$NAME" "$CERT" \
    || die "encryption failed"
  mv "$WORK/$NAME" "$DIR/$NAME"
  # images: only add, never delete - a server that lost them must not take the copies along
  rsync -a --rsync-path="sudo rsync" -e "ssh ${C2_SSH_KEY:+-i $C2_SSH_KEY -o IdentitiesOnly=yes} -o BatchMode=yes" \
    --include='*.bin' --exclude='*' "$C2_SSH:$REMOTE_DIR/images/" "$DIR/images/" || die "could not mirror the images"
  ls -1t "$DIR"/c2-*.tar.zst.cms | tail -n +$((KEEP + 1)) | xargs -r rm -f
  echo "offsite-backup: $DIR/$NAME ($(stat -c %s "$DIR/$NAME") bytes), $(ls "$DIR"/c2-*.tar.zst.cms | wc -l) copies kept"
  ;;
list)
  ls -lt "$DIR"/c2-*.tar.zst.cms 2>/dev/null || echo "no copies in $DIR"
  du -sh "$DIR/images" 2>/dev/null || true
  ;;
restore)
  FILE=${2:?which file}; OUT=${3:?into which directory}
  [ -r "$KEY" ] || die "no private key at $KEY"
  [ -e "$OUT/state.json" ] && die "$OUT/state.json exists - restore into an empty directory"
  mkdir -p "$OUT"
  openssl cms -decrypt -binary -inform DER -in "$FILE" -inkey "$KEY" | zstd -dq | tar -C "$OUT" -xf -
  [ -d "$DIR/images" ] && cp -n "$DIR"/images/*.bin "$OUT/images/" 2>/dev/null || true
  echo "restored into $OUT - stop the server, put this in place of its data directory, chown to its user, start it"
  ;;
*)
  sed -n '2,25p' "$0"; exit 2 ;;
esac
