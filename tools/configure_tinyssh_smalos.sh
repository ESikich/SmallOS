#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "usage: $0 tinyssh-source-dir" >&2
  exit 2
fi

src=$1

define_feature() {
  macro=$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')
  printf '#define %s 1\n' "$macro" > "$src/$1.h"
}

undef_feature() {
  macro=$(printf '%s' "$1" | tr '[:lower:]' '[:upper:]')
  printf '#undef %s\n' "$macro" > "$src/$1.h"
}

undef_feature haslib1305
undef_feature haslib25519
undef_feature haslibntruprime
undef_feature haslibrandombytes
undef_feature haslibutilh
define_feature haslimits
define_feature haslogintty
undef_feature hasmlock
define_feature hasopenpty
define_feature hasutilh
undef_feature hasutmp
undef_feature hasutmpaddrv6
undef_feature hasutmphost
undef_feature hasutmploginlogout
undef_feature hasutmplogwtmp
undef_feature hasutmpname
undef_feature hasutmppid
undef_feature hasutmptime
undef_feature hasutmptv
undef_feature hasutmptype
undef_feature hasutmpuser
undef_feature hasutmpx
undef_feature hasutmpxaddrv6
undef_feature hasutmpxsyslen
undef_feature hasutmpxupdwtmpx
undef_feature hasvalgrind
